#include <stdexcept>
#include "Flash_Block_Manager.h"


namespace SSD_Components
{
	unsigned int Block_Pool_Slot_Type::Page_vector_size = 0;
	Flash_Block_Manager_Base::Flash_Block_Manager_Base(GC_and_WL_Unit_Base* gc_and_wl_unit, unsigned int max_allowed_block_erase_count, unsigned int total_concurrent_streams_no,
		unsigned int channel_count, unsigned int chip_no_per_channel, unsigned int die_no_per_chip, unsigned int plane_no_per_die,
		unsigned int block_no_per_plane, unsigned int page_no_per_block)
		: gc_and_wl_unit(gc_and_wl_unit), max_allowed_block_erase_count(max_allowed_block_erase_count), total_concurrent_streams_no(total_concurrent_streams_no),
		channel_count(channel_count), chip_no_per_channel(chip_no_per_channel), die_no_per_chip(die_no_per_chip), plane_no_per_die(plane_no_per_die),
		block_no_per_plane(block_no_per_plane), pages_no_per_block(page_no_per_block)
	{
		plane_manager = new PlaneBookKeepingType***[channel_count];
		for (unsigned int channelID = 0; channelID < channel_count; channelID++) {
			plane_manager[channelID] = new PlaneBookKeepingType**[chip_no_per_channel];
			for (unsigned int chipID = 0; chipID < chip_no_per_channel; chipID++) {
				plane_manager[channelID][chipID] = new PlaneBookKeepingType*[die_no_per_chip];
				for (unsigned int dieID = 0; dieID < die_no_per_chip; dieID++) {
					plane_manager[channelID][chipID][dieID] = new PlaneBookKeepingType[plane_no_per_die];

					//Initialize plane book keeping data structure
					for (unsigned int planeID = 0; planeID < plane_no_per_die; planeID++) {

						/**/plane_manager[channelID][chipID][dieID][planeID].Total_GC_count = 0;
						/**/plane_manager[channelID][chipID][dieID][planeID].Total_WL_count = 0;

						plane_manager[channelID][chipID][dieID][planeID].Total_pages_count = block_no_per_plane * pages_no_per_block;
						plane_manager[channelID][chipID][dieID][planeID].Free_pages_count = block_no_per_plane * pages_no_per_block;
						plane_manager[channelID][chipID][dieID][planeID].Valid_pages_count = 0;
						plane_manager[channelID][chipID][dieID][planeID].Invalid_pages_count = 0;
						plane_manager[channelID][chipID][dieID][planeID].Ongoing_erase_operations.clear();
						plane_manager[channelID][chipID][dieID][planeID].Blocks = new Block_Pool_Slot_Type[block_no_per_plane];
						
						//Initialize block pool for plane
						for (unsigned int blockID = 0; blockID < block_no_per_plane; blockID++) {
							plane_manager[channelID][chipID][dieID][planeID].Blocks[blockID].BlockID = blockID;
							plane_manager[channelID][chipID][dieID][planeID].Blocks[blockID].Current_page_write_index = 0;
							plane_manager[channelID][chipID][dieID][planeID].Blocks[blockID].Current_status = Block_Service_Status::IDLE;
							plane_manager[channelID][chipID][dieID][planeID].Blocks[blockID].Invalid_page_count = 0;

							plane_manager[channelID][chipID][dieID][planeID].Blocks[blockID].Erase_count = 0;
							
							
							
							plane_manager[channelID][chipID][dieID][planeID].Blocks[blockID].Holds_mapping_data = false;
							plane_manager[channelID][chipID][dieID][planeID].Blocks[blockID].Has_ongoing_gc_wl = false;

							/**/plane_manager[channelID][chipID][dieID][planeID].Blocks[blockID].Has_ongoing_wl = false;

							plane_manager[channelID][chipID][dieID][planeID].Blocks[blockID].Erase_transaction = NULL;
							plane_manager[channelID][chipID][dieID][planeID].Blocks[blockID].Ongoing_user_program_count = 0;
							plane_manager[channelID][chipID][dieID][planeID].Blocks[blockID].Ongoing_user_read_count = 0;
							Block_Pool_Slot_Type::Page_vector_size = pages_no_per_block / (sizeof(uint64_t) * 8) + (pages_no_per_block % (sizeof(uint64_t) * 8) == 0 ? 0 : 1);
							plane_manager[channelID][chipID][dieID][planeID].Blocks[blockID].Invalid_page_bitmap = new uint64_t[Block_Pool_Slot_Type::Page_vector_size];
							for (unsigned int i = 0; i < Block_Pool_Slot_Type::Page_vector_size; i++) {
								plane_manager[channelID][chipID][dieID][planeID].Blocks[blockID].Invalid_page_bitmap[i] = All_VALID_PAGE;
							}
							plane_manager[channelID][chipID][dieID][planeID].Add_to_free_block_pool(&plane_manager[channelID][chipID][dieID][planeID].Blocks[blockID], false);
						}

						//Stats::Total_gc_executions += plane_manager[channelID][chipID][dieID][planeID].Total_GC_count;

						plane_manager[channelID][chipID][dieID][planeID].Data_wf = new Block_Pool_Slot_Type*[total_concurrent_streams_no];
						plane_manager[channelID][chipID][dieID][planeID].Translation_wf = new Block_Pool_Slot_Type*[total_concurrent_streams_no];
						plane_manager[channelID][chipID][dieID][planeID].GC_wf = new Block_Pool_Slot_Type*[total_concurrent_streams_no];
						for (unsigned int stream_cntr = 0; stream_cntr < total_concurrent_streams_no; stream_cntr++) {
							plane_manager[channelID][chipID][dieID][planeID].Data_wf[stream_cntr] = plane_manager[channelID][chipID][dieID][planeID].Get_a_free_block(stream_cntr, false);
							plane_manager[channelID][chipID][dieID][planeID].Translation_wf[stream_cntr] = plane_manager[channelID][chipID][dieID][planeID].Get_a_free_block(stream_cntr, true);
							plane_manager[channelID][chipID][dieID][planeID].GC_wf[stream_cntr] = plane_manager[channelID][chipID][dieID][planeID].Get_a_free_block(stream_cntr, false);
						}
					}
				}
			}
		}
	}

	Flash_Block_Manager_Base::~Flash_Block_Manager_Base() 
	{
		for (unsigned int channel_id = 0; channel_id < channel_count; channel_id++) {
			for (unsigned int chip_id = 0; chip_id < chip_no_per_channel; chip_id++) {
				for (unsigned int die_id = 0; die_id < die_no_per_chip; die_id++) {
					for (unsigned int plane_id = 0; plane_id < plane_no_per_die; plane_id++) {
						for (unsigned int blockID = 0; blockID < block_no_per_plane; blockID++) {
							delete[] plane_manager[channel_id][chip_id][die_id][plane_id].Blocks[blockID].Invalid_page_bitmap;
						}
						delete[] plane_manager[channel_id][chip_id][die_id][plane_id].Blocks;
						delete[] plane_manager[channel_id][chip_id][die_id][plane_id].GC_wf;
						delete[] plane_manager[channel_id][chip_id][die_id][plane_id].Data_wf;
						delete[] plane_manager[channel_id][chip_id][die_id][plane_id].Translation_wf;
					}
					delete[] plane_manager[channel_id][chip_id][die_id];
				}
				delete[] plane_manager[channel_id][chip_id];
			}
			delete[] plane_manager[channel_id];
		}
		delete[] plane_manager;
	}

	void Flash_Block_Manager_Base::Set_GC_and_WL_Unit(GC_and_WL_Unit_Base* gcwl)
	{
		this->gc_and_wl_unit = gcwl;
	}

	void Block_Pool_Slot_Type::Erase()
	{
		Current_page_write_index = 0;
		Invalid_page_count = 0;
		Erase_count++;
		for (unsigned int i = 0; i < Block_Pool_Slot_Type::Page_vector_size; i++) {
			Invalid_page_bitmap[i] = All_VALID_PAGE;
		}
		Stream_id = NO_STREAM;
		Holds_mapping_data = false;
		Erase_transaction = NULL;
	}

	Block_Pool_Slot_Type* PlaneBookKeepingType::Get_a_free_block(stream_id_type stream_id, bool for_mapping_data)
	{
		return Take_free_block(Free_block_pool.begin(), stream_id, for_mapping_data);
	}

	Block_Pool_Slot_Type* PlaneBookKeepingType::Take_free_block(decltype(Free_block_pool)::iterator entry,
		stream_id_type stream_id, bool for_mapping_data)
	{
		if (entry == Free_block_pool.end()) {
			throw std::runtime_error("Requesting an unavailable free block");
		}
		auto* new_block = entry->second;
		Free_block_pool.erase(entry);
		new_block->Stream_id = stream_id;
		new_block->Holds_mapping_data = for_mapping_data;
		Block_usage_history.push(new_block->BlockID);

		return new_block;
	}
	
	void PlaneBookKeepingType::Check_bookkeeping_correctness(const NVM::FlashMemory::Physical_Page_Address& plane_address)
	{
		if (Total_pages_count != Free_pages_count + Valid_pages_count + Invalid_pages_count) {
			PRINT_ERROR("Inconsistent status in the plane bookkeeping record!")
		}
		if (Free_pages_count == 0) {
			PRINT_ERROR("Plane " << "@" << plane_address.ChannelID << "@" << plane_address.ChipID << "@" << plane_address.DieID << "@" << plane_address.PlaneID << " pool size: " << Get_free_block_pool_size() << " ran out of free pages! Bad resource management! It is not safe to continue simulation!");
		}
	}

	unsigned int PlaneBookKeepingType::Get_free_block_pool_size()
	{
		return (unsigned int)Free_block_pool.size();
	}

	void PlaneBookKeepingType::Add_to_free_block_pool(Block_Pool_Slot_Type* block, bool consider_dynamic_wl)
	{
		if (consider_dynamic_wl) {
			std::pair<unsigned int, Block_Pool_Slot_Type*> entry(block->Erase_count, block);
			Free_block_pool.insert(entry);
		}
		else {
			std::pair<unsigned int, Block_Pool_Slot_Type*> entry(0, block);
			Free_block_pool.insert(entry);
		}
		
	}


	unsigned int Flash_Block_Manager_Base::Get_min_max_erase_difference(const NVM::FlashMemory::Physical_Page_Address& plane_address)
	{
		PlaneBookKeepingType *plane_record = &plane_manager[plane_address.ChannelID][plane_address.ChipID][plane_address.DieID][plane_address.PlaneID];

		unsigned int min_erased_block = Get_coldest_block_id(plane_address);
		unsigned int max_erased_block = Get_hotest_block_id(plane_address);
		// if (plane_address.ChannelID == 2 && plane_address.ChipID == 0 && plane_address.DieID == 0 && plane_address.PlaneID == 0) {
		// 	std::cout << "MAX" << max_erased_block << "Erase Count " << plane_record->Blocks[max_erased_block].Erase_count << std::endl;
		// 	std::cout << "MIN" << min_erased_block << "Erase Count" << plane_record->Blocks[min_erased_block].Erase_count << std::endl;
		// }
		return  plane_record->Blocks[max_erased_block].Erase_count - plane_record->Blocks[min_erased_block].Erase_count;
	}

	flash_block_ID_type Flash_Block_Manager_Base::Get_coldest_block_id(const NVM::FlashMemory::Physical_Page_Address& plane_address)
	{
		unsigned int min_erased_block = 0;

		PlaneBookKeepingType *plane_record = &plane_manager[plane_address.ChannelID][plane_address.ChipID][plane_address.DieID][plane_address.PlaneID];
		
		for (unsigned int stream_id = 0; stream_id < gc_and_wl_unit->address_mapping_unit->Get_no_of_input_streams(); stream_id++) {
			while (((&plane_record->Blocks[min_erased_block]) == plane_record->Translation_wf[stream_id])
				|| ((&plane_record->Blocks[min_erased_block]) == plane_record->Data_wf[stream_id])
				|| ((&plane_record->Blocks[min_erased_block]) == plane_record->GC_wf[stream_id])
				|| (plane_record->Blocks[min_erased_block].Current_page_write_index != pages_no_per_block)
				|| (plane_record->Blocks[min_erased_block].Ongoing_user_program_count > 0)) {
				min_erased_block++;
			}
		}

		for (unsigned int stream_id = 0; stream_id < gc_and_wl_unit->address_mapping_unit->Get_no_of_input_streams(); stream_id++) {
			//while ((&plane_record->Blocks[min_erased_block] == plane_record->Data_wf[stream_id])
			//while ((&plane_record->Blocks[min_erased_block] == plane_record->Data_wf[stream_id])
			//	|| (&plane_record->Blocks[min_erased_block] == plane_record->Translation_wf[stream_id])
			//	|| (&plane_record->Blocks[min_erased_block] == plane_record->GC_wf[stream_id])) {
			//	min_erased_block++;
			//}


			for (unsigned int i = min_erased_block + 1; i < block_no_per_plane; i++) {
				if ((plane_record->Blocks[i].Erase_count <= plane_record->Blocks[min_erased_block].Erase_count)
					&& ((&plane_record->Blocks[i]) != plane_record->Translation_wf[stream_id])
					&& ((&plane_record->Blocks[i]) != plane_record->Data_wf[stream_id])
					&& ((&plane_record->Blocks[i]) != plane_record->GC_wf[stream_id])
					&& (plane_record->Blocks[i].Current_page_write_index == pages_no_per_block)
					&& (plane_record->Blocks[i].Ongoing_user_program_count == 0)) {
					min_erased_block = i;
					//
				}
			}
		}

		return min_erased_block;
	}

	/**/flash_block_ID_type Flash_Block_Manager_Base::Get_hotest_block_id(const NVM::FlashMemory::Physical_Page_Address& plane_address)
	{
		unsigned int max_erased_block = 0;
		PlaneBookKeepingType* plane_record = &plane_manager[plane_address.ChannelID][plane_address.ChipID][plane_address.DieID][plane_address.PlaneID];

		for (unsigned int stream_id = 0; stream_id < gc_and_wl_unit->address_mapping_unit->Get_no_of_input_streams(); stream_id++) {
			//while ((&plane_record->Blocks[max_erased_block]) == plane_record->Data_wf[stream_id]
			//	|| (&plane_record->Blocks[max_erased_block]) == plane_record->Translation_wf[stream_id]
			//	|| (&plane_record->Blocks[max_erased_block]) == plane_record->GC_wf[stream_id]) {
			//	max_erased_block++;
			//}

			for (unsigned int i = max_erased_block + 1; i < block_no_per_plane; i++) {
				if ((plane_record->Blocks[i].Erase_count > plane_record->Blocks[max_erased_block].Erase_count)) {
					max_erased_block = i;
				}
			}
		}

		return max_erased_block;
	}

	PlaneBookKeepingType* Flash_Block_Manager_Base::Get_plane_bookkeeping_entry(const NVM::FlashMemory::Physical_Page_Address& plane_address)
	{
		return &(plane_manager[plane_address.ChannelID][plane_address.ChipID][plane_address.DieID][plane_address.PlaneID]);
	}


	bool Flash_Block_Manager_Base::Block_has_ongoing_gc_wl(const NVM::FlashMemory::Physical_Page_Address& block_address)
	{
		PlaneBookKeepingType *plane_record = &plane_manager[block_address.ChannelID][block_address.ChipID][block_address.DieID][block_address.PlaneID];
		return plane_record->Blocks[block_address.BlockID].Has_ongoing_gc_wl;
	}

	/**/bool Flash_Block_Manager_Base::Block_has_ongoing_wl(const NVM::FlashMemory::Physical_Page_Address& block_address)
	{
		PlaneBookKeepingType* plane_record = &plane_manager[block_address.ChannelID][block_address.ChipID][block_address.DieID][block_address.PlaneID];
		return plane_record->Blocks[block_address.BlockID].Has_ongoing_wl;
	}

	
	bool Flash_Block_Manager_Base::Can_execute_gc_wl(const NVM::FlashMemory::Physical_Page_Address& block_address)
	{
		PlaneBookKeepingType *plane_record = &plane_manager[block_address.ChannelID][block_address.ChipID][block_address.DieID][block_address.PlaneID];
		if(plane_record->Blocks[block_address.BlockID].Ongoing_user_program_count < 0){
			std::cout <<"1 " <<plane_record->Blocks[block_address.BlockID].Ongoing_user_program_count << std::endl;
		}
		if(plane_record->Blocks[block_address.BlockID].Ongoing_user_read_count < 0){
			std::cout <<"2 " << plane_record->Blocks[block_address.BlockID].Ongoing_user_read_count << std::endl;
		}
		return (plane_record->Blocks[block_address.BlockID].Ongoing_user_program_count + plane_record->Blocks[block_address.BlockID].Ongoing_user_read_count == 0);
	}

	//void Flash_Block_Manager_Base::GC_WL_started(const NVM::FlashMemory::Physical_Page_Address& block_address)
	/**/void Flash_Block_Manager_Base::GC_WL_started(const NVM::FlashMemory::Physical_Page_Address& block_address, bool wl)
	{
		PlaneBookKeepingType *plane_record = &plane_manager[block_address.ChannelID][block_address.ChipID][block_address.DieID][block_address.PlaneID];
		if (wl && plane_record->Blocks[block_address.BlockID].Has_ongoing_gc_wl) {
			PRINT_ERROR("WL选中的块正在做GC！")
		}
		plane_record->Blocks[block_address.BlockID].Has_ongoing_gc_wl = true;

		if (wl) {
			/**/plane_record->Blocks[block_address.BlockID].Has_ongoing_wl = true;
		}

	}
	
	void Flash_Block_Manager_Base::program_transaction_issued(const NVM::FlashMemory::Physical_Page_Address& page_address)
	{
		PlaneBookKeepingType *plane_record = &plane_manager[page_address.ChannelID][page_address.ChipID][page_address.DieID][page_address.PlaneID];
		plane_record->Blocks[page_address.BlockID].Ongoing_user_program_count++;		

	}
	
	void Flash_Block_Manager_Base::Read_transaction_issued(const NVM::FlashMemory::Physical_Page_Address& page_address)
	{
		PlaneBookKeepingType *plane_record = &plane_manager[page_address.ChannelID][page_address.ChipID][page_address.DieID][page_address.PlaneID];
		plane_record->Blocks[page_address.BlockID].Ongoing_user_read_count++;
	}

	void Flash_Block_Manager_Base::Program_transaction_serviced(const NVM::FlashMemory::Physical_Page_Address& page_address)
	{
		PlaneBookKeepingType *plane_record = &plane_manager[page_address.ChannelID][page_address.ChipID][page_address.DieID][page_address.PlaneID];
		plane_record->Blocks[page_address.BlockID].Ongoing_user_program_count--;

	}

	void Flash_Block_Manager_Base::Read_transaction_serviced(const NVM::FlashMemory::Physical_Page_Address& page_address)
	{
		PlaneBookKeepingType *plane_record = &plane_manager[page_address.ChannelID][page_address.ChipID][page_address.DieID][page_address.PlaneID];
		plane_record->Blocks[page_address.BlockID].Ongoing_user_read_count--;

	}
	
	bool Flash_Block_Manager_Base::Is_having_ongoing_program(const NVM::FlashMemory::Physical_Page_Address& block_address)
	{
		PlaneBookKeepingType *plane_record = &plane_manager[block_address.ChannelID][block_address.ChipID][block_address.DieID][block_address.PlaneID];
		return plane_record->Blocks[block_address.BlockID].Ongoing_user_program_count > 0;
	}

	bool Flash_Block_Manager_Base::Is_having_ongoing_read(const NVM::FlashMemory::Physical_Page_Address& block_address)
	{
		PlaneBookKeepingType* plane_record = &plane_manager[block_address.ChannelID][block_address.ChipID][block_address.DieID][block_address.PlaneID];
		return plane_record->Blocks[block_address.BlockID].Ongoing_user_read_count > 0;
	}

	void Flash_Block_Manager_Base::GC_WL_finished(const NVM::FlashMemory::Physical_Page_Address& block_address)
	{
		PlaneBookKeepingType *plane_record = &plane_manager[block_address.ChannelID][block_address.ChipID][block_address.DieID][block_address.PlaneID];
		plane_record->Blocks[block_address.BlockID].Has_ongoing_gc_wl = false;

		/**/plane_record->Blocks[block_address.BlockID].Has_ongoing_wl = false;
	}
	
	bool Flash_Block_Manager_Base::Is_page_valid(Block_Pool_Slot_Type* block, flash_page_ID_type page_id)
	{
		if ((block->Invalid_page_bitmap[page_id / 64] & (((uint64_t)0x1) << (page_id % 64))) == 0) {
			return true;
		}
		return false;
	}

	/**/unsigned int Flash_Block_Manager_Base::get_channel_count()
	{
		return channel_count;
	}

	/**/unsigned int Flash_Block_Manager_Base::get_chip_no_per_channel()
	{
		return chip_no_per_channel;
	}

	/**/unsigned int Flash_Block_Manager_Base::get_die_no_per_chip()
	{
		return die_no_per_chip;
	}

	/**/unsigned int Flash_Block_Manager_Base::get_plane_no_per_die()
	{
		return plane_no_per_die;
	}
	

	/**/unsigned int Flash_Block_Manager_Base::get_average_plane_GC_and_WL_count()
	{
		unsigned int Plane_average_GC_and_WL_count = (int)((double)(Stats::Total_gc_executions + Stats::Total_wl_executions) / (double)(channel_count * chip_no_per_channel * die_no_per_chip * plane_no_per_die));
		return Plane_average_GC_and_WL_count;
	}


	/**/unsigned int Flash_Block_Manager_Base::get_average_Erase_count()
	{
		unsigned int Plane_Total_Erase_count = 0;
		for (unsigned int channelID = 0; channelID < channel_count; channelID++) {
			for (unsigned int chipID = 0; chipID < chip_no_per_channel; chipID++) {
				for (unsigned int dieID = 0; dieID < die_no_per_chip; dieID++) {
					for (unsigned int planeID = 0; planeID < plane_no_per_die; planeID++) {
						for (unsigned int blockID = 0; blockID < block_no_per_plane; blockID++) {
							Plane_Total_Erase_count += plane_manager[channelID][chipID][dieID][planeID].Blocks[blockID].Erase_count;
						}
					}
				}
			}
		}
		unsigned int Plane_average_Erase_count = (int)((double)Plane_Total_Erase_count / (double)(channel_count * chip_no_per_channel * die_no_per_chip * plane_no_per_die));
		return Plane_average_Erase_count;
	}


	unsigned int Flash_Block_Manager_Base::Get_Max_Erase_count_on_plane(const NVM::FlashMemory::Physical_Page_Address& page_address){
		unsigned int Max_Erase_count_in_plane = plane_manager[page_address.ChannelID][page_address.ChipID][page_address.DieID][page_address.PlaneID].Blocks[0].Erase_count;

		for (unsigned int blockID = 1; blockID < block_no_per_plane; blockID++) {
			if(Max_Erase_count_in_plane < plane_manager[page_address.ChannelID][page_address.ChipID][page_address.DieID][page_address.PlaneID].Blocks[blockID].Erase_count){
				Max_Erase_count_in_plane = plane_manager[page_address.ChannelID][page_address.ChipID][page_address.DieID][page_address.PlaneID].Blocks[blockID].Erase_count;
			}
		}
		return Max_Erase_count_in_plane;
	}

	unsigned int Flash_Block_Manager_Base::Get_total_erase_count_in_plane(const NVM::FlashMemory::Physical_Page_Address& page_address){
		unsigned int total_erase_count_in_plane=0;
		for (unsigned int blockID = 0; blockID < block_no_per_plane; blockID++) {
			total_erase_count_in_plane += plane_manager[page_address.ChannelID][page_address.ChipID][page_address.DieID][page_address.PlaneID].Blocks[blockID].Erase_count;
		}
		return total_erase_count_in_plane;

	}
	
}
