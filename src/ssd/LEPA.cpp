#include "LEPA.h"
#include "NVM_PHY_ONFI.h"

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace SSD_Components
{
	namespace
	{
		uint64_t latency_ratio(sim_time_type latency, sim_time_type read_time)
		{
			if (read_time == 0) {
				throw std::invalid_argument("LEPA requires a positive NAND read latency");
			}
			const auto remainder = latency % read_time;
			return std::max<uint64_t>(1, latency / read_time + (remainder >= read_time - remainder));
		}
	}

	LEPA::LEPA(unsigned int channels, unsigned int chips_per_channel, unsigned int dies_per_chip,
		unsigned int planes_per_die, sim_time_type read_time, sim_time_type program_time,
		sim_time_type erase_time, bool independent_wordlines, NVM_PHY_ONFI& controller, bool wordline_aware)
		: chips_per_channel(chips_per_channel), dies_per_chip(dies_per_chip), planes_per_die(planes_per_die),
		read_time(read_time), write_time_ratio(latency_ratio(program_time, read_time)),
		erase_time_ratio(latency_ratio(erase_time, read_time)), independent_wordlines(independent_wordlines),
		controller(controller), wordline_aware(wordline_aware),
		dies(static_cast<std::size_t>(channels) * chips_per_channel * dies_per_chip)
	{
		for (auto& die : dies) {
			die.reads.resize(planes_per_die);
			die.erases.resize(planes_per_die);
		}
	}

	std::size_t LEPA::die_index(const NVM::FlashMemory::Physical_Page_Address& address) const
	{
		return (static_cast<std::size_t>(address.ChannelID) * chips_per_channel + address.ChipID)
			* dies_per_chip + address.DieID;
	}

	uint64_t LEPA::die_load(const Die& die) const
	{
		if (!independent_wordlines) {
			// In MQSim's single-plane mode all queued transactions consume a NAND operation slot.
			return std::accumulate(die.reads.begin(), die.reads.end(), uint64_t(0))
				+ die.writes * write_time_ratio
				+ std::accumulate(die.erases.begin(), die.erases.end(), uint64_t(0)) * erase_time_ratio;
		}
		// Wu et al., equations (2)-(5). Incomplete write stripes can accept the next page.
		const auto read_load = *std::max_element(die.reads.begin(), die.reads.end());
		const auto stripes = (die.writes + planes_per_die - 1 - die.next_write_plane) / planes_per_die;
		const auto erase_stripes = *std::max_element(die.erases.begin(), die.erases.end());
		return read_load + stripes * write_time_ratio + erase_stripes * erase_time_ratio;
	}

	sim_time_type LEPA::program_time(const NVM::FlashMemory::Physical_Page_Address& address) const
	{
		return controller.Get_chip(address.ChannelID, address.ChipID)->Get_configured_program_latency(address.PageID);
	}

	uint64_t LEPA::Write_allocation_cost(const NVM::FlashMemory::Physical_Page_Address& address) const
	{
		const auto& die = dies[die_index(address)];
		if (!wordline_aware) {
			return die_load(die);
		}
		const auto reads = std::accumulate(die.reads.begin(), die.reads.end(), uint64_t(0));
		const auto erases = std::accumulate(die.erases.begin(), die.erases.end(), uint64_t(0));
		// Preserve LEPA's read and erase weights, with individual program costs in nanoseconds.
		return (reads + erases * erase_time_ratio) * read_time + die.outstanding_program_time + program_time(address);
	}

	uint64_t LEPA::Redirection_threshold(const NVM::FlashMemory::Physical_Page_Address& address) const
	{
		return wordline_aware ? program_time(address) : write_time_ratio;
	}

	unsigned int LEPA::Next_write_plane(const NVM::FlashMemory::Physical_Page_Address& address) const
	{
		return dies[die_index(address)].next_write_plane;
	}

	void LEPA::Record_selection(bool redirected)
	{
		++selections;
		redirected_selections += redirected;
	}

	void LEPA::Allocate_write(const NVM_Transaction_Flash& transaction)
	{
		if (!allocated_writes.insert(&transaction).second) {
			throw std::logic_error("LEPA write was allocated twice");
		}
		auto& die = dies[die_index(transaction.Address)];
		die.next_write_plane = (transaction.Address.PlaneID + 1) % planes_per_die;
		add_outstanding(die, transaction);
	}

	uint64_t& LEPA::command_count(Die& die, const NVM_Transaction_Flash& transaction)
	{
		switch (transaction.Type) {
		case Transaction_Type::READ:
			return die.reads[transaction.Address.PlaneID];
		case Transaction_Type::WRITE:
			return die.writes;
		case Transaction_Type::ERASE:
			return die.erases[transaction.Address.PlaneID];
		default:
			throw std::logic_error("Unexpected NAND transaction type in LEPA");
		}
	}

	void LEPA::add_outstanding(Die& die, const NVM_Transaction_Flash& transaction)
	{
		++command_count(die, transaction);
		if (wordline_aware && transaction.Type == Transaction_Type::WRITE) {
			die.outstanding_program_time += program_time(transaction.Address);
		}
	}

	void LEPA::Submit(const NVM_Transaction_Flash& transaction)
	{
		if (transaction.Type != Transaction_Type::WRITE || allocated_writes.erase(&transaction) == 0) {
			add_outstanding(dies[die_index(transaction.Address)], transaction);
		}
	}

	void LEPA::Complete(const NVM_Transaction_Flash& transaction)
	{
		auto& die = dies[die_index(transaction.Address)];
		auto& count = command_count(die, transaction);
		const auto program = wordline_aware && transaction.Type == Transaction_Type::WRITE ? program_time(transaction.Address) : 0;
		if (count == 0 || die.outstanding_program_time < program) {
			throw std::logic_error("LEPA observed completion of an uncounted NAND transaction");
		}
		--count;
		die.outstanding_program_time -= program;
	}

	void LEPA::Report_results_in_XML(Utils::XmlWriter& xmlwriter) const
	{
		uint64_t reads = 0, writes = 0, erases = 0;
		sim_time_type outstanding_program_time = 0;
		for (const auto& die : dies) {
			reads += std::accumulate(die.reads.begin(), die.reads.end(), uint64_t(0));
			writes += die.writes;
			erases += std::accumulate(die.erases.begin(), die.erases.end(), uint64_t(0));
			outstanding_program_time += die.outstanding_program_time;
		}
		xmlwriter.Write_open_tag("LEPA");
		xmlwriter.Write_attribute_string("Independent_Wordline_Multiplane", independent_wordlines ? "true" : "false");
		xmlwriter.Write_attribute_string("Read_Unit_ns", std::to_string(read_time));
		xmlwriter.Write_attribute_string("Write_Time_Ratio", std::to_string(write_time_ratio));
		xmlwriter.Write_attribute_string("Erase_Time_Ratio", std::to_string(erase_time_ratio));
		xmlwriter.Write_attribute_string("Write_Selections", std::to_string(selections));
		xmlwriter.Write_attribute_string("Die_Redirections", std::to_string(redirected_selections));
		xmlwriter.Write_attribute_string("Outstanding_Reads", std::to_string(reads));
		xmlwriter.Write_attribute_string("Outstanding_Writes", std::to_string(writes));
		xmlwriter.Write_attribute_string("Outstanding_Erases", std::to_string(erases));
		if (wordline_aware) {
			xmlwriter.Write_attribute_string("WL_Aware", "true");
			xmlwriter.Write_attribute_string("Program_Latency_Source", "CONFIGURED");
			xmlwriter.Write_attribute_string("Outstanding_Program_Time_ns", std::to_string(outstanding_program_time));
		}
		xmlwriter.Write_close_tag();
	}
}
