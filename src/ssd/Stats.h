#ifndef STATS_H
#define STATS_H

#include "SSD_Defs.h"

namespace SSD_Components
{
	class Stats
	{
	public:
		static void Init_stats(unsigned int channel_no, unsigned int chip_no_per_channel, unsigned int die_no_per_chip, unsigned int plane_no_per_die, unsigned int block_no_per_plane, unsigned int page_no_per_block, unsigned int max_allowed_block_erase_count,int setcount);
		static void Clear_stats(unsigned int channel_no, unsigned int chip_no_per_channel, unsigned int die_no_per_chip, unsigned int plane_no_per_die, unsigned int block_no_per_plane, unsigned int page_no_per_block, unsigned int max_allowed_block_erase_count,int setcount);
		static unsigned long IssuedReadCMD, IssuedCopybackReadCMD, IssuedInterleaveReadCMD, IssuedMultiplaneReadCMD, IssuedMultiplaneCopybackReadCMD;
		static unsigned long IssuedProgramCMD, IssuedInterleaveProgramCMD, IssuedMultiplaneProgramCMD, IssuedInterleaveMultiplaneProgramCMD, IssuedCopybackProgramCMD, IssuedMultiplaneCopybackProgramCMD;
		static unsigned long IssuedEraseCMD, IssuedInterleaveEraseCMD, IssuedMultiplaneEraseCMD, IssuedInterleaveMultiplaneEraseCMD;

		static unsigned long IssuedSuspendProgramCMD, IssuedSuspendEraseCMD;

		static unsigned long Total_flash_reads_for_mapping, Total_flash_writes_for_mapping;
		static unsigned long Total_flash_reads_for_mapping_per_stream[MAX_SUPPORT_STREAMS], Total_flash_writes_for_mapping_per_stream[MAX_SUPPORT_STREAMS];

		/**/static unsigned long long Total_Requests_Serviced, Total_Simulation_Time;
		/**/static unsigned int Trace_End_Flag;
		/**/static unsigned int all_chip_ongoging_erase;
		/**/static unsigned int IOless_and_GCless, IOless_and_GCmore;
		/**/static unsigned int cold_and_GCless, hot_and_GCless;
		/**/static unsigned int evict_slot_back, evict_slot_total, evict_slot_io;
		/**/static unsigned int gc_greedy_after;
		/**/static unsigned int cold_in_cache, hot_in_cache;
		/**/static unsigned int write_to_nand, hot_data, cold_data;
		/**/static unsigned int Total_Cache_hits, Total_readTR_hits, Total_writeTR_hits;
		/**/static unsigned int Total_transactions, Total_readTR, Total_writeTR;

		//*****
		static int Setcount;
		static int Channel_no;
		static int Die_no_per_Chip;
		static int Plane_no_per_Die;
		static int Block_no_per_Plane;
		static unsigned long cache_hit;
		static unsigned long read_transaction;
		static unsigned long write_transaction;
		static int stage;
		static int flag;
		static unsigned long CMTsize;
		static unsigned long long total_request_in_file;
		static unsigned long long total_requests_to_server;
		static int choose_type;
		static int now_count;
		static int stage1_end_count;
		static int stage2_end_count;
		static unsigned int online_read_count;
		static unsigned int read_count;
		static unsigned int write_count;
		static unsigned int read_wait;
		static unsigned int write_wait;

		static unsigned int loss_read_sector_count;
		static unsigned int total_read_sector_count;

		static int turn_on;
		static std::string filename;
		

		static unsigned int channel_origin[16];
		static unsigned int channel_total[16];
		//*****

		static unsigned int CMT_hits, readTR_CMT_hits, writeTR_CMT_hits;
		static unsigned int CMT_miss, readTR_CMT_miss, writeTR_CMT_miss;
		static unsigned int total_CMT_queries, total_readTR_CMT_queries, total_writeTR_CMT_queries;
		
		static unsigned int CMT_hits_per_stream[MAX_SUPPORT_STREAMS], readTR_CMT_hits_per_stream[MAX_SUPPORT_STREAMS], writeTR_CMT_hits_per_stream[MAX_SUPPORT_STREAMS];
		static unsigned int CMT_miss_per_stream[MAX_SUPPORT_STREAMS], readTR_CMT_miss_per_stream[MAX_SUPPORT_STREAMS], writeTR_CMT_miss_per_stream[MAX_SUPPORT_STREAMS];
		static unsigned int total_CMT_queries_per_stream[MAX_SUPPORT_STREAMS], total_readTR_CMT_queries_per_stream[MAX_SUPPORT_STREAMS], total_writeTR_CMT_queries_per_stream[MAX_SUPPORT_STREAMS];
		

		static unsigned int Total_gc_executions, Total_gc_executions_per_stream[MAX_SUPPORT_STREAMS];
		static unsigned int Total_page_movements_for_gc, Total_gc_page_movements_per_stream[MAX_SUPPORT_STREAMS];

		static unsigned int Total_wl_executions, Total_wl_executions_per_stream[MAX_SUPPORT_STREAMS];
		static unsigned int Total_page_movements_for_wl, Total_wl_page_movements_per_stream[MAX_SUPPORT_STREAMS];

		static unsigned int***** Block_erase_histogram;
		/*NEW*/static unsigned int**** IO_Traffic_Array;
		static unsigned int*** IO_Traffic_Array_Die;

		// /*NEW*/static unsigned int** IO_Traffic_Array_Chip;
		// /*NEW*/static unsigned int IO_Traffic_Array_Average;
		// /*NEW*/static unsigned int IO_Traffic_Array_Total;
		

	};
}

#endif // !STATS_H
