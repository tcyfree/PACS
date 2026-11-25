#ifndef LEPA_H
#define LEPA_H

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "NVM_Transaction_Flash.h"
#include "../utils/XMLWriter.h"

namespace SSD_Components
{
	class NVM_PHY_ONFI;

	class LEPA
	{
	public:
		LEPA(unsigned int channels, unsigned int chips_per_channel, unsigned int dies_per_chip,
			unsigned int planes_per_die, sim_time_type read_time, sim_time_type program_time,
			sim_time_type erase_time, bool independent_wordlines, NVM_PHY_ONFI& controller, bool wordline_aware);

		bool Wordline_aware() const { return wordline_aware; }
		uint64_t Write_allocation_cost(const NVM::FlashMemory::Physical_Page_Address& address) const;
		uint64_t Redirection_threshold(const NVM::FlashMemory::Physical_Page_Address& address) const;
		unsigned int Next_write_plane(const NVM::FlashMemory::Physical_Page_Address& address) const;
		void Record_selection(bool redirected);
		void Allocate_write(const NVM_Transaction_Flash& transaction);
		void Submit(const NVM_Transaction_Flash& transaction);
		void Complete(const NVM_Transaction_Flash& transaction);
		void Report_results_in_XML(Utils::XmlWriter& xmlwriter) const;

	private:
		struct Die
		{
			std::vector<uint64_t> reads;
			std::vector<uint64_t> erases;
			uint64_t writes = 0;
			sim_time_type outstanding_program_time = 0;
			unsigned int next_write_plane = 0;
		};

		std::size_t die_index(const NVM::FlashMemory::Physical_Page_Address& address) const;
		uint64_t die_load(const Die& die) const;
		sim_time_type program_time(const NVM::FlashMemory::Physical_Page_Address& address) const;
		uint64_t& command_count(Die& die, const NVM_Transaction_Flash& transaction);
		void add_outstanding(Die& die, const NVM_Transaction_Flash& transaction);

		unsigned int chips_per_channel, dies_per_chip, planes_per_die;
		sim_time_type read_time;
		uint64_t write_time_ratio, erase_time_ratio;
		bool independent_wordlines;
		NVM_PHY_ONFI& controller;
		const bool wordline_aware;
		std::vector<Die> dies;
		// Several pages of a request may be placed before the TSU receives any of them.
		std::unordered_set<const NVM_Transaction_Flash*> allocated_writes;
		uint64_t selections = 0;
		uint64_t redirected_selections = 0;
	};
}

#endif
