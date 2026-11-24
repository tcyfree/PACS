#ifndef PACS_H
#define PACS_H

#include <array>
#include <vector>
#include "../sim/Sim_Defs.h"
#include "../nvm_chip/flash_memory/Physical_Page_Address.h"
#include "../utils/XMLWriter.h"

namespace SSD_Components
{
	class PACS
	{
	public:
		PACS(unsigned int channels, unsigned int chips_per_channel, unsigned int dies_per_chip);
		void Queue_program(const NVM::FlashMemory::Physical_Page_Address& address);
		void Complete_program(const NVM::FlashMemory::Physical_Page_Address& address);
		void Observe_program(const NVM::FlashMemory::Physical_Page_Address& address, unsigned int page_type,
			sim_time_type program_start, sim_time_type ready_time);
		double Estimate_completion_cost(const NVM::FlashMemory::Physical_Page_Address& address, unsigned int page_type) const;
		void Record_ranked_allocation(bool redirected);
		void Report_results_in_XML(Utils::XmlWriter& xmlwriter) const;

	private:
		static constexpr unsigned int batch_size = 9;
		static constexpr unsigned int minimum_samples = 36;
		static constexpr double alpha = 0.25;
		static constexpr unsigned int max_page_types = 3;

		struct Profile {
			std::array<sim_time_type, batch_size> batch;
			unsigned int batch_count = 0;
			uint64_t samples = 0;
			double estimated_latency = 0;
		};
		struct Die {
			std::array<Profile, max_page_types> profiles;
			unsigned int outstanding_programs = 0;
		};
		std::size_t die_index(const NVM::FlashMemory::Physical_Page_Address& address) const;

		const unsigned int chips_per_channel, dies_per_chip;
		std::vector<Die> dies;
		uint64_t qualified_observations = 0;
		uint64_t discarded_observations = 0;
		uint64_t ranked_allocations = 0;
		uint64_t redirected_allocations = 0;
	};
}

#endif
