#include <algorithm>
#include <stdexcept>
#include "PACS.h"

namespace SSD_Components
{
	PACS::PACS(unsigned int channels, unsigned int chips_per_channel, unsigned int dies_per_chip)
		: chips_per_channel(chips_per_channel), dies_per_chip(dies_per_chip),
		dies(static_cast<std::size_t>(channels) * chips_per_channel * dies_per_chip)
	{
	}

	std::size_t PACS::die_index(const NVM::FlashMemory::Physical_Page_Address& address) const
	{
		return (static_cast<std::size_t>(address.ChannelID) * chips_per_channel + address.ChipID)
			* dies_per_chip + address.DieID;
	}

	void PACS::Queue_program(const NVM::FlashMemory::Physical_Page_Address& address)
	{
		++dies[die_index(address)].outstanding_programs;
	}

	void PACS::Complete_program(const NVM::FlashMemory::Physical_Page_Address& address)
	{
		auto& outstanding = dies[die_index(address)].outstanding_programs;
		if (outstanding == 0) {
			throw std::logic_error("PACS observed a program completion without a queued program");
		}
		--outstanding;
	}

	void PACS::Observe_program(const NVM::FlashMemory::Physical_Page_Address& address, unsigned int page_type,
		sim_time_type program_start, sim_time_type ready_time)
	{
		if (program_start == INVALID_TIME || ready_time <= program_start) {
			++discarded_observations;
			return;
		}
		++qualified_observations;
		auto& profile = dies[die_index(address)].profiles[page_type];
		profile.batch[profile.batch_count++] = ready_time - program_start;
		if (profile.batch_count == batch_size) {
			auto median = profile.batch.begin() + batch_size / 2;
			std::nth_element(profile.batch.begin(), median, profile.batch.end());
			profile.estimated_latency = profile.samples == 0 ? *median
				: (1 - alpha) * profile.estimated_latency + alpha * *median;
			profile.samples += batch_size;
			profile.batch_count = 0;
		}
	}

	double PACS::Estimate_completion_cost(const NVM::FlashMemory::Physical_Page_Address& address, unsigned int page_type) const
	{
		const auto& die = dies[die_index(address)];
		const auto& profile = die.profiles[page_type];
		return profile.samples >= minimum_samples ? (1.0 + die.outstanding_programs) * profile.estimated_latency : 0;
	}

	void PACS::Record_ranked_allocation(bool redirected)
	{
		++ranked_allocations;
		redirected_allocations += redirected;
	}

	void PACS::Report_results_in_XML(Utils::XmlWriter& xmlwriter) const
	{
		uint64_t outstanding_programs = 0;
		for (const auto& die : dies) {
			outstanding_programs += die.outstanding_programs;
		}
		xmlwriter.Write_open_tag("PACS");
		xmlwriter.Write_attribute_string("Median_Batch_Size", std::to_string(batch_size));
		xmlwriter.Write_attribute_string("Minimum_Samples", std::to_string(minimum_samples));
		xmlwriter.Write_attribute_string("EWMA_Alpha", std::to_string(alpha));
		xmlwriter.Write_attribute_string("Qualified_Program_Observations", std::to_string(qualified_observations));
		xmlwriter.Write_attribute_string("Discarded_Program_Observations", std::to_string(discarded_observations));
		xmlwriter.Write_attribute_string("Ranked_Write_Allocations", std::to_string(ranked_allocations));
		xmlwriter.Write_attribute_string("Redirected_Write_Allocations", std::to_string(redirected_allocations));
		xmlwriter.Write_attribute_string("Outstanding_Programs_At_End", std::to_string(outstanding_programs));
		for (unsigned int page_type = 0; page_type < max_page_types; ++page_type) {
			std::vector<double> estimates;
			for (const auto& die : dies) {
				const auto& profile = die.profiles[page_type];
				if (profile.samples >= minimum_samples) {
					estimates.push_back(profile.estimated_latency);
				}
			}
			if (!estimates.empty()) {
				std::sort(estimates.begin(), estimates.end());
				const auto count = estimates.size();
				const double reference = (estimates[(count - 1) / 2] + estimates[count / 2]) / 2;
				xmlwriter.Write_open_tag("Program_Class");
				xmlwriter.Write_attribute_string("Page_Type", std::to_string(page_type));
				xmlwriter.Write_attribute_string("Profiled_Dies", std::to_string(count));
				xmlwriter.Write_attribute_string("Reference_Latency_ns", std::to_string(reference));
				xmlwriter.Write_close_tag();
			}
		}
		xmlwriter.Write_close_tag();
	}
}
