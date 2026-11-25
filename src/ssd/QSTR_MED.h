#ifndef QSTR_MED_H
#define QSTR_MED_H

#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../nvm_chip/flash_memory/Block_Latency_Profile.h"
#include "../utils/XMLWriter.h"

namespace SSD_Components
{
	class Block_Pool_Slot_Type;
	class Flash_Block_Manager_Base;

	class QSTR_MED
	{
	public:
		using Address = NVM::FlashMemory::Physical_Page_Address;
		using Profile = NVM::FlashMemory::Block_Latency_Profile;
		enum class Destination { HOST, GC, MAPPING };

		QSTR_MED(Flash_Block_Manager_Base& block_manager, const std::shared_ptr<const Profile>& profile,
			unsigned int pages_per_wordline, std::vector<std::vector<Address>> stream_planes);

		Block_Pool_Slot_Type* Take_frontier(stream_id_type stream, const Address& lane, Destination destination);
		void Observe_program(const Address& address);
		void Clear_open_block_observations(const Address& address);
		void Report_results_in_XML(Utils::XmlWriter& xmlwriter) const;

	private:
		struct Signature
		{
			sim_time_type program_sum;
			std::vector<uint64_t> bits;
		};

		struct Observation
		{
			std::vector<sim_time_type> program_times;
			std::size_t completed_pages = 0;
		};

		struct Member
		{
			Address address;
			Block_Pool_Slot_Type* block;
		};

		static Signature make_signature(const std::vector<sim_time_type>& program_times);
		const Signature& signature(const Address& address);
		std::vector<Member> candidates(const Address& lane, bool slow, std::size_t limit);
		std::vector<Member> assemble(stream_id_type stream, bool slow);
		void record_group_cost(const std::vector<Member>& members);
		void release_group_holding(std::size_t block_index);

		Flash_Block_Manager_Base& block_manager;
		std::shared_ptr<const Profile> profile;
		unsigned int pages_per_wordline;
		std::vector<std::vector<Address>> stream_planes;
		Signature default_signature;
		std::unordered_map<std::size_t, Signature> signatures;
		std::unordered_map<std::size_t, Observation> observations;
		std::vector<std::array<std::vector<Member>, 2>> pending_groups;
		std::unordered_set<std::size_t> reserved_blocks;
		uint64_t fast_groups = 0, slow_groups = 0, complete_groups = 0, cancelled_groups = 0;
		uint64_t distance_checks = 0, updated_blocks = 0, fallback_allocations = 0;
		uint64_t extra_program_time = 0, extra_erase_time = 0;
	};
}

#endif
