
#include "../nvm_chip/flash_memory/Physical_Page_Address.h"
#include "Flash_Block_Manager.h"
#include "Stats.h"

namespace SSD_Components
{
	Flash_Block_Manager::Flash_Block_Manager(GC_and_WL_Unit_Base* gc_and_wl_unit, unsigned int max_allowed_block_erase_count, unsigned int total_concurrent_streams_no,
		unsigned int channel_count, unsigned int chip_no_per_channel, unsigned int die_no_per_chip, unsigned int plane_no_per_die,
		unsigned int block_no_per_plane, unsigned int page_no_per_block)
		: Flash_Block_Manager_Base(gc_and_wl_unit, max_allowed_block_erase_count, total_concurrent_streams_no, channel_count, chip_no_per_channel, die_no_per_chip,
			plane_no_per_die, block_no_per_plane, page_no_per_block)
	{
	}

	Flash_Block_Manager::~Flash_Block_Manager()
	{
	}

	QSTR_MED* Flash_Block_Manager::Enable_qstr_med(const std::shared_ptr<const QSTR_MED::Profile>& profile,
		unsigned int pages_per_wordline, std::vector<std::vector<QSTR_MED::Address>> stream_planes)
	{
		qstr_med.reset(new QSTR_MED(*this, profile, pages_per_wordline, std::move(stream_planes)));
		return qstr_med.get();
	}

	Block_Pool_Slot_Type* Flash_Block_Manager::next_write_frontier(PlaneBookKeepingType& plane,
		const QSTR_MED::Address& address, stream_id_type stream, QSTR_MED::Destination destination)
	{
		return qstr_med ? qstr_med->Take_frontier(stream, address, destination)
			: plane.Get_a_free_block(stream, destination == QSTR_MED::Destination::MAPPING);
	}

	//void Flash_Block_Manager::Allocate_block_and_page_in_plane_for_user_write(const stream_id_type stream_id, NVM::FlashMemory::Physical_Page_Address& page_address)
	/*Inconsistency*/void Flash_Block_Manager::Allocate_block_and_page_in_plane_for_user_write(const stream_id_type stream_id, NVM::FlashMemory::Physical_Page_Address& page_address, LPA_type lpa)
	{
		PlaneBookKeepingType *plane_record = &plane_manager[page_address.ChannelID][page_address.ChipID][page_address.DieID][page_address.PlaneID];
		plane_record->Valid_pages_count++;
		plane_record->Free_pages_count--;		
		page_address.BlockID = plane_record->Data_wf[stream_id]->BlockID;
		page_address.PageID = plane_record->Data_wf[stream_id]->Current_page_write_index++;
		program_transaction_issued(page_address);


		/*Inconsistency*///gc_and_wl_unit->flash_controller->Change_fake_flash_page_status_for_preconditioning(page_address, lpa);

		//if (page_address.ChannelID == 2 && page_address.ChipID == 1 && page_address.DieID == 0 && page_address.PlaneID == 1 && page_address.BlockID == 0 && page_address.PageID == 122) {
		//	std::cout << "user-写第0块，122页，要checkgc" << std::endl;
		//}

		//if (lpa == 188714) {
		//	std::cout << "user写走188714" << std::endl;
		//}

		//The current write frontier block is written to the end
		if(plane_record->Data_wf[stream_id]->Current_page_write_index == pages_no_per_block) {
			//Assign a new write frontier block
			plane_record->Data_wf[stream_id] = next_write_frontier(*plane_record, page_address, stream_id, QSTR_MED::Destination::HOST);
			//if (page_address.ChannelID == 2 && page_address.ChipID == 1 && page_address.DieID == 0 && page_address.PlaneID == 1 && plane_record->Data_wf[stream_id]->BlockID == 0) {
			//	std::cout << "取到该255块--user_write" << std::endl;
			//}
			gc_and_wl_unit->Check_gc_required(plane_record->Get_free_block_pool_size(), page_address);
		}

		plane_record->Check_bookkeeping_correctness(page_address);
	}

	void Flash_Block_Manager::Allocate_block_and_page_in_plane_for_precondition(const stream_id_type stream_id, NVM::FlashMemory::Physical_Page_Address& page_address, LPA_type lpa)
	{
		PlaneBookKeepingType *plane_record = &plane_manager[page_address.ChannelID][page_address.ChipID][page_address.DieID][page_address.PlaneID];
		plane_record->Valid_pages_count++;
		plane_record->Free_pages_count--;		
		page_address.BlockID = plane_record->Data_wf[stream_id]->BlockID;
		page_address.PageID = plane_record->Data_wf[stream_id]->Current_page_write_index++;

		//The current write frontier block is written to the end
		if(plane_record->Data_wf[stream_id]->Current_page_write_index == pages_no_per_block) {
			//Assign a new write frontier block
			plane_record->Data_wf[stream_id] = plane_record->Get_a_free_block(stream_id, false);
		}

		plane_record->Check_bookkeeping_correctness(page_address);
	}
	
	//void Flash_Block_Manager::Allocate_block_and_page_in_plane_for_user_write_for_read(const stream_id_type stream_id, NVM::FlashMemory::Physical_Page_Address& page_address)
	/*Inconsistency*/void Flash_Block_Manager::Allocate_block_and_page_in_plane_for_user_write_for_read(const stream_id_type stream_id, NVM::FlashMemory::Physical_Page_Address& page_address, LPA_type lpa)
	{
		PlaneBookKeepingType* plane_record = &plane_manager[page_address.ChannelID][page_address.ChipID][page_address.DieID][page_address.PlaneID];
		plane_record->Valid_pages_count++;
		plane_record->Free_pages_count--;
		page_address.BlockID = plane_record->Data_wf[stream_id]->BlockID;
		page_address.PageID = plane_record->Data_wf[stream_id]->Current_page_write_index++;
		program_transaction_issued(page_address);
		//if (page_address.ChannelID == 2 && page_address.ChipID == 0 && page_address.DieID == 0 && page_address.PlaneID == 0 && page_address.BlockID == 0) {
		//	std::cout << "看看这块上有什么写请求：user—write-Allocate_block_and_page_in_plane_for_user_write" << page_address.PageID << std::endl;
		//}

		//if (page_address.ChannelID == 2 && page_address.ChipID == 0 && page_address.DieID == 0 && page_address.PlaneID == 0 && page_address.BlockID == 510 && page_address.PageID == 127) {
		//	std::cout << "分配第510块，127页时，要checkgc" << std::endl;
		//}

		/*Inconsistency*/this->gc_and_wl_unit->flash_controller->Change_flash_page_status_for_preconditioning(page_address, lpa);
		if (qstr_med) {
			qstr_med->Observe_program(page_address);
		}

		/*if (page_address.ChannelID == 2 && page_address.ChipID == 1 && page_address.DieID == 0 && page_address.PlaneID == 1 && page_address.BlockID == 0 && page_address.PageID == 122) {
			std::cout << "online-写第0块，122页，要checkgc" << std::endl;
		}*/
		//if (lpa == 188714) {
		//	std::cout << "online_read写走188714" << std::endl;
		//}
		//The current write frontier block is written to the end
		if (plane_record->Data_wf[stream_id]->Current_page_write_index == pages_no_per_block) {
			//Assign a new write frontier block
			plane_record->Data_wf[stream_id] = next_write_frontier(*plane_record, page_address, stream_id, QSTR_MED::Destination::HOST);
			gc_and_wl_unit->Check_gc_required(plane_record->Get_free_block_pool_size(), page_address);
		}

		plane_record->Check_bookkeeping_correctness(page_address);
	}
	
	//void Flash_Block_Manager::Allocate_block_and_page_in_plane_for_gc_write(const stream_id_type stream_id, NVM::FlashMemory::Physical_Page_Address& page_address)
	/*Inconsistency*/void Flash_Block_Manager::Allocate_block_and_page_in_plane_for_gc_write(const stream_id_type stream_id, NVM::FlashMemory::Physical_Page_Address& page_address, LPA_type lpa)
	{
		PlaneBookKeepingType* plane_record = &plane_manager[page_address.ChannelID][page_address.ChipID][page_address.DieID][page_address.PlaneID];
		plane_record->Valid_pages_count++;
		plane_record->Free_pages_count--;

		page_address.BlockID = plane_record->GC_wf[stream_id]->BlockID;
		page_address.PageID = plane_record->GC_wf[stream_id]->Current_page_write_index++;



		program_transaction_issued(page_address);

		/*Inconsistency*///this->gc_and_wl_unit->flash_controller->Change_fake_flash_page_status_for_preconditioning(page_address, lpa);

		//if (page_address.ChannelID == 1 && page_address.ChipID == 0 && page_address.DieID == 1 && page_address.PlaneID == 0 && page_address.BlockID == 0) {
		//	std::cout << "gc-写第0块，122页，要checkgc,lpa为：" << lpa << std::endl;
		//}
		/*if (lpa == 1074065) {
			std::cout << "gc写该数据，地址分配为：" << page_address.ChannelID << page_address.ChipID << page_address.DieID << page_address.PlaneID << page_address.BlockID << "|" << page_address.PageID << std::endl;
		}*/

		//The current write frontier block is written to the end
		if (plane_record->GC_wf[stream_id]->Current_page_write_index == pages_no_per_block) {
			//Assign a new write frontier block
			plane_record->GC_wf[stream_id] = next_write_frontier(*plane_record, page_address, stream_id, QSTR_MED::Destination::GC);

			/*if (page_address.ChannelID == 1 && page_address.ChipID == 0 && page_address.DieID == 1 && page_address.PlaneID == 0 && plane_record->GC_wf[stream_id]->BlockID == 0) {
				std::cout << "gc池选中了一个新的，第0块" << std::endl;
			}*/

			gc_and_wl_unit->Check_gc_required(plane_record->Get_free_block_pool_size(), page_address);
		}
		plane_record->Check_bookkeeping_correctness(page_address);
	}

	void Flash_Block_Manager::Allocate_Pages_in_block_and_invalidate_remaining_for_preconditioning(const stream_id_type stream_id, const NVM::FlashMemory::Physical_Page_Address& plane_address, std::vector<NVM::FlashMemory::Physical_Page_Address>& page_addresses)
	{
		if(page_addresses.size() > pages_no_per_block) {
			PRINT_ERROR("Error while precondition a physical block: the size of the address list is larger than the pages_no_per_block!")
		}
			
		PlaneBookKeepingType *plane_record = &plane_manager[plane_address.ChannelID][plane_address.ChipID][plane_address.DieID][plane_address.PlaneID];
		if (plane_record->Data_wf[stream_id]->Current_page_write_index > 0) {
			PRINT_ERROR("Illegal operation: the Allocate_Pages_in_block_and_invalidate_remaining_for_preconditioning function should be executed for an erased block!")
		}

		//Assign physical addresses
		for (int i = 0; i < page_addresses.size(); i++) {
			plane_record->Valid_pages_count++;
			plane_record->Free_pages_count--;

			page_addresses[i].BlockID = plane_record->Data_wf[stream_id]->BlockID;
			page_addresses[i].PageID = plane_record->Data_wf[stream_id]->Current_page_write_index++;

			/*if (plane_address.ChannelID == 2 && plane_address.ChipID == 0 && plane_address.DieID == 0 && plane_address.PlaneID == 0 && plane_record->Data_wf[stream_id]->BlockID == 0) {
				std::cout << "开始填2,0,0,0,0块,第" << page_addresses[i].PageID << "页" << std::endl;
			}*/
			plane_record->Check_bookkeeping_correctness(page_addresses[i]);
		}


		//Invalidate the remaining pages in the block
		NVM::FlashMemory::Physical_Page_Address target_address(plane_address);
		while (plane_record->Data_wf[stream_id]->Current_page_write_index < pages_no_per_block) {
			plane_record->Free_pages_count--;
			target_address.BlockID = plane_record->Data_wf[stream_id]->BlockID;
			target_address.PageID = plane_record->Data_wf[stream_id]->Current_page_write_index++;
			Invalidate_page_in_block_for_preconditioning(stream_id, target_address);
			plane_record->Check_bookkeeping_correctness(plane_address);
		}

		//Update the write frontier
		plane_record->Data_wf[stream_id] = plane_record->Get_a_free_block(stream_id, false);
	}

	//void Flash_Block_Manager::Allocate_block_and_page_in_plane_for_translation_write(const stream_id_type streamID, NVM::FlashMemory::Physical_Page_Address& page_address, bool is_for_gc)
	/*Inconsistency*/void Flash_Block_Manager::Allocate_block_and_page_in_plane_for_translation_write(const stream_id_type streamID, NVM::FlashMemory::Physical_Page_Address& page_address, bool is_for_gc, LPA_type mvpn)
	{
		PlaneBookKeepingType *plane_record = &plane_manager[page_address.ChannelID][page_address.ChipID][page_address.DieID][page_address.PlaneID];
		plane_record->Valid_pages_count++;
		plane_record->Free_pages_count--;
		page_address.BlockID = plane_record->Translation_wf[streamID]->BlockID;
		page_address.PageID = plane_record->Translation_wf[streamID]->Current_page_write_index++;
		program_transaction_issued(page_address);

		//if (page_address.ChannelID == 2 && page_address.ChipID == 0 && page_address.DieID == 0 && page_address.PlaneID == 0 && page_address.BlockID == 0) {
		//	std::cout << "看看这块上有什么写请求：transalation—write-" << page_address.PageID << std::endl;
		//}

		/*Inconsistency*///this->gc_and_wl_unit->flash_controller->Change_fake_flash_page_status_for_preconditioning(page_address, mvpn);

		//The current write frontier block for translation pages is written to the end
		if (plane_record->Translation_wf[streamID]->Current_page_write_index == pages_no_per_block) {
			//Assign a new write frontier block
			plane_record->Translation_wf[streamID] = next_write_frontier(*plane_record, page_address, streamID, QSTR_MED::Destination::MAPPING);
			/*if (page_address.ChannelID == 2 && page_address.ChipID == 1 && page_address.DieID == 0 && page_address.PlaneID == 1 && plane_record->GC_wf[streamID]->BlockID == 0) {
				std::cout << "取到该255块--translate_write" << std::endl;
			}*/
			if (!is_for_gc) {
				gc_and_wl_unit->Check_gc_required(plane_record->Get_free_block_pool_size(), page_address);
			}
		}
		plane_record->Check_bookkeeping_correctness(page_address);
	}

	inline void Flash_Block_Manager::Invalidate_page_in_block(const stream_id_type stream_id, const NVM::FlashMemory::Physical_Page_Address& page_address)
	{
		PlaneBookKeepingType* plane_record = &plane_manager[page_address.ChannelID][page_address.ChipID][page_address.DieID][page_address.PlaneID];
		plane_record->Invalid_pages_count++;
		plane_record->Valid_pages_count--;
		if (plane_record->Blocks[page_address.BlockID].Stream_id != stream_id) {
			PRINT_ERROR("Inconsistent status in the Invalidate_page_in_block function! The accessed block is not allocated to stream " << stream_id << "|" << plane_record->Blocks[page_address.BlockID].Stream_id)
		}
		plane_record->Blocks[page_address.BlockID].Invalid_page_count++;
		plane_record->Blocks[page_address.BlockID].Invalid_page_bitmap[page_address.PageID / 64] |= ((uint64_t)0x1) << (page_address.PageID % 64);
	}

	inline void Flash_Block_Manager::Invalidate_page_in_block_for_preconditioning(const stream_id_type stream_id, const NVM::FlashMemory::Physical_Page_Address& page_address)
	{
		PlaneBookKeepingType* plane_record = &plane_manager[page_address.ChannelID][page_address.ChipID][page_address.DieID][page_address.PlaneID];
		plane_record->Invalid_pages_count++;
		if (plane_record->Blocks[page_address.BlockID].Stream_id != stream_id) {
			PRINT_ERROR("Inconsistent status in the Invalidate_page_in_block function! The accessed block is not allocated to stream " << stream_id)
		}
		plane_record->Blocks[page_address.BlockID].Invalid_page_count++;
		plane_record->Blocks[page_address.BlockID].Invalid_page_bitmap[page_address.PageID / 64] |= ((uint64_t)0x1) << (page_address.PageID % 64);
	}

	void Flash_Block_Manager::Add_erased_block_to_pool(const NVM::FlashMemory::Physical_Page_Address& block_address)
	{
		PlaneBookKeepingType *plane_record = &plane_manager[block_address.ChannelID][block_address.ChipID][block_address.DieID][block_address.PlaneID];
		Block_Pool_Slot_Type* block = &(plane_record->Blocks[block_address.BlockID]);
		plane_record->Free_pages_count += block->Invalid_page_count;
		plane_record->Invalid_pages_count -= block->Invalid_page_count;

		Stats::Block_erase_histogram[block_address.ChannelID][block_address.ChipID][block_address.DieID][block_address.PlaneID][block->Erase_count]--;
		
		/*if (block_address.ChannelID == 1 && block_address.ChipID == 0 && block_address.DieID == 1 && block_address.PlaneID == 0 && block_address.BlockID == 0) {
			std::cout << "擦除1|0|1|0|0块" << std::endl;
		}*/

		if (qstr_med) {
			qstr_med->Clear_open_block_observations(block_address);
		}
		block->Erase();

		Stats:: Block_erase_histogram[block_address.ChannelID][block_address.ChipID][block_address.DieID][block_address.PlaneID][block->Erase_count]++;

		/**/
		if (block->Erase_count == 500) {
			Stats::Trace_End_Flag = 1;
		}
		if(block->Erase_count<=250 && SSD_Components::Stats::flag==1){
			SSD_Components::Stats::stage1_end_count=SSD_Components::Stats::now_count;
			SSD_Components::Stats::stage=1;
		}
		if(block->Erase_count>250 && block->Erase_count<=375 && SSD_Components::Stats::flag!=3){
			SSD_Components::Stats::stage2_end_count=SSD_Components::Stats::now_count;
			SSD_Components::Stats::flag=2;
			SSD_Components::Stats::stage=2;
		}
		if(block->Erase_count>375 ){
			
			SSD_Components::Stats::flag=3;
			SSD_Components::Stats::stage=3;
			
		}
		plane_record->Add_to_free_block_pool(block, gc_and_wl_unit->Use_dynamic_wearleveling());
		plane_record->Check_bookkeeping_correctness(block_address);
	}

	inline unsigned int Flash_Block_Manager::Get_pool_size(const NVM::FlashMemory::Physical_Page_Address& plane_address)
	{
		return (unsigned int) plane_manager[plane_address.ChannelID][plane_address.ChipID][plane_address.DieID][plane_address.PlaneID].Free_block_pool.size();
	}
}
