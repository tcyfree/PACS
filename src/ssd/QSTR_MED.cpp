#include "QSTR_MED.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

#include "Flash_Block_Manager_Base.h"

namespace SSD_Components
{
	namespace
	{
		constexpr std::size_t candidate_window = 4;
		constexpr std::size_t strings_per_layer = 4;

		bool same_plane(const QSTR_MED::Address& lhs, const QSTR_MED::Address& rhs)
		{
			return lhs.ChannelID == rhs.ChannelID && lhs.ChipID == rhs.ChipID
				&& lhs.DieID == rhs.DieID && lhs.PlaneID == rhs.PlaneID;
		}

		unsigned int hamming_distance(const std::vector<uint64_t>& lhs, const std::vector<uint64_t>& rhs)
		{
			unsigned int distance = 0;
			for (std::size_t i = 0; i < lhs.size(); ++i) {
				distance += __builtin_popcountll(lhs[i] ^ rhs[i]);
			}
			return distance;
		}
	}

	QSTR_MED::QSTR_MED(Flash_Block_Manager_Base& block_manager, const std::shared_ptr<const Profile>& profile,
		unsigned int pages_per_wordline, std::vector<std::vector<Address>> stream_planes)
		: block_manager(block_manager), profile(profile), pages_per_wordline(pages_per_wordline),
		stream_planes(std::move(stream_planes)), default_signature(make_signature(profile->Default_program_times())),
		pending_groups(this->stream_planes.size())
	{
	}

	QSTR_MED::Signature QSTR_MED::make_signature(const std::vector<sim_time_type>& program_times)
	{
		Signature result{std::accumulate(program_times.begin(), program_times.end(), sim_time_type(0)),
			std::vector<uint64_t>((program_times.size() + 63) / 64)};
		for (std::size_t start = 0; start < program_times.size(); start += strings_per_layer) {
			std::array<std::size_t, strings_per_layer> strings;
			const auto count = std::min(strings_per_layer, program_times.size() - start);
			std::iota(strings.begin(), strings.begin() + count, start);
			std::sort(strings.begin(), strings.begin() + count, [&](std::size_t lhs, std::size_t rhs) {
				return program_times[lhs] == program_times[rhs] ? lhs < rhs : program_times[lhs] < program_times[rhs];
			});
			for (std::size_t rank = (count + 1) / 2; rank < count; ++rank) {
				const auto wordline = strings[rank];
				result.bits[wordline / 64] |= uint64_t(1) << (wordline % 64);
			}
		}
		return result;
	}

	const QSTR_MED::Signature& QSTR_MED::signature(const Address& address)
	{
		const auto index = profile->Block_index(address);
		const auto entry = signatures.find(index);
		if (entry != signatures.end()) {
			return entry->second;
		}
		const auto& times = profile->Program_times(address);
		if (&times == &profile->Default_program_times()) {
			return default_signature;
		}
		return signatures.emplace(index, make_signature(times)).first->second;
	}

	std::vector<QSTR_MED::Member> QSTR_MED::candidates(const Address& lane, bool slow, std::size_t limit)
	{
		const auto& pool = block_manager.Get_plane_bookkeeping_entry(lane)->Free_block_pool;
		std::vector<Member> available;
		available.reserve(pool.size());
		for (const auto& entry : pool) {
			auto address = lane;
			address.BlockID = entry.second->BlockID;
			if (reserved_blocks.count(profile->Block_index(address)) == 0) {
				available.push_back({address, entry.second});
			}
		}
		const auto count = std::min(limit, available.size());
		std::partial_sort(available.begin(), available.begin() + count, available.end(), [&](const Member& lhs, const Member& rhs) {
			const auto lhs_time = signature(lhs.address).program_sum;
			const auto rhs_time = signature(rhs.address).program_sum;
			if (lhs_time == rhs_time) {
				return lhs.address.BlockID < rhs.address.BlockID;
			}
			return slow ? lhs_time > rhs_time : lhs_time < rhs_time;
		});
		available.resize(count);
		return available;
	}

	std::vector<QSTR_MED::Member> QSTR_MED::assemble(stream_id_type stream, bool slow)
	{
		std::vector<std::vector<Member>> windows;
		std::size_t reference_lane = 0;
		for (const auto& lane : stream_planes[stream]) {
			auto window = candidates(lane, slow, candidate_window);
			if (window.empty()) {
				return {};
			}
			if (!windows.empty()) {
				const auto candidate_time = signature(window.front().address).program_sum;
				const auto reference_time = signature(windows[reference_lane].front().address).program_sum;
				if (slow ? candidate_time > reference_time : candidate_time < reference_time) {
					reference_lane = windows.size();
				}
			}
			windows.push_back(std::move(window));
		}
		if (windows.empty()) {
			return {};
		}

		const auto& reference = signature(windows[reference_lane].front().address).bits;
		std::vector<Member> assembled;
		assembled.push_back(windows[reference_lane].front());
		for (std::size_t lane = 0; lane < windows.size(); ++lane) {
			if (lane == reference_lane) {
				continue;
			}
			const Member* closest = nullptr;
			auto minimum_distance = std::numeric_limits<unsigned int>::max();
			for (const auto& candidate : windows[lane]) {
				const auto distance = hamming_distance(reference, signature(candidate.address).bits);
				++distance_checks;
				if (distance < minimum_distance) {
					minimum_distance = distance;
					closest = &candidate;
				}
			}
			assembled.push_back(*closest);
		}

		for (const auto& member : assembled) {
			reserved_blocks.insert(profile->Block_index(member.address));
		}
		if (slow) {
			++slow_groups;
		} else {
			++fast_groups;
		}
		record_group_cost(assembled);
		return assembled;
	}

	void QSTR_MED::record_group_cost(const std::vector<Member>& members)
	{
		if (profile->Override_count() == 0) {
			return;
		}
		auto min_erase = std::numeric_limits<sim_time_type>::max();
		sim_time_type max_erase = 0;
		const auto wordlines = profile->Default_program_times().size();
		std::vector<sim_time_type> min_program(wordlines, min_erase), max_program(wordlines, 0);
		for (const auto& member : members) {
			const auto erase = profile->Erase_time(member.address);
			min_erase = std::min(min_erase, erase);
			max_erase = std::max(max_erase, erase);
			const auto& program = profile->Program_times(member.address);
			for (std::size_t wl = 0; wl < wordlines; ++wl) {
				min_program[wl] = std::min(min_program[wl], program[wl]);
				max_program[wl] = std::max(max_program[wl], program[wl]);
			}
		}
		extra_erase_time += max_erase - min_erase;
		for (std::size_t wl = 0; wl < wordlines; ++wl) {
			extra_program_time += max_program[wl] - min_program[wl];
		}
	}

	void QSTR_MED::release_group_holding(std::size_t index)
	{
		for (auto& plans : pending_groups) {
			for (auto& plan : plans) {
				const auto holder = std::find_if(plan.begin(), plan.end(), [&](const Member& member) {
					return profile->Block_index(member.address) == index;
				});
				if (holder == plan.end()) {
					continue;
				}
				for (const auto& member : plan) {
					reserved_blocks.erase(profile->Block_index(member.address));
				}
				plan.clear();
				++cancelled_groups;
				return;
			}
		}
		throw std::logic_error("QSTR-MED could not locate a reserved block");
	}

	Block_Pool_Slot_Type* QSTR_MED::Take_frontier(stream_id_type stream, const Address& lane, Destination destination)
	{
		auto* plane = block_manager.Get_plane_bookkeeping_entry(lane);
		auto& pool = plane->Free_block_pool;
		const auto claim = [&](Block_Pool_Slot_Type* block) {
			const auto iterator = std::find_if(pool.begin(), pool.end(), [&](const std::pair<const unsigned int, Block_Pool_Slot_Type*>& entry) {
				return entry.second == block;
			});
			return plane->Take_free_block(iterator, stream, destination == Destination::MAPPING);
		};

		if (destination != Destination::MAPPING) {
			const bool slow = destination == Destination::GC;
			auto& plan = pending_groups[stream][slow ? 1 : 0];
			if (plan.empty()) {
				plan = assemble(stream, slow);
			}
			const auto member = std::find_if(plan.begin(), plan.end(), [&](const Member& entry) {
				return same_plane(entry.address, lane);
			});
			if (member != plan.end()) {
				auto* block = claim(member->block);
				reserved_blocks.erase(profile->Block_index(member->address));
				plan.erase(member);
				if (plan.empty()) {
					++complete_groups;
				}
				return block;
			}
			++fallback_allocations;
			// A plane may fill faster than its peers. Use a local block without consuming their reservations.
			const auto local = candidates(lane, slow, 1);
			if (!local.empty()) {
				return claim(local.front().block);
			}
		}

		for (auto it = pool.begin(); it != pool.end(); ++it) {
			auto address = lane;
			address.BlockID = it->second->BlockID;
			if (reserved_blocks.count(profile->Block_index(address)) == 0) {
				return plane->Take_free_block(it, stream, destination == Destination::MAPPING);
			}
		}
		if (!pool.empty()) {
			auto address = lane;
			address.BlockID = pool.begin()->second->BlockID;
			release_group_holding(profile->Block_index(address));
		}
		return plane->Get_a_free_block(stream, destination == Destination::MAPPING);
	}

	void QSTR_MED::Observe_program(const Address& address)
	{
		const auto index = profile->Block_index(address);
		auto& block = observations[index];
		const auto& times = profile->Program_times(address);
		if (block.program_times.empty()) {
			block.program_times.resize(times.size());
		}
		const auto wordline = address.PageID / pages_per_wordline;
		// Use the simulated per-address NAND time when that page completes.
		block.program_times[wordline] = times[wordline];
		++block.completed_pages;
		if (block.completed_pages == times.size() * pages_per_wordline) {
			signatures[index] = make_signature(block.program_times);
			observations.erase(index);
			++updated_blocks;
		}
	}

	void QSTR_MED::Clear_open_block_observations(const Address& address)
	{
		observations.erase(profile->Block_index(address));
	}

	void QSTR_MED::Report_results_in_XML(Utils::XmlWriter& xmlwriter) const
	{
		xmlwriter.Write_open_tag("QSTR_MED");
		xmlwriter.Write_attribute_string("Profiled_Blocks", std::to_string(profile->Override_count()));
		xmlwriter.Write_attribute_string("Fast_Superblocks", std::to_string(fast_groups));
		xmlwriter.Write_attribute_string("Slow_Superblocks", std::to_string(slow_groups));
		xmlwriter.Write_attribute_string("Fully_Allocated_Superblocks", std::to_string(complete_groups));
		xmlwriter.Write_attribute_string("Cancelled_Superblocks", std::to_string(cancelled_groups));
		xmlwriter.Write_attribute_string("Local_Fallback_Allocations", std::to_string(fallback_allocations));
		xmlwriter.Write_attribute_string("Hamming_Distance_Checks", std::to_string(distance_checks));
		xmlwriter.Write_attribute_string("Updated_Block_Signatures", std::to_string(updated_blocks));
		xmlwriter.Write_attribute_string("Extra_Superblock_Program_Time_ns", std::to_string(extra_program_time));
		xmlwriter.Write_attribute_string("Extra_Superblock_Erase_Time_ns", std::to_string(extra_erase_time));
		xmlwriter.Write_close_tag();
	}
}
