#include "Stats.h"


namespace SSD_Components
{
	unsigned long Stats::IssuedReadCMD = 0;
	unsigned long Stats::IssuedCopybackReadCMD = 0;
	unsigned long Stats::IssuedInterleaveReadCMD = 0;
	unsigned long Stats::IssuedMultiplaneReadCMD = 0;
	unsigned long Stats::IssuedMultiplaneCopybackReadCMD = 0;
	unsigned long Stats::IssuedProgramCMD = 0;
	unsigned long Stats::IssuedInterleaveProgramCMD = 0;
	unsigned long Stats::IssuedMultiplaneProgramCMD = 0;
	unsigned long Stats::IssuedMultiplaneCopybackProgramCMD = 0;
	unsigned long Stats::IssuedInterleaveMultiplaneProgramCMD = 0;
	unsigned long Stats::IssuedSuspendProgramCMD = 0;
	unsigned long Stats::IssuedCopybackProgramCMD = 0;
	unsigned long Stats::IssuedEraseCMD = 0;
	unsigned long Stats::IssuedInterleaveEraseCMD = 0;
	unsigned long Stats::IssuedMultiplaneEraseCMD = 0;
	unsigned long Stats::IssuedInterleaveMultiplaneEraseCMD = 0;
	unsigned long Stats::IssuedSuspendEraseCMD = 0;
	unsigned long Stats::Total_flash_reads_for_mapping = 0;
	unsigned long Stats::Total_flash_writes_for_mapping = 0;
	unsigned long Stats::Total_flash_reads_for_mapping_per_stream[MAX_SUPPORT_STREAMS] = { 0 };
	unsigned long Stats::Total_flash_writes_for_mapping_per_stream[MAX_SUPPORT_STREAMS] = { 0 };
	unsigned int***** Stats::Block_erase_histogram;
	/*NEW*/unsigned int**** Stats::IO_Traffic_Array;
	unsigned int*** Stats::IO_Traffic_Array_Die;
	// /*NEW*/unsigned int** Stats::IO_Traffic_Array_Chip;
	// /*NEW*/unsigned int Stats::IO_Traffic_Array_Average = 0;
	// /*NEW*/unsigned int Stats::IO_Traffic_Array_Total = 0;

	
	/**/unsigned long long Stats::Total_Requests_Serviced = 0, Stats::Total_Simulation_Time = 0;
	/**/unsigned int Stats::Trace_End_Flag = 0; 
	/**/unsigned int Stats::all_chip_ongoging_erase = 0; 
	/**/unsigned int Stats::IOless_and_GCless = 0, Stats::IOless_and_GCmore = 0;
	/**/unsigned int Stats::cold_and_GCless = 0, Stats::hot_and_GCless = 0;
	/**/unsigned int Stats::evict_slot_back = 0, Stats::evict_slot_total = 0, Stats::evict_slot_io = 0;
	/**/unsigned int Stats::gc_greedy_after = 0;
	/**/unsigned int Stats::cold_in_cache = 0, Stats::hot_in_cache = 0;
	/**/unsigned int Stats::write_to_nand = 0, Stats::hot_data = 0, Stats::cold_data = 0;
	/**/unsigned int Stats::Total_Cache_hits = 0, Stats::Total_readTR_hits = 0, Stats::Total_writeTR_hits = 0;
	/**/unsigned int Stats::Total_transactions = 0, Stats::Total_readTR = 0, Stats::Total_writeTR = 0;

	//*****
	int Stats::Setcount=0;
	int Stats::Channel_no=0;
	int Stats::Die_no_per_Chip=0;
	int Stats::Plane_no_per_Die=0;
	int Stats::Block_no_per_Plane=0;
	unsigned long Stats::cache_hit=0;
	unsigned long Stats::read_transaction=0;
	unsigned long Stats::write_transaction=0;
	int Stats::stage=1;
	int Stats::flag=1;
	unsigned long Stats::CMTsize=0;
	unsigned long long Stats::total_request_in_file=0;
	unsigned long long Stats::total_requests_to_server=0;
	int Stats::choose_type=0;
	int Stats::now_count=0;
	int Stats::stage1_end_count=0;
	int Stats::stage2_end_count=0;
	unsigned int Stats::online_read_count=0;
	unsigned int Stats::read_count=0;
	unsigned int Stats::write_count=0;
	unsigned int Stats::read_wait=0;
	unsigned int Stats::write_wait=0;

	unsigned int Stats::loss_read_sector_count=0;
	unsigned int Stats::total_read_sector_count=0;

	int Stats::turn_on=0;
	std::string Stats::filename;


	unsigned int Stats::channel_origin[16]={0};
	unsigned int Stats::channel_total[16]={0};
	//*****



	unsigned int  Stats::CMT_hits = 0, Stats::readTR_CMT_hits = 0, Stats::writeTR_CMT_hits = 0;
	unsigned int  Stats::CMT_miss = 0, Stats::readTR_CMT_miss = 0, Stats::writeTR_CMT_miss = 0;
	unsigned int  Stats::total_CMT_queries = 0, Stats::total_readTR_CMT_queries = 0, Stats::total_writeTR_CMT_queries = 0;

	unsigned int Stats::Total_gc_executions = 0, Stats::Total_gc_executions_per_stream[MAX_SUPPORT_STREAMS] = { 0 };
	unsigned int Stats::Total_page_movements_for_gc = 0, Stats::Total_gc_page_movements_per_stream[MAX_SUPPORT_STREAMS] = { 0 };

	unsigned int Stats::Total_wl_executions = 0, Stats::Total_wl_executions_per_stream[MAX_SUPPORT_STREAMS] = { 0 };
	unsigned int Stats::Total_page_movements_for_wl = 0, Stats::Total_wl_page_movements_per_stream[MAX_SUPPORT_STREAMS] = { 0 };

	unsigned int Stats::CMT_hits_per_stream[MAX_SUPPORT_STREAMS] = { 0 }, Stats::readTR_CMT_hits_per_stream[MAX_SUPPORT_STREAMS] = { 0 }, Stats::writeTR_CMT_hits_per_stream[MAX_SUPPORT_STREAMS] = { 0 };
	unsigned int Stats::CMT_miss_per_stream[MAX_SUPPORT_STREAMS] = { 0 }, Stats::readTR_CMT_miss_per_stream[MAX_SUPPORT_STREAMS] = { 0 }, Stats::writeTR_CMT_miss_per_stream[MAX_SUPPORT_STREAMS] = { 0 };
	unsigned int Stats::total_CMT_queries_per_stream[MAX_SUPPORT_STREAMS] = { 0 }, Stats::total_readTR_CMT_queries_per_stream[MAX_SUPPORT_STREAMS] = { 0 }, Stats::total_writeTR_CMT_queries_per_stream[MAX_SUPPORT_STREAMS] = { 0 };


	void Stats::Init_stats(unsigned int channel_no, unsigned int chip_no_per_channel, unsigned int die_no_per_chip, unsigned int plane_no_per_die, 
		unsigned int block_no_per_plane, unsigned int page_no_per_block, unsigned int max_allowed_block_erase_count,int setcount)
	{
		Channel_no=channel_no;
		Die_no_per_Chip=die_no_per_chip;
		Plane_no_per_Die=plane_no_per_die;
		Block_no_per_Plane=block_no_per_plane;
		Setcount=setcount;
		Block_erase_histogram = new unsigned int ****[channel_no];
		IO_Traffic_Array = new unsigned int***[channel_no];
		IO_Traffic_Array_Die = new unsigned int**[channel_no];
		for (unsigned int channel_cntr = 0; channel_cntr < channel_no; channel_cntr++) {
			Block_erase_histogram[channel_cntr] = new unsigned int***[chip_no_per_channel];
			IO_Traffic_Array[channel_cntr] = new unsigned int**[chip_no_per_channel];
			IO_Traffic_Array_Die[channel_cntr] = new unsigned int*[chip_no_per_channel];
			for (unsigned int chip_cntr = 0; chip_cntr < chip_no_per_channel; chip_cntr++) {
				Block_erase_histogram[channel_cntr][chip_cntr] = new unsigned int**[die_no_per_chip];
				IO_Traffic_Array[channel_cntr][chip_cntr] = new unsigned int*[die_no_per_chip];
				IO_Traffic_Array_Die[channel_cntr][chip_cntr] = new unsigned int[die_no_per_chip];
				for (unsigned int die_cntr = 0; die_cntr < die_no_per_chip; die_cntr++) {
					Block_erase_histogram[channel_cntr][chip_cntr][die_cntr] = new unsigned int*[plane_no_per_die];
					IO_Traffic_Array[channel_cntr][chip_cntr][die_cntr] = new unsigned int[plane_no_per_die];
					IO_Traffic_Array_Die[channel_cntr][chip_cntr][die_cntr] = 0;
					for (unsigned int plane_cntr = 0; plane_cntr < plane_no_per_die; plane_cntr++) {
						Block_erase_histogram[channel_cntr][chip_cntr][die_cntr][plane_cntr] = new unsigned int[max_allowed_block_erase_count];
						IO_Traffic_Array[channel_cntr][chip_cntr][die_cntr][plane_cntr] = 0;
						Block_erase_histogram[channel_cntr][chip_cntr][die_cntr][plane_cntr][0] = block_no_per_plane * page_no_per_block; //At the start of the simulation all pages have zero erase count
						for (unsigned int i = 1; i < max_allowed_block_erase_count; ++i) {
							Block_erase_histogram[channel_cntr][chip_cntr][die_cntr][plane_cntr][i] = 0;
						}
					}
				}
			}
		}

		IssuedReadCMD = 0; IssuedCopybackReadCMD = 0; IssuedInterleaveReadCMD = 0; IssuedMultiplaneReadCMD = 0; IssuedMultiplaneCopybackReadCMD = 0;
		IssuedProgramCMD = 0; IssuedInterleaveProgramCMD = 0; IssuedMultiplaneProgramCMD = 0; IssuedMultiplaneCopybackProgramCMD = 0; IssuedInterleaveMultiplaneProgramCMD = 0; IssuedSuspendProgramCMD = 0; IssuedCopybackProgramCMD = 0;
		IssuedEraseCMD = 0; IssuedInterleaveEraseCMD = 0; IssuedMultiplaneEraseCMD = 0; IssuedInterleaveMultiplaneEraseCMD = 0;
		IssuedSuspendEraseCMD = 0;
		
		Total_Requests_Serviced = 0, Total_Simulation_Time = 0;
		Trace_End_Flag = 0;
		// IO_Traffic_Array_Total = 0;
		// /*NEW*/IO_Traffic_Array_Average = 0;
		/**/all_chip_ongoging_erase = 0;
		/**/IOless_and_GCless = 0, IOless_and_GCmore = 0;
		/**/cold_and_GCless = 0, hot_and_GCless = 0;
		/**/evict_slot_back = 0, evict_slot_total = 0, evict_slot_io = 0;
		/**/gc_greedy_after = 0;
		/**/cold_in_cache = 0, hot_in_cache = 0;
		/**/write_to_nand = 0, hot_data = 0, cold_data = 0;
		/**/Total_Cache_hits = 0, Total_readTR_hits = 0, Total_writeTR_hits = 0;
		/**/Total_transactions = 0, Total_readTR = 0, Total_writeTR = 0;

		Total_flash_reads_for_mapping = 0; Total_flash_writes_for_mapping = 0; 
		CMT_hits = 0; readTR_CMT_hits = 0; writeTR_CMT_hits = 0;
		CMT_miss = 0; readTR_CMT_miss = 0; writeTR_CMT_miss = 0;
		total_CMT_queries = 0; total_readTR_CMT_queries = 0; total_writeTR_CMT_queries = 0;

		Total_gc_executions = 0;  Total_page_movements_for_gc = 0;
		Total_wl_executions = 0;  Total_page_movements_for_wl = 0;

		for (stream_id_type stream_id = 0; stream_id < MAX_SUPPORT_STREAMS; stream_id++) {
			Total_flash_reads_for_mapping_per_stream[stream_id] = 0;
			Total_flash_writes_for_mapping_per_stream[stream_id] = 0;
			CMT_hits_per_stream[stream_id] = 0; readTR_CMT_hits_per_stream[stream_id] = 0; writeTR_CMT_hits_per_stream[stream_id] = 0;
			CMT_miss_per_stream[stream_id] = 0; readTR_CMT_miss_per_stream[stream_id] = 0;  writeTR_CMT_miss_per_stream[stream_id] = 0;
			total_CMT_queries_per_stream[stream_id] = 0; total_readTR_CMT_queries_per_stream[stream_id] = 0; total_writeTR_CMT_queries_per_stream[stream_id] = 0;
			Total_gc_executions_per_stream[stream_id] = 0;
			Total_gc_page_movements_per_stream[stream_id] = 0;
			Total_wl_executions_per_stream[stream_id] = 0;
			Total_wl_page_movements_per_stream[stream_id] = 0;
		}
	}

	void Stats::Clear_stats(unsigned int channel_no, unsigned int chip_no_per_channel, unsigned int die_no_per_chip, unsigned int plane_no_per_die,
		unsigned int block_no_per_plane, unsigned int page_no_per_block, unsigned int max_allowed_block_erase_count,int setcount)
	{
		std::cout << "构了" << std::endl;
		for (unsigned int channel_cntr = 0; channel_cntr < channel_no; channel_cntr++) {
			for (unsigned int chip_cntr = 0; chip_cntr < chip_no_per_channel; chip_cntr++) {
				for (unsigned int die_cntr = 0; die_cntr < die_no_per_chip; die_cntr++) {
					for (unsigned int plane_cntr = 0; plane_cntr < plane_no_per_die; plane_cntr++) {
						delete[] Block_erase_histogram[channel_cntr][chip_cntr][die_cntr][plane_cntr];
					}
					delete[] Block_erase_histogram[channel_cntr][chip_cntr][die_cntr];
					delete[] IO_Traffic_Array[channel_cntr][chip_cntr][die_cntr];
				}
				delete[] Block_erase_histogram[channel_cntr][chip_cntr];
				delete[] IO_Traffic_Array[channel_cntr][chip_cntr];
				delete[] IO_Traffic_Array_Die[channel_cntr][chip_cntr];
			}
			delete[] Block_erase_histogram[channel_cntr];
			delete[] IO_Traffic_Array[channel_cntr];
			delete[] IO_Traffic_Array_Die[channel_cntr];
			// delete[] IO_Traffic_Array_Chip[channel_cntr];
		}
		delete[] Block_erase_histogram;
		delete[] IO_Traffic_Array;
		delete[] IO_Traffic_Array_Die;
		// delete[] IO_Traffic_Array_Chip;
	}
}
