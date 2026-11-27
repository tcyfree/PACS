#ifndef PACS_H
#define PACS_H

#include <array>
#include <map>
#include <unordered_map>
#include <vector>
#include "../sim/Sim_Defs.h"
#include "../nvm_chip/flash_memory/Physical_Page_Address.h"
#include "../utils/XMLWriter.h"

namespace SSD_Components
{
	class User_Request;

	struct PACS_Adaptive_Options {
		bool enabled = false;
		double gain_beta = 1.0;
		unsigned int search_budget = 256;
		bool use_average_program_latency = false;
		bool record_wl_decisions = false;
		sim_time_type average_program_latency = 0;
	};

	struct PACS_Search_Statistics {
		std::size_t critical_pages = 0;
		unsigned int evaluations = 0;
		unsigned int rejected_by_beta = 0;
		bool only_below_beta = false;
		bool budget_exhausted = false;
	};

	struct PACS_Decision_Comparison {
		std::size_t pages;
		std::size_t cwdp_to_mean, cwdp_to_full, mean_to_full, mean_to_full_lanes, same_die_plane_changes;
		double cwdp_finish, mean_plan_finish, full_finish;
		double single_page_program_gain;
		PACS_Search_Statistics mean_search, full_search;
	};

	class PACS
	{
	public:
		PACS(unsigned int channels, unsigned int chips_per_channel, unsigned int dies_per_chip,
			bool request_level = false, bool cross_channel = false, PACS_Adaptive_Options adaptive = {});
		bool Request_level_enabled() const { return request_level; }
		bool Cross_channel_enabled() const { return cross_channel; }
		const PACS_Adaptive_Options& Adaptive_options() const { return adaptive; }
		bool Uses_online_program_profiling() const { return !adaptive.enabled; }
		void Queue_program(const NVM::FlashMemory::Physical_Page_Address& address);
		void Complete_program(const NVM::FlashMemory::Physical_Page_Address& address);
		void Observe_program(const NVM::FlashMemory::Physical_Page_Address& address, unsigned int page_type,
			sim_time_type program_start, sim_time_type ready_time);
		double Estimate_completion_cost(const NVM::FlashMemory::Physical_Page_Address& address, unsigned int page_type) const;
		double Estimate_program_latency(const NVM::FlashMemory::Physical_Page_Address& address, unsigned int page_type) const;
		void Record_ranked_allocation(bool redirected);
		void Record_single_page_plan(bool crossed_channel, bool changed_plane);
		void Record_request_plan(std::size_t redirected_pages, std::size_t crossed_pages, std::size_t changed_planes);
		void Record_adaptive_search(std::size_t critical_pages, unsigned int evaluations, bool budget_exhausted);
		void Record_wl_decision(const PACS_Decision_Comparison& comparison);
		void Record_wl_candidates(bool multipage, double program_gap, double load_gap,
			double original_program, double selected_program);
		void Record_wl_plan_commit(bool multipage, bool matched);
		void Track_request_prediction(const User_Request* request, std::size_t pages, sim_time_type start,
			double estimated_duration, bool crossed_channel);
		void Observe_request_page_completion(const User_Request* request, sim_time_type finish);
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
		struct Request_Prediction {
			std::size_t pending_pages;
			sim_time_type start;
			double estimated_duration;
			bool crossed_channel;
		};
		struct Prediction_Error {
			uint64_t samples = 0;
			double signed_error = 0;
			double absolute_error = 0;
			double actual_duration = 0;
			void Observe(double estimated, sim_time_type actual);
			void Report(Utils::XmlWriter& xmlwriter, const char* tag) const;
		};
		struct Time_Summary {
			uint64_t samples = 0;
			double sum = 0, min = 0, max = 0;
			void Observe(double value);
			void Report(Utils::XmlWriter& xmlwriter) const;
			void Report(Utils::XmlWriter& xmlwriter, const char* tag) const;
		};
		enum { gap_bin_count = 22 };
		static const std::array<double, gap_bin_count> gap_upper_ns;
		struct Gap_Distribution {
			Time_Summary times;
			std::array<uint64_t, gap_bin_count + 1> counts{};
			void Observe(double gap);
			void Report(Utils::XmlWriter& xmlwriter, const char* tag) const;
		};
		struct WL_Decisions {
			std::array<uint64_t, 5> categories{};
			uint64_t requests = 0, pages = 0, committed_plans = 0;
			uint64_t changed_requests = 0, changed_pages = 0, changed_lanes = 0;
			uint64_t non_wl_redirections = 0, same_die_plane_changes = 0;
			uint64_t full_rejected = 0, mean_rejected = 0, full_only_below_beta = 0, mean_only_below_beta = 0;
			uint64_t full_budget_exhausted = 0, mean_budget_exhausted = 0;
			std::map<std::size_t, uint64_t> pages_changed_per_request;
			Time_Summary original_program, selected_program, dynamic_gain, wl_gain, single_load_gain, single_program_gain;
			Gap_Distribution candidate_program_gap, candidate_load_gap;
			std::array<std::array<uint64_t, 3>, 3> single_gain_signs{};
			void Report(Utils::XmlWriter& xmlwriter, const char* tag) const;
		};
		std::size_t die_index(const NVM::FlashMemory::Physical_Page_Address& address) const;

		const unsigned int chips_per_channel, dies_per_chip;
		const bool request_level;
		const bool cross_channel;
		const PACS_Adaptive_Options adaptive;
		std::vector<Die> dies;
		uint64_t qualified_observations = 0;
		uint64_t discarded_observations = 0;
		uint64_t ranked_allocations = 0;
		uint64_t redirected_allocations = 0;
		uint64_t planned_requests = 0;
		uint64_t redirected_requests = 0;
		uint64_t redirected_request_pages = 0;
		uint64_t cross_channel_requests = 0;
		uint64_t cross_channel_pages = 0;
		uint64_t joint_cross_channel_requests = 0;
		uint64_t single_page_cross_channel_writes = 0;
		uint64_t single_page_cross_plane_writes = 0;
		uint64_t multipage_cross_plane_pages = 0;
		std::map<std::size_t, uint64_t> critical_page_counts;
		std::map<unsigned int, uint64_t> search_evaluation_counts;
		uint64_t search_budget_exhaustions = 0;
		std::unordered_map<const User_Request*, Request_Prediction> request_predictions;
		Prediction_Error prediction_error, cross_channel_prediction_error;
		std::array<WL_Decisions, 2> wl_decisions;
	};
}

#endif
