#ifndef BLOCK_LATENCY_PROFILE_H
#define BLOCK_LATENCY_PROFILE_H

#include <string>
#include <unordered_map>
#include <vector>

#include "../../sim/Sim_Defs.h"
#include "Physical_Page_Address.h"

namespace NVM
{
	namespace FlashMemory
	{
		class Block_Latency_Profile
		{
		public:
			Block_Latency_Profile(unsigned int channels, unsigned int chips, unsigned int dies,
				unsigned int planes, unsigned int blocks, const std::vector<sim_time_type>& program,
				sim_time_type erase, const std::string& filename);

			std::size_t Block_index(const Physical_Page_Address& address) const;
			const std::vector<sim_time_type>& Program_times(const Physical_Page_Address& address) const;
			sim_time_type Erase_time(const Physical_Page_Address& address) const;
			const std::vector<sim_time_type>& Default_program_times() const { return default_program; }
			std::size_t Override_count() const { return blocks.size(); }

		private:
			struct Timings
			{
				sim_time_type erase;
				std::vector<sim_time_type> program;
			};

			unsigned int chips_per_channel, dies_per_chip, planes_per_die, blocks_per_plane;
			std::vector<sim_time_type> default_program;
			sim_time_type default_erase;
			std::unordered_map<std::size_t, Timings> blocks;
		};
	}
}

#endif
