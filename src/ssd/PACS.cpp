#include <algorithm>
#include <cmath>
#include <stdexcept>
#include "PACS.h"

namespace SSD_Components
{
	PACS::PACS(unsigned int channels, unsigned int chips_per_channel, unsigned int dies_per_chip, bool request_level,
		bool cross_channel, PACS_Adaptive_Options adaptive)
		: chips_per_channel(chips_per_channel), dies_per_chip(dies_per_chip), request_level(request_level), cross_channel(cross_channel),
		adaptive(adaptive),
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
		return Estimate_completion_cost(address, Estimate_program_latency(address, page_type));
	}

	double PACS::Estimate_completion_cost(const NVM::FlashMemory::Physical_Page_Address& address, double program_latency) const
	{
		return (1.0 + dies[die_index(address)].outstanding_programs) * program_latency;
	}

	double PACS::Estimate_program_latency(const NVM::FlashMemory::Physical_Page_Address& address, unsigned int page_type) const
	{
		const auto& profile = dies[die_index(address)].profiles[page_type];
		return profile.samples >= minimum_samples ? profile.estimated_latency : 0;
	}

	void PACS::Record_ranked_allocation(bool redirected)
	{
		++ranked_allocations;
		redirected_allocations += redirected;
	}

	void PACS::Record_request_plan(std::size_t redirected_pages, std::size_t crossed_pages)
	{
		++planned_requests;
		redirected_requests += redirected_pages != 0;
		redirected_request_pages += redirected_pages;
		cross_channel_requests += crossed_pages != 0;
		cross_channel_pages += crossed_pages;
		joint_cross_channel_requests += crossed_pages > 1;
	}

	void PACS::Record_adaptive_search(std::size_t critical_pages, unsigned int evaluations, bool budget_exhausted)
	{
		++critical_page_counts[critical_pages];
		++search_evaluation_counts[evaluations];
		search_budget_exhaustions += budget_exhausted;
	}

	void PACS::Track_request_prediction(const User_Request* request, std::size_t pages, sim_time_type start,
		double estimated_duration, bool crossed_channel)
	{
		if (!request_predictions.emplace(request, Request_Prediction{pages, start, estimated_duration, crossed_channel}).second) {
			throw std::logic_error("PACS already tracks a completion prediction for this request");
		}
	}

	void PACS::Observe_request_page_completion(const User_Request* request, sim_time_type finish)
	{
		const auto entry = request_predictions.find(request);
		if (entry == request_predictions.end() || --entry->second.pending_pages != 0) {
			return;
		}
		const auto& prediction = entry->second;
		const auto duration = finish - prediction.start;
		prediction_error.Observe(prediction.estimated_duration, duration);
		if (prediction.crossed_channel) {
			cross_channel_prediction_error.Observe(prediction.estimated_duration, duration);
		}
		request_predictions.erase(entry);
	}

	void PACS::Prediction_Error::Observe(double estimated, sim_time_type actual)
	{
		const double error = estimated - actual;
		++samples;
		signed_error += error;
		absolute_error += std::abs(error);
		actual_duration += actual;
	}

	void PACS::Prediction_Error::Report(Utils::XmlWriter& xmlwriter, const char* tag) const
	{
		xmlwriter.Write_open_tag(tag);
		xmlwriter.Write_attribute_string("Samples", std::to_string(samples));
		if (samples) {
			xmlwriter.Write_attribute_string("Mean_Signed_Error_ns", std::to_string(signed_error / samples));
			xmlwriter.Write_attribute_string("Mean_Absolute_Error_ns", std::to_string(absolute_error / samples));
			xmlwriter.Write_attribute_string("Mean_Actual_Duration_ns", std::to_string(actual_duration / samples));
		}
		xmlwriter.Write_close_tag();
	}

	void PACS::Report_results_in_XML(Utils::XmlWriter& xmlwriter) const
	{
		uint64_t outstanding_programs = 0;
		for (const auto& die : dies) {
			outstanding_programs += die.outstanding_programs;
		}
		xmlwriter.Write_open_tag("PACS");
		if (Uses_online_program_profiling()) {
			xmlwriter.Write_attribute_string("Median_Batch_Size", std::to_string(batch_size));
			xmlwriter.Write_attribute_string("Minimum_Samples", std::to_string(minimum_samples));
			xmlwriter.Write_attribute_string("EWMA_Alpha", std::to_string(alpha));
		} else {
			xmlwriter.Write_attribute_string("Program_Latency_Source", "CONFIGURED");
		}
		xmlwriter.Write_attribute_string("Qualified_Program_Observations", std::to_string(qualified_observations));
		xmlwriter.Write_attribute_string("Discarded_Program_Observations", std::to_string(discarded_observations));
		xmlwriter.Write_attribute_string("Ranked_Write_Allocations", std::to_string(ranked_allocations));
		xmlwriter.Write_attribute_string("Redirected_Write_Allocations", std::to_string(redirected_allocations));
		xmlwriter.Write_attribute_string("Outstanding_Programs_At_End", std::to_string(outstanding_programs));
		if (request_level) {
			xmlwriter.Write_attribute_string("Planned_Multipage_Write_Requests", std::to_string(planned_requests));
			xmlwriter.Write_attribute_string("Redirected_Multipage_Write_Requests", std::to_string(redirected_requests));
			xmlwriter.Write_attribute_string("Redirected_Multipage_Write_Pages", std::to_string(redirected_request_pages));
		}
		if (cross_channel) {
			xmlwriter.Write_attribute_string("Cross_Channel_Multipage_Write_Requests", std::to_string(cross_channel_requests));
		}
		if (adaptive.enabled) {
			xmlwriter.Write_attribute_string("Cross_Channel_Write_Pages", std::to_string(cross_channel_pages));
			xmlwriter.Write_attribute_string("Joint_Cross_Channel_Multipage_Write_Requests", std::to_string(joint_cross_channel_requests));
			xmlwriter.Write_attribute_string("Open_Request_Predictions_At_End", std::to_string(request_predictions.size()));
			xmlwriter.Write_open_tag("Adaptive_Search");
			xmlwriter.Write_attribute_string("Gain_Beta", std::to_string(adaptive.gain_beta));
			xmlwriter.Write_attribute_string("Search_Budget", std::to_string(adaptive.search_budget));
			xmlwriter.Write_attribute_string("Budget_Exhausted_Requests", std::to_string(search_budget_exhaustions));
			for (const auto& entry : critical_page_counts) {
				xmlwriter.Write_open_tag("Critical_Page_Count");
				xmlwriter.Write_attribute_string("Pages", std::to_string(entry.first));
				xmlwriter.Write_attribute_string("Requests", std::to_string(entry.second));
				xmlwriter.Write_close_tag();
			}
			for (const auto& entry : search_evaluation_counts) {
				xmlwriter.Write_open_tag("Candidate_Plan_Count");
				xmlwriter.Write_attribute_string("Evaluations", std::to_string(entry.first));
				xmlwriter.Write_attribute_string("Requests", std::to_string(entry.second));
				xmlwriter.Write_close_tag();
			}
			xmlwriter.Write_close_tag();
			prediction_error.Report(xmlwriter, "Request_Completion_Prediction");
			cross_channel_prediction_error.Report(xmlwriter, "Cross_Channel_Completion_Prediction");
		}
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
