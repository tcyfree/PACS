#include "Block_Latency_Profile.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace NVM
{
	namespace FlashMemory
	{
		namespace
		{
			sim_time_type read_time(std::istringstream& row)
			{
				std::string input;
				if (!(row >> input) || input.find_first_not_of("0123456789") != std::string::npos) {
					throw std::invalid_argument("expected an integer latency in nanoseconds");
				}
				const auto value = std::stoull(input);
				if (value == 0 || value == INVALID_TIME) {
					throw std::invalid_argument("invalid NAND latency");
				}
				return value;
			}
		}

		Block_Latency_Profile::Block_Latency_Profile(unsigned int channels, unsigned int chips, unsigned int dies,
			unsigned int planes, unsigned int block_count, const std::vector<sim_time_type>& program,
			sim_time_type erase, const std::string& filename)
			: chips_per_channel(chips), dies_per_chip(dies), planes_per_die(planes), blocks_per_plane(block_count),
			default_program(program), default_erase(erase)
		{
			if (default_program.empty()) {
				throw std::invalid_argument("Block latency profiles require Wordline_Program_Latencies");
			}
			if (filename.empty()) {
				return;
			}
			std::ifstream input(filename);
			if (!input) {
				throw std::invalid_argument("Cannot open block latency profile " + filename);
			}
			std::string line;
			unsigned int line_number = 0;
			while (std::getline(input, line)) {
				++line_number;
				std::istringstream row(line.substr(0, line.find('#')));
				row >> std::ws;
				if (row.eof()) {
					continue;
				}
				try {
					uint64_t channel, chip, die, plane, block;
					if (!(row >> channel >> chip >> die >> plane >> block) || channel >= channels || chip >= chips
						|| die >= dies || plane >= planes || block >= block_count) {
						throw std::invalid_argument("physical block address outside the SSD geometry");
					}
					Timings timings;
					timings.erase = read_time(row);
					timings.program.reserve(program.size());
					for (std::size_t wl = 0; wl < program.size(); ++wl) {
						timings.program.push_back(read_time(row));
					}
					std::string extra;
					if (row >> extra) {
						throw std::invalid_argument("too many wordline latencies");
					}
					const Physical_Page_Address address(channel, chip, die, plane, block, 0);
					if (!blocks.emplace(Block_index(address), std::move(timings)).second) {
						throw std::invalid_argument("duplicate physical block address");
					}
				} catch (const std::exception& error) {
					throw std::invalid_argument(filename + ":" + std::to_string(line_number) + ": " + error.what());
				}
			}
			if (input.bad()) {
				throw std::runtime_error("Cannot read block latency profile " + filename);
			}
		}

		std::size_t Block_Latency_Profile::Block_index(const Physical_Page_Address& address) const
		{
			return (((static_cast<std::size_t>(address.ChannelID) * chips_per_channel + address.ChipID)
				* dies_per_chip + address.DieID) * planes_per_die + address.PlaneID) * blocks_per_plane + address.BlockID;
		}

		const std::vector<sim_time_type>& Block_Latency_Profile::Program_times(const Physical_Page_Address& address) const
		{
			const auto entry = blocks.find(Block_index(address));
			return entry == blocks.end() ? default_program : entry->second.program;
		}

		sim_time_type Block_Latency_Profile::Erase_time(const Physical_Page_Address& address) const
		{
			const auto entry = blocks.find(Block_index(address));
			return entry == blocks.end() ? default_erase : entry->second.erase;
		}
	}
}
