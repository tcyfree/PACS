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
		return (1.0 + dies[die_index(address)].outstanding_programs) * Estimate_program_latency(address, page_type);
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

	void PACS::Record_single_page_plan(bool crossed_channel, bool changed_plane)
	{
		single_page_cross_channel_writes += crossed_channel;
		single_page_cross_plane_writes += changed_plane;
	}

	void PACS::Record_request_plan(std::size_t redirected_pages, std::size_t crossed_pages, std::size_t changed_planes)
	{
		++planned_requests;
		redirected_requests += redirected_pages != 0;
		redirected_request_pages += redirected_pages;
		cross_channel_requests += crossed_pages != 0;
		cross_channel_pages += crossed_pages;
		joint_cross_channel_requests += crossed_pages > 1;
		multipage_cross_plane_pages += changed_planes;
	}

	void PACS::Record_adaptive_search(std::size_t critical_pages, unsigned int evaluations, bool budget_exhausted)
	{
		++critical_page_counts[critical_pages];
		++search_evaluation_counts[evaluations];
		search_budget_exhaustions += budget_exhausted;
	}

	void PACS::Record_wl_decision(const PACS_Decision_Comparison& comparison)
	{
		auto& stats = wl_decisions[comparison.pages > 1];
		++stats.requests;
		stats.pages += comparison.pages;
		const bool mean_redirected = comparison.cwdp_to_mean != 0;
		const bool full_redirected = comparison.cwdp_to_full != 0;
		const bool changed = comparison.mean_to_full != 0;
		if (!mean_redirected) {
			++stats.categories[full_redirected ? 2 : 0];
		} else if (!changed) {
			++stats.categories[1];
		} else {
			++stats.categories[full_redirected ? 3 : 4];
		}
		stats.changed_requests += changed;
		stats.changed_pages += comparison.mean_to_full;
		stats.changed_lanes += comparison.mean_to_full_lanes;
		stats.non_wl_redirections += mean_redirected;
		stats.same_die_plane_changes += comparison.same_die_plane_changes;
		++stats.pages_changed_per_request[comparison.mean_to_full];
		stats.full_rejected += comparison.full_search.rejected_by_beta;
		stats.mean_rejected += comparison.mean_search.rejected_by_beta;
		stats.full_only_below_beta += comparison.full_search.only_below_beta;
		stats.mean_only_below_beta += comparison.mean_search.only_below_beta;
		stats.full_budget_exhausted += comparison.full_search.budget_exhausted;
		stats.mean_budget_exhausted += comparison.mean_search.budget_exhausted;
		stats.dynamic_gain.Observe(comparison.cwdp_finish - comparison.mean_plan_finish);
		stats.wl_gain.Observe(comparison.mean_plan_finish - comparison.full_finish);

		if (comparison.pages == 1 && full_redirected) {
			const double program_gain = comparison.single_page_program_gain;
			const double load_gain = comparison.cwdp_finish - comparison.full_finish - program_gain;
			stats.single_load_gain.Observe(load_gain);
			stats.single_program_gain.Observe(program_gain);
			const auto sign_index = [](double gain) { return gain > 0 ? 2 : gain < 0 ? 0 : 1; };
			++stats.single_gain_signs[sign_index(load_gain)][sign_index(program_gain)];
		}
	}

	void PACS::Record_wl_candidates(bool multipage, double program_gap, double load_gap, double original_program, double selected_program)
	{
		auto& stats = wl_decisions[multipage];
		stats.original_program.Observe(original_program);
		stats.selected_program.Observe(selected_program);
		stats.candidate_program_gap.Observe(program_gap);
		stats.candidate_load_gap.Observe(load_gap);
	}

	void PACS::Record_wl_plan_commit(bool multipage, bool matched)
	{
		wl_decisions[multipage].committed_plans += matched;
	}

	void PACS::Time_Summary::Observe(double value)
	{
		min = samples ? std::min(min, value) : value;
		max = samples ? std::max(max, value) : value;
		sum += value;
		++samples;
	}

	void PACS::Time_Summary::Report(Utils::XmlWriter& xmlwriter) const
	{
		xmlwriter.Write_attribute_string("Samples", std::to_string(samples));
		if (samples) {
			xmlwriter.Write_attribute_string("Min_ns", std::to_string(min));
			xmlwriter.Write_attribute_string("Mean_ns", std::to_string(sum / samples));
			xmlwriter.Write_attribute_string("Max_ns", std::to_string(max));
		}
	}

	void PACS::Time_Summary::Report(Utils::XmlWriter& xmlwriter, const char* tag) const
	{
		xmlwriter.Write_open_tag(tag);
		Report(xmlwriter);
		xmlwriter.Write_close_tag();
	}

	const std::array<double, PACS::gap_bin_count> PACS::gap_upper_ns = {{
		0, 1000, 5000, 10000, 25000, 50000, 100000, 200000, 300000, 460000, 500000, 750000,
		1000000, 1500000, 2000000, 5000000, 10000000, 20000000, 50000000, 100000000, 500000000, 1000000000,
	}};

	void PACS::Gap_Distribution::Observe(double gap)
	{
		times.Observe(gap);
		const auto bin = std::lower_bound(gap_upper_ns.begin(), gap_upper_ns.end(), gap);
		++counts[bin - gap_upper_ns.begin()];
	}

	void PACS::Gap_Distribution::Report(Utils::XmlWriter& xmlwriter, const char* tag) const
	{
		xmlwriter.Write_open_tag(tag);
		times.Report(xmlwriter);
		for (std::size_t i = 0; i < gap_upper_ns.size(); ++i) {
			xmlwriter.Write_open_tag("Bin");
			xmlwriter.Write_attribute_string("Upper_ns", std::to_string(static_cast<sim_time_type>(gap_upper_ns[i])));
			xmlwriter.Write_attribute_string("Samples", std::to_string(counts[i]));
			xmlwriter.Write_close_tag();
		}
		xmlwriter.Write_attribute_string("Above_Last_Bin", std::to_string(counts.back()));
		xmlwriter.Write_close_tag();
	}

	void PACS::WL_Decisions::Report(Utils::XmlWriter& xmlwriter, const char* tag) const
	{
		xmlwriter.Write_open_tag(tag);
		const auto count = [&xmlwriter](const char* label, uint64_t value) {
			xmlwriter.Write_attribute_string(label, std::to_string(value));
		};
		count("Compared_Requests", requests);
		count("Compared_Pages", pages);
		count("Committed_Plans", committed_plans);
		count("WL_Decision_Changed_Requests", changed_requests);
		count("WL_Decision_Changed_Pages", changed_pages);
		count("WL_Decision_Changed_Page_Lanes", changed_lanes);
		count("Non_WL_Redirections", non_wl_redirections);
		count("CWDP_Equals_NoWL_Equals_Full", categories[0]);
		count("Non_WL_Only_Redirections", categories[1]);
		count("WL_Triggered_Redirections", categories[2]);
		count("WL_Changed_Redirections", categories[3]);
		count("WL_Restored_CWDP", categories[4]);
		count("Same_Die_Plane_Redirections", same_die_plane_changes);
		count("Full_Rejected_By_Beta", full_rejected);
		count("NoWL_Rejected_By_Beta", mean_rejected);
		count("Full_Requests_Only_Below_Beta", full_only_below_beta);
		count("NoWL_Requests_Only_Below_Beta", mean_only_below_beta);
		count("Full_Budget_Exhausted_Requests", full_budget_exhausted);
		count("NoWL_Budget_Exhausted_Requests", mean_budget_exhausted);
		candidate_program_gap.Report(xmlwriter, "Candidate_WL_Latency_Gap");
		candidate_load_gap.Report(xmlwriter, "Candidate_Load_Gap");
		original_program.Report(xmlwriter, "Original_Program_Latency");
		selected_program.Report(xmlwriter, "Selected_Program_Latency");
		dynamic_gain.Report(xmlwriter, "CWDP_Minus_NoWL_Completion");
		wl_gain.Report(xmlwriter, "NoWL_Minus_Full_Completion");
		if (single_load_gain.samples) {
			single_load_gain.Report(xmlwriter, "Single_Redirected_Load_Gain");
			single_program_gain.Report(xmlwriter, "Single_Redirected_Program_Gain");
			for (int load = 0; load < 3; ++load) {
				for (int program = 0; program < 3; ++program) {
					xmlwriter.Write_open_tag("Single_Gain_Sign_Count");
					xmlwriter.Write_attribute_string("Load_Sign", std::to_string(load - 1));
					xmlwriter.Write_attribute_string("Program_Sign", std::to_string(program - 1));
					count("Requests", single_gain_signs[load][program]);
					xmlwriter.Write_close_tag();
				}
			}
		}
		for (const auto& entry : pages_changed_per_request) {
			xmlwriter.Write_open_tag("Changed_Pages_Per_Request");
			count("Pages", entry.first);
			count("Requests", entry.second);
			xmlwriter.Write_close_tag();
		}
		xmlwriter.Write_close_tag();
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
			xmlwriter.Write_attribute_string("Program_Latency_Source", adaptive.use_average_program_latency ? "AVERAGE" : "CONFIGURED");
			if (adaptive.use_average_program_latency || adaptive.record_wl_decisions) {
				xmlwriter.Write_attribute_string("Average_Program_Latency_ns", std::to_string(adaptive.average_program_latency));
			}
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
			xmlwriter.Write_attribute_string("Single_Page_Cross_Channel_Writes", std::to_string(single_page_cross_channel_writes));
			xmlwriter.Write_attribute_string("Single_Page_Cross_Plane_Writes", std::to_string(single_page_cross_plane_writes));
			xmlwriter.Write_attribute_string("Cross_Plane_Multipage_Write_Pages", std::to_string(multipage_cross_plane_pages));
			xmlwriter.Write_attribute_string("Cross_Channel_Write_Pages", std::to_string(cross_channel_pages));
			xmlwriter.Write_attribute_string("Joint_Cross_Channel_Multipage_Write_Requests", std::to_string(joint_cross_channel_requests));
			xmlwriter.Write_attribute_string("Open_Request_Predictions_At_End", std::to_string(request_predictions.size()));
			xmlwriter.Write_open_tag("Adaptive_Search");
			xmlwriter.Write_attribute_string("Gain_Beta", std::to_string(adaptive.gain_beta));
			xmlwriter.Write_attribute_string("Screening_Beta", std::to_string(adaptive.Effective_screening_beta()));
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
			if (adaptive.record_wl_decisions) {
				xmlwriter.Write_open_tag("WL_Decision_Stats");
				wl_decisions[0].Report(xmlwriter, "Single_Page");
				wl_decisions[1].Report(xmlwriter, "Multipage");
				xmlwriter.Write_close_tag();
			}
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
