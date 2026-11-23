#include "Data_Cache_Flash.h"
#include <assert.h>
#include "Stats.h"
#include "TSU_Priority_OutOfOrder.h"


namespace SSD_Components
{
	//Data_Cache_Flash::Data_Cache_Flash(unsigned int capacity_in_pages) : capacity_in_pages(capacity_in_pages) {}
	/**/Data_Cache_Flash::Data_Cache_Flash(Flash_Block_Manager_Base* cache_block_manager, unsigned int capacity_in_pages) : cache_block_manager(cache_block_manager), capacity_in_pages(capacity_in_pages) {}

	bool Data_Cache_Flash::Exists(const stream_id_type stream_id, const LPA_type lpn)
	{
		LPA_type key = LPN_TO_UNIQUE_KEY(stream_id, lpn);
		auto it = slots.find(key);
		if (it == slots.end()) {
			return false;
		}

		return true;
	}

	/**/bool Data_Cache_Flash::ghost_Exists(const stream_id_type stream_id, const LPA_type lpn)
	{
		LPA_type key = LPN_TO_UNIQUE_KEY(stream_id, lpn);
		auto it = ghost_slots.find(key);
		if (it == ghost_slots.end()) {
			return false;
		}

		/*幽灵命中，该lpa次数+1*/
		
		return true;
	}

	Data_Cache_Flash::~Data_Cache_Flash()
	{
		for (auto &slot : slots) {
			delete slot.second;
		}
	}

	Data_Cache_Slot_Type Data_Cache_Flash::Get_slot(const stream_id_type stream_id, const LPA_type lpn)
	{
		LPA_type key = LPN_TO_UNIQUE_KEY(stream_id, lpn);
		auto it = slots.find(key);
		assert(it != slots.end());
		if (lru_list.begin()->first != key) {
			lru_list.splice(lru_list.begin(), lru_list, it->second->lru_list_ptr);
		}

		return *(it->second);
	}

	/**/Data_Cache_Slot_Type Data_Cache_Flash::Get_ghost_slot(const stream_id_type stream_id, const LPA_type lpn)
	{
		LPA_type key = LPN_TO_UNIQUE_KEY(stream_id, lpn);
		auto it = ghost_slots.find(key);
		/*如果没找到slot即遍历到了末尾，则直接报错*/
		assert(it != ghost_slots.end());
		/*找到了slot*/
		if (ghost_lru_list.begin()->first != key) {
			ghost_lru_list.splice(ghost_lru_list.begin(), ghost_lru_list, it->second->lru_list_ptr);
		}

		return *(it->second);
	}

	bool Data_Cache_Flash::Check_free_slot_availability()
	{
		return slots.size() < capacity_in_pages;
	}

	/**/bool Data_Cache_Flash::Check_free_ghost_slot_availability()
	{
		return ghost_slots.size() < capacity_in_pages;
	}

	bool Data_Cache_Flash::Check_free_slot_availability(unsigned int no_of_slots)
	{
		return slots.size() + no_of_slots <= capacity_in_pages;
	}

	bool Data_Cache_Flash::Empty()
	{
		return slots.size() == 0;
	}

	bool Data_Cache_Flash::Full()
	{
		return slots.size() == capacity_in_pages;
	}

	Data_Cache_Slot_Type Data_Cache_Flash::Evict_one_dirty_slot()
	{
		assert(slots.size() > 0);
		auto itr = lru_list.rbegin();
		while (itr != lru_list.rend()) {
			if ((*itr).second->Status == Cache_Slot_Status::DIRTY_NO_FLASH_WRITEBACK) {
				break;
			}
			itr++;
		}

		Data_Cache_Slot_Type evicted_item = *lru_list.back().second;
		if (itr == lru_list.rend()) {
			evicted_item.Status = Cache_Slot_Status::EMPTY;
			return evicted_item;
		}

		slots.erase(lru_list.back().first);
		delete lru_list.back().second;
		lru_list.pop_back();
		
		return evicted_item;
	}

	Data_Cache_Slot_Type Data_Cache_Flash::Evict_one_slot_lru()
	{
		Stats::evict_slot_total++;
		assert(slots.size() > 0);
		slots.erase(lru_list.back().first);
		Data_Cache_Slot_Type evicted_item = *lru_list.back().second;
		delete lru_list.back().second;
		lru_list.pop_back();

		return evicted_item;
	}

	/**/Data_Cache_Slot_Type Data_Cache_Flash::Evict_one_slot_lru_GCAR()
	{
		Stats::evict_slot_total++;
		assert(slots.size() > 0);


		for (auto it = lru_list.rbegin(); it != lru_list.rend(); ++it) {
			Data_Cache_Slot_Type& current_slot = *(it->second);
			NVM::FlashMemory::Physical_Page_Address current_slot_plane_address = get_plane_address(current_slot.LPA);
			if (static_cast<TSU_Priority_OutOfOrder*>(cache_block_manager->gc_and_wl_unit->tsu)->Is_safe_for_evict(current_slot_plane_address)) {
				slots.erase(it->first); // 从 slots 中删除当前元素对应的槽位
				Data_Cache_Slot_Type evicted_item = current_slot;
				delete it->second; // 释放当前元素的指针内存
				lru_list.erase((++it).base()); // 从 lru_list 中删除当前元素
				return evicted_item;
			}
		}
		
		Stats::all_chip_ongoging_erase++;
		slots.erase(lru_list.back().first);
		Data_Cache_Slot_Type evicted_item = *lru_list.back().second;
		delete lru_list.back().second;
		lru_list.pop_back();

		return evicted_item;

	}

	/**/Data_Cache_Slot_Type Data_Cache_Flash::Evict_one_slot_lru_LCR()
	{
		Stats::evict_slot_total++;
		assert(slots.size() >= 1);

		std::vector<std::pair<LPA_type, Data_Cache_Slot_Type*>> last_quarter_items;

		/*窗口大小*/
		int quarter_size = lru_list.size() / 8;
		int count = 0;


		for (auto it = lru_list.rbegin(); it != lru_list.rend(); ++it) {
			count++;
			Data_Cache_Slot_Type& current_slot = *(it->second);


			if (count <= quarter_size) {
				last_quarter_items.emplace_back(current_slot.LPA, &current_slot);

				if (current_slot.Status == Cache_Slot_Status::CLEAN) {
					Stats::evict_slot_back++;

					slots.erase(it->first); // 从 slots 中删除当前元素对应的槽位
					Data_Cache_Slot_Type evicted_item = current_slot;
					delete it->second; // 释放当前元素的指针内存
					lru_list.erase((++it).base()); // 从 lru_list 中删除当前元素
					return evicted_item;
				}
				else
				{
					continue;
				}
			}
			else {
				break;
			}
		}

		/*没有干净数据，找IO最小的数据*/

		Stats::evict_slot_io++;

		//从vector中找IO队列最短的数据

		Data_Cache_Slot_Type* evicted_item = nullptr;

		NVM::FlashMemory::Physical_Page_Address tmp_plane_address;
		NVM::FlashMemory::Physical_Page_Address evict_plane_address;

		for (size_t i = 0; i < last_quarter_items.size(); ++i) {
			Data_Cache_Slot_Type& current_slot = *last_quarter_items[i].second;
			if (!evicted_item
			    // ||
				// Stats::IO_Traffic_Array_Chip[tmp_plane_address.ChannelID][tmp_plane_address.ChipID] <
				// Stats::IO_Traffic_Array_Chip[evict_plane_address.ChannelID][evict_plane_address.ChipID]
				) {
				evicted_item = &current_slot;
			}

			tmp_plane_address = get_plane_address(current_slot.LPA);
			evict_plane_address = get_plane_address(evicted_item->LPA);
		}

		slots.erase(evicted_item->LPA);

		int flag_ = 0;
		for (auto it = lru_list.begin(); it != lru_list.end(); ++it) {
			if (it->second == evicted_item) {
				// 找到并删除evicted_item
				lru_list.erase(it);
				flag_ = 1;
				break; // 找到后可以终止循环
			}
		}

		if (flag_ == 0) {
			PRINT_ERROR("lru_list中没有该元素，错误")
		}

		Data_Cache_Slot_Type evicted_slot = *evicted_item;
		delete evicted_item;

		return evicted_slot;
	}


	/**/Data_Cache_Slot_Type Data_Cache_Flash::Evict_one_slot_lru_GC_and_WL_Unit_Plane()
	{
		Stats::evict_slot_total++;
		assert(slots.size() >= 1);

		std::vector<std::pair<LPA_type, Data_Cache_Slot_Type*>> last_quarter_items;
		//std::list<std::pair<LPA_type, Data_Cache_Slot_Type*>> half_of_last_quarter_items;


		//获取平均值
		unsigned int Plane_average_GC_and_WL_count = cache_block_manager->get_average_plane_GC_and_WL_count();
		//遍历所有

		//新增:
		int quarter_size = lru_list.size() / 8;
		int count = 0;

		int IO_flag = 0;

		for (auto it = lru_list.rbegin(); it != lru_list.rend(); ++it) {
			/**********************新增***************/
			count++;
			Data_Cache_Slot_Type& current_slot = *(it->second);
			LPA_type current_slot_lpn = current_slot.LPA;
			NVM::FlashMemory::Physical_Page_Address current_slot_plane_address = get_plane_address(current_slot_lpn);
			PlaneBookKeepingType* current_slot_plane_record = cache_block_manager->Get_plane_bookkeeping_entry(current_slot_plane_address);


			unsigned int current_slot_GC_count = current_slot_plane_record->Total_GC_count;
			unsigned int current_slot_WL_count = current_slot_plane_record->Total_WL_count;



			if (count <= quarter_size) {
				last_quarter_items.emplace_back(current_slot.LPA, &current_slot);

				if ((current_slot_GC_count + current_slot_WL_count) <= Plane_average_GC_and_WL_count) {
					Stats::evict_slot_back++;
					
					slots.erase(it->first); // 从 slots 中删除当前元素对应的槽位
					Data_Cache_Slot_Type evicted_item = current_slot;
					delete it->second; // 释放当前元素的指针内存
					lru_list.erase((++it).base()); // 从 lru_list 中删除当前元素

					//if (evicted_item.LPA == 390780) {
					//	std::cout << "cece" << std::endl;
					//}
					return evicted_item;
				}
				else
				{
					continue;
				}
			}
			else {
				/*GC没找到，找IO最少的*/
				IO_flag = 1;
				Stats::evict_slot_io++;
				// 按 GC 次数升序排序
				// std::sort(last_quarter_items.begin(), last_quarter_items.end(),
				// 	[this](const auto& item1, const auto& item2) {
				// 		Data_Cache_Slot_Type& item1_slot = *(item1.second);
				// 		Data_Cache_Slot_Type& item2_slot = *(item2.second);

				// 		NVM::FlashMemory::Physical_Page_Address item1_slot_address = get_plane_address(item1_slot.LPA);
				// 		NVM::FlashMemory::Physical_Page_Address item2_slot_address = get_plane_address(item2_slot.LPA);
				// 		PlaneBookKeepingType* record1 = cache_block_manager->Get_plane_bookkeeping_entry(item1_slot_address);
				// 		PlaneBookKeepingType* record2 = cache_block_manager->Get_plane_bookkeeping_entry(item2_slot_address);
				// 		return (record1->Total_GC_count + record1->Total_WL_count) < (record2->Total_GC_count + record2->Total_WL_count);
				// 	});

				// 从排序后的数据中选取1/2并且 IO_Traffic 最小的数据
				size_t items_to_select = last_quarter_items.size() / 2;
				Data_Cache_Slot_Type* evicted_item = nullptr;

				for (size_t i = 0; i < items_to_select; ++i) {
					Data_Cache_Slot_Type& current_slot = *last_quarter_items[i].second;
					if (!evicted_item ||
						static_cast<TSU_Priority_OutOfOrder*>(cache_block_manager->gc_and_wl_unit->tsu)->Get_IO_Traffic_on_Plane(get_plane_address(current_slot.LPA)) <
						static_cast<TSU_Priority_OutOfOrder*>(cache_block_manager->gc_and_wl_unit->tsu)->Get_IO_Traffic_on_Plane(get_plane_address(evicted_item->LPA))) {
						evicted_item = &current_slot;
					}
				}

				slots.erase(evicted_item->LPA);

				int flag_ = 0;
				for (auto it = lru_list.begin(); it != lru_list.end(); ++it) {
					if (it->second == evicted_item) {
						// 找到并删除evicted_item
						lru_list.erase(it);
						flag_ = 1;
						break; // 找到后可以终止循环
					}
				}

				if (flag_ == 0) {
					PRINT_ERROR("lru_list中没有该元素，错误")
				}

				Data_Cache_Slot_Type evicted_slot = *evicted_item;
				delete evicted_item;

				//if (evicted_slot.LPA == 390780) {
				//	std::cout << "cece" << std::endl;
				//}

				return evicted_slot;
			}
		}

		//新策略

		

		//原策略：当没有小于GC均值的数据，则直接找一个IO最小的数据
		//if (IO_flag == 1) {
		//	Stats::evict_slot_io++;

		//	Data_Cache_Slot_Type& best_slot = *lru_list.back().second;
		//	NVM::FlashMemory::Physical_Page_Address best_slot_plane_address = get_plane_address(best_slot.LPA);
		//	unsigned int Min_IO_Traffic = static_cast<TSU_Priority_OutOfOrder*>(cache_block_manager->gc_and_wl_unit->tsu)->Get_IO_Traffic_on_Plane(best_slot_plane_address);

		//	std::list<std::pair<LPA_type, Data_Cache_Slot_Type*>>::iterator min_io_item = --lru_list.end();

		//	auto start_index = lru_list.size() * 3 / 4;
		//	auto iter = lru_list.begin();
		//	std::advance(iter, start_index);
		//	while (iter != lru_list.end()) {
		//		// 计算当前数据项的IO值
		//		Data_Cache_Slot_Type& current_slot = *(iter->second);
		//		LPA_type current_slot_lpn = current_slot.LPA;
		//		NVM::FlashMemory::Physical_Page_Address current_slot_plane_address = get_plane_address(current_slot_lpn);
		//		unsigned int IO_Traffic = static_cast<TSU_Priority_OutOfOrder*>(cache_block_manager->gc_and_wl_unit->tsu)->Get_IO_Traffic_on_Plane(current_slot_plane_address);
		//		if (IO_Traffic < Min_IO_Traffic) {
		//			Min_IO_Traffic = IO_Traffic;
		//			min_io_item = iter;
		//		}
		//		++iter;
		//	}
		//	
		//	if (min_io_item != lru_list.end()) {
		//		slots.erase(min_io_item->first);
		//		Data_Cache_Slot_Type evicted_item = *min_io_item->second;
		//		delete min_io_item->second;
		//		lru_list.erase(min_io_item);
		//		return evicted_item;
		//	}

		//}
		PRINT_ERROR("错误！")
	}

	/*NEW*/Data_Cache_Slot_Type Data_Cache_Flash::Evict_one_slot_lru_NEW_A()
	{
		Stats::evict_slot_total++;
		assert(slots.size() >= 1);

		std::vector<std::pair<LPA_type, Data_Cache_Slot_Type*>> last_quarter_items;


		//获取平均值
		unsigned int Plane_average_GC_and_WL_count = cache_block_manager->get_average_plane_GC_and_WL_count();
		//遍历所有

		//新增:
		int quarter_size = lru_list.size() / 8;
		int count = 0;

		int IO_flag = 0;

		for (auto it = lru_list.rbegin(); it != lru_list.rend(); ++it) {
			/**********************新增***************/
			count++;
			Data_Cache_Slot_Type& current_slot = *(it->second);
			LPA_type current_slot_lpn = current_slot.LPA;
			NVM::FlashMemory::Physical_Page_Address current_slot_plane_address = get_plane_address(current_slot_lpn);
			PlaneBookKeepingType* current_slot_plane_record = cache_block_manager->Get_plane_bookkeeping_entry(current_slot_plane_address);


			unsigned int current_slot_GC_count = current_slot_plane_record->Total_GC_count;
			unsigned int current_slot_WL_count = current_slot_plane_record->Total_WL_count;



			if (count <= quarter_size) {
				last_quarter_items.emplace_back(current_slot.LPA, &current_slot);

				if ((current_slot_GC_count + current_slot_WL_count) <= Plane_average_GC_and_WL_count) {
					Stats::evict_slot_back++;

					slots.erase(it->first); // 从 slots 中删除当前元素对应的槽位
					Data_Cache_Slot_Type evicted_item = current_slot;
					delete it->second; // 释放当前元素的指针内存
					lru_list.erase((++it).base()); // 从 lru_list 中删除当前元素
					return evicted_item;
				}
				else
				{
					continue;
				}
			}
			else {
				/*GC没找到，找IO最少的*/
				IO_flag = 1;
				Stats::evict_slot_io++;
				// 按 GC 次数升序排序
				//std::sort(last_quarter_items.begin(), last_quarter_items.end(),
				//	[this](const auto& item1, const auto& item2) {
				//		Data_Cache_Slot_Type& item1_slot = *(item1.second);
				//		Data_Cache_Slot_Type& item2_slot = *(item2.second);

				//		NVM::FlashMemory::Physical_Page_Address item1_slot_address = get_plane_address(item1_slot.LPA);
				//		NVM::FlashMemory::Physical_Page_Address item2_slot_address = get_plane_address(item2_slot.LPA);
				//		PlaneBookKeepingType* record1 = cache_block_manager->Get_plane_bookkeeping_entry(item1_slot_address);
				//		PlaneBookKeepingType* record2 = cache_block_manager->Get_plane_bookkeeping_entry(item2_slot_address);
				//		return (record1->Total_GC_count + record1->Total_WL_count) < (record2->Total_GC_count + record2->Total_WL_count);
				//	});

				// 从排序后的数据中选取1/2并且 IO_Traffic 最小的数据
				size_t items_to_select = last_quarter_items.size();		//不排序，直接找最小
				Data_Cache_Slot_Type* evicted_item = nullptr;

				NVM::FlashMemory::Physical_Page_Address tmp_plane_address;
				NVM::FlashMemory::Physical_Page_Address evict_plane_address;

				for (size_t i = 0; i < items_to_select; ++i) {
					Data_Cache_Slot_Type& current_slot = *last_quarter_items[i].second;
					if (!evicted_item 
					// ||
					// 	Stats::IO_Traffic_Array[tmp_plane_address.ChannelID][tmp_plane_address.ChipID][tmp_plane_address.DieID][tmp_plane_address.PlaneID] <
					// 	Stats::IO_Traffic_Array[evict_plane_address.ChannelID][evict_plane_address.ChipID][evict_plane_address.DieID][evict_plane_address.PlaneID]
						) {
						evicted_item = &current_slot;
					}
					tmp_plane_address = get_plane_address(current_slot.LPA);
					evict_plane_address = get_plane_address(evicted_item->LPA);
				}

				slots.erase(evicted_item->LPA);

				int flag_ = 0;
				for (auto it = lru_list.begin(); it != lru_list.end(); ++it) {
					if (it->second == evicted_item) {
						// 找到并删除evicted_item
						lru_list.erase(it);
						flag_ = 1;
						break; // 找到后可以终止循环
					}
				}

				if (flag_ == 0) {
					PRINT_ERROR("lru_list中没有该元素，错误")
				}

				Data_Cache_Slot_Type evicted_slot = *evicted_item;
				delete evicted_item;
				return evicted_slot;
			}
		}
		PRINT_ERROR("错误！")
	}

	/**/Data_Cache_Slot_Type Data_Cache_Flash::Evict_one_ghost_slot_lru()
	{
		/*确保缓存中有数据slot*/
		assert(ghost_slots.size() > 0);

		/*erase lru列表最后一个槽位的KEY*/
		ghost_slots.erase(ghost_lru_list.back().first);
		/*新建变量来接收被淘汰槽位的内容*/
		Data_Cache_Slot_Type evicted_item = *ghost_lru_list.back().second;
		/*释放该槽的指针并删除该槽位*/
		delete ghost_lru_list.back().second;
		ghost_lru_list.pop_back();

		//std::cout << "删除后还剩下" << ghost_slots.size() << std::endl;

		return evicted_item;
	}

	

	void Data_Cache_Flash::Change_slot_status_to_writeback(const stream_id_type stream_id, const LPA_type lpn)
	{
		LPA_type key = LPN_TO_UNIQUE_KEY(stream_id, lpn);
		auto it = slots.find(key);
		assert(it != slots.end());
		it->second->Status = Cache_Slot_Status::DIRTY_FLASH_WRITEBACK;
	}

	void Data_Cache_Flash::Insert_read_data(const stream_id_type stream_id, const LPA_type lpn, const data_cache_content_type content,
		const data_timestamp_type timestamp, const page_status_type state_bitmap_of_read_sectors)
	{
		LPA_type key = LPN_TO_UNIQUE_KEY(stream_id, lpn);
		
		if (slots.find(key) != slots.end()) {
			throw std::logic_error("Duplicate lpn insertion into data cache!");
		}
		if (slots.size() >= capacity_in_pages) {
			throw std::logic_error("Data cache overfull!");
		}

		Data_Cache_Slot_Type* cache_slot = new Data_Cache_Slot_Type();
		cache_slot->LPA = lpn;
		cache_slot->State_bitmap_of_existing_sectors = state_bitmap_of_read_sectors;
		cache_slot->Content = content;
		cache_slot->Timestamp = timestamp;
		cache_slot->Status = Cache_Slot_Status::CLEAN;
		lru_list.push_front(std::pair<LPA_type, Data_Cache_Slot_Type*>(key, cache_slot));
		cache_slot->lru_list_ptr = lru_list.begin();
		slots[key] = cache_slot;
	}

	void Data_Cache_Flash::Insert_write_data(const stream_id_type stream_id, const LPA_type lpn, const data_cache_content_type content,
		const data_timestamp_type timestamp, const page_status_type state_bitmap_of_write_sectors)
	{
		LPA_type key = LPN_TO_UNIQUE_KEY(stream_id, lpn);
		
		if (slots.find(key) != slots.end()) {
			throw std::logic_error("Duplicate lpn insertion into data cache!!");
		}
		
		if (slots.size() >= capacity_in_pages) {
			throw std::logic_error("Data cache overfull!");
		}

		Data_Cache_Slot_Type* cache_slot = new Data_Cache_Slot_Type();
		cache_slot->LPA = lpn;
		cache_slot->State_bitmap_of_existing_sectors = state_bitmap_of_write_sectors;
		cache_slot->Content = content;
		cache_slot->Timestamp = timestamp;
		cache_slot->Status = Cache_Slot_Status::DIRTY_NO_FLASH_WRITEBACK;
		lru_list.push_front(std::pair<LPA_type, Data_Cache_Slot_Type*>(key, cache_slot));
		cache_slot->lru_list_ptr = lru_list.begin();
		slots[key] = cache_slot;
	}

	/**/void Data_Cache_Flash::Insert_write_data_ghost(const stream_id_type stream_id, const LPA_type lpn, const data_cache_content_type content,
		const data_timestamp_type timestamp, const page_status_type state_bitmap_of_write_sectors)
	{
		LPA_type key = LPN_TO_UNIQUE_KEY(stream_id, lpn);

		if (ghost_slots.find(key) != ghost_slots.end()) {
			throw std::logic_error("Duplicate lpn insertion into data cache!!");
		}

		if (ghost_slots.size() >= capacity_in_pages) {
			throw std::logic_error("Data cache overfull!");
		}

		Data_Cache_Slot_Type* cache_slot = new Data_Cache_Slot_Type();

		cache_slot->LPA = lpn;
		cache_slot->State_bitmap_of_existing_sectors = state_bitmap_of_write_sectors;
		cache_slot->Content = content;
		cache_slot->Timestamp = timestamp;
		cache_slot->Status = Cache_Slot_Status::DIRTY_NO_FLASH_WRITEBACK;
		cache_slot->hit_times = 0;
		ghost_lru_list.push_front(std::pair<LPA_type, Data_Cache_Slot_Type*>(key, cache_slot));
		cache_slot->lru_list_ptr = ghost_lru_list.begin();
		ghost_slots[key] = cache_slot;
	}

	void Data_Cache_Flash::Update_data(const stream_id_type stream_id, const LPA_type lpn, const data_cache_content_type content,
		const data_timestamp_type timestamp, const page_status_type state_bitmap_of_write_sectors)
	{
		LPA_type key = LPN_TO_UNIQUE_KEY(stream_id, lpn);
		auto it = slots.find(key);
		assert(it != slots.end());

		it->second->LPA = lpn;
		it->second->State_bitmap_of_existing_sectors = state_bitmap_of_write_sectors;
		it->second->Content = content;
		it->second->Timestamp = timestamp;
		it->second->Status = Cache_Slot_Status::DIRTY_NO_FLASH_WRITEBACK;
		if (lru_list.begin()->first != key) {
			lru_list.splice(lru_list.begin(), lru_list, it->second->lru_list_ptr);
		}
	}

	/**/void Data_Cache_Flash::Update_data_ghost(const stream_id_type stream_id, const LPA_type lpn, const data_cache_content_type content,
		const data_timestamp_type timestamp, const page_status_type state_bitmap_of_write_sectors)
	{
		LPA_type key = LPN_TO_UNIQUE_KEY(stream_id, lpn);
		auto it = ghost_slots.find(key);
		assert(it != ghost_slots.end());

		it->second->LPA = lpn;
		it->second->State_bitmap_of_existing_sectors = state_bitmap_of_write_sectors;
		it->second->Content = content;
		it->second->Timestamp = timestamp;
		it->second->hit_times = it->second->hit_times + 1;
		it->second->Status = Cache_Slot_Status::DIRTY_NO_FLASH_WRITEBACK;
		if (ghost_lru_list.begin()->first != key) {
			ghost_lru_list.splice(ghost_lru_list.begin(), ghost_lru_list, it->second->lru_list_ptr);
		}
	}

	void Data_Cache_Flash::Remove_slot(const stream_id_type stream_id, const LPA_type lpn)
	{
		LPA_type key = LPN_TO_UNIQUE_KEY(stream_id, lpn);
		auto it = slots.find(key);
		assert(it != slots.end());
		lru_list.erase(it->second->lru_list_ptr);
		delete it->second;
		slots.erase(it);
	}

	/**/NVM::FlashMemory::Physical_Page_Address Data_Cache_Flash::get_plane_address(LPA_type slot_lpn) {
		unsigned int Channel_no = cache_block_manager->get_channel_count();
		unsigned int Chip_no = cache_block_manager->get_chip_no_per_channel();
		unsigned int Die_no = cache_block_manager->get_die_no_per_chip();
		unsigned int Plane_no = cache_block_manager->get_plane_no_per_die();

		/*CWDP*/
		Flash_Plane_Allocation_Scheme_Type PlaneAllocationScheme_tmp = Flash_Plane_Allocation_Scheme_Type::CWDP;

		uint32_t ChannelId = slot_lpn % Channel_no;
		uint32_t ChipId = (slot_lpn / Channel_no) % Chip_no;
		uint32_t DieId = (slot_lpn / (Chip_no * Channel_no)) % Die_no;
		uint32_t PlaneId = (slot_lpn / (Die_no * Chip_no * Channel_no)) % Plane_no;

		switch (PlaneAllocationScheme_tmp) {
		case Flash_Plane_Allocation_Scheme_Type::CWDP:
			ChannelId = slot_lpn % Channel_no;
			ChipId = (slot_lpn / Channel_no) % Chip_no;
			DieId = (slot_lpn / (Chip_no * Channel_no)) % Die_no;
			PlaneId = (slot_lpn / (Die_no * Chip_no * Channel_no)) % Plane_no;
			break;
		case Flash_Plane_Allocation_Scheme_Type::CWPD:
			ChannelId = slot_lpn % Channel_no;
			ChipId = (slot_lpn / Channel_no) % Chip_no;
			DieId = (slot_lpn / (Channel_no * Chip_no * Plane_no)) % Die_no;
			PlaneId = (slot_lpn / (Channel_no * Chip_no)) % Plane_no;
			break;
		case Flash_Plane_Allocation_Scheme_Type::CDWP:
			ChannelId = slot_lpn % Channel_no;
			ChipId = (slot_lpn / (Die_no * Channel_no)) % Chip_no;
			DieId = (slot_lpn / Channel_no) % Die_no;
			PlaneId = (slot_lpn / (Die_no * Chip_no * Channel_no)) % Plane_no;
			break;
		case Flash_Plane_Allocation_Scheme_Type::CDPW:
			ChannelId = slot_lpn % Channel_no;
			ChipId = (slot_lpn / (Plane_no * Die_no * Channel_no)) % Chip_no;
			DieId = (slot_lpn / Channel_no) % Die_no;
			PlaneId = (slot_lpn / (Die_no * Channel_no)) % Plane_no;
			break;
		case Flash_Plane_Allocation_Scheme_Type::CPWD:
			ChannelId = slot_lpn % Channel_no;
			ChipId = (slot_lpn / (Plane_no * Channel_no)) % Chip_no;
			DieId = (slot_lpn / (Plane_no * Chip_no * Channel_no)) % Die_no;
			PlaneId = (slot_lpn / Channel_no) % Plane_no;
			break;
		case Flash_Plane_Allocation_Scheme_Type::CPDW:
			ChannelId = slot_lpn % Channel_no;
			ChipId = (slot_lpn / (Plane_no * Die_no * Channel_no)) % Chip_no;
			DieId = (slot_lpn / (Plane_no * Channel_no)) % Die_no;
			PlaneId = (slot_lpn / Channel_no) % Plane_no;
			break;
			//Static: Way first
		case Flash_Plane_Allocation_Scheme_Type::WCDP:
			ChannelId = (slot_lpn / Chip_no) % Channel_no;
			ChipId = slot_lpn % Chip_no;
			DieId = (slot_lpn / (Chip_no * Channel_no)) % Die_no;
			PlaneId = (slot_lpn / (Chip_no * Channel_no * Die_no)) % Plane_no;
			break;
		case Flash_Plane_Allocation_Scheme_Type::WCPD:
			ChannelId = (slot_lpn / Chip_no) % Channel_no;
			ChipId = slot_lpn % Chip_no;
			DieId = (slot_lpn / (Chip_no * Channel_no * Plane_no)) % Die_no;
			PlaneId = (slot_lpn / (Chip_no * Channel_no)) % Plane_no;
			break;
		case Flash_Plane_Allocation_Scheme_Type::WDCP:
			ChannelId = (slot_lpn / (Chip_no * Die_no)) % Channel_no;
			ChipId = slot_lpn % Chip_no;
			DieId = (slot_lpn / Chip_no) % Die_no;
			PlaneId = (slot_lpn / (Chip_no * Die_no * Channel_no)) % Plane_no;
			break;
		case Flash_Plane_Allocation_Scheme_Type::WDPC:
			ChannelId = (slot_lpn / (Chip_no * Die_no * Plane_no)) % Channel_no;
			ChipId = slot_lpn % Chip_no;
			DieId = (slot_lpn / Chip_no) % Die_no;
			PlaneId = (slot_lpn / (Chip_no * Die_no)) % Plane_no;
			break;
		case Flash_Plane_Allocation_Scheme_Type::WPCD:
			ChannelId = (slot_lpn / (Chip_no * Plane_no)) % Channel_no;
			ChipId = slot_lpn % Chip_no;
			DieId = (slot_lpn / (Chip_no * Plane_no * Channel_no)) % Die_no;
			PlaneId = (slot_lpn / Chip_no) % Plane_no;
			break;
		case Flash_Plane_Allocation_Scheme_Type::WPDC:
			ChannelId = (slot_lpn / (Chip_no * Plane_no * Die_no)) % Channel_no;
			ChipId = slot_lpn % Chip_no;
			DieId = (slot_lpn / (Chip_no * Plane_no)) % Die_no;
			PlaneId = (slot_lpn / Chip_no) % Plane_no;
			break;
			//Static: Die first
		case Flash_Plane_Allocation_Scheme_Type::DCWP:
			ChannelId = (slot_lpn / Die_no) % Channel_no;
			ChipId = (slot_lpn / (Die_no * Channel_no)) % Chip_no;
			DieId = slot_lpn % Die_no;
			PlaneId = (slot_lpn / (Die_no * Channel_no * Chip_no)) % Plane_no;
			break;
		case Flash_Plane_Allocation_Scheme_Type::DCPW:
			ChannelId = (slot_lpn / Die_no) % Channel_no;
			ChipId = (slot_lpn / (Die_no * Channel_no * Plane_no)) % Chip_no;
			DieId = slot_lpn % Die_no;
			PlaneId = (slot_lpn / (Die_no * Channel_no)) % Plane_no;
			break;
		case Flash_Plane_Allocation_Scheme_Type::DWCP:
			ChannelId = (slot_lpn / (Die_no * Chip_no)) % Channel_no;
			ChipId = (slot_lpn / Die_no) % Chip_no;
			DieId = slot_lpn % Die_no;
			PlaneId = (slot_lpn / (Die_no * Chip_no * Channel_no)) % Plane_no;
			break;
		case Flash_Plane_Allocation_Scheme_Type::DWPC:
			ChannelId = (slot_lpn / (Die_no * Chip_no * Plane_no)) % Channel_no;
			ChipId = (slot_lpn / Die_no) % Chip_no;
			DieId = slot_lpn % Die_no;
			PlaneId = (slot_lpn / (Die_no * Chip_no)) % Plane_no;
			break;
		case Flash_Plane_Allocation_Scheme_Type::DPCW:
			ChannelId = (slot_lpn / (Die_no * Plane_no)) % Channel_no;
			ChipId = (slot_lpn / (Die_no * Plane_no * Channel_no)) % Chip_no;
			DieId = slot_lpn % Die_no;
			PlaneId = (slot_lpn / Die_no) % Plane_no;
			break;
		case Flash_Plane_Allocation_Scheme_Type::DPWC:
			ChannelId = (slot_lpn / (Die_no * Plane_no * Chip_no)) % Channel_no;
			ChipId = (slot_lpn / (Die_no * Plane_no)) % Chip_no;
			DieId = slot_lpn % Die_no;
			PlaneId = (slot_lpn / Die_no) % Plane_no;
			break;
			//Static: Plane first
		case Flash_Plane_Allocation_Scheme_Type::PCWD:
			ChannelId = (slot_lpn / Plane_no) % Channel_no;
			ChipId = (slot_lpn / (Plane_no * Channel_no)) % Chip_no;
			DieId = (slot_lpn / (Plane_no * Channel_no * Chip_no)) % Die_no;
			PlaneId = slot_lpn % Plane_no;
			break;
		case Flash_Plane_Allocation_Scheme_Type::PCDW:
			ChannelId = (slot_lpn / Plane_no) % Channel_no;
			ChipId = (slot_lpn / (Plane_no * Channel_no * Die_no)) % Chip_no;
			DieId = (slot_lpn / (Plane_no * Channel_no)) % Die_no;
			PlaneId = slot_lpn % Plane_no;
			break;
		case Flash_Plane_Allocation_Scheme_Type::PWCD:
			ChannelId = (slot_lpn / (Plane_no * Chip_no)) % Channel_no;
			ChipId = (slot_lpn / Plane_no) % Chip_no;
			DieId = (slot_lpn / (Plane_no * Chip_no * Channel_no)) % Die_no;
			PlaneId = slot_lpn % Plane_no;
			break;
		case Flash_Plane_Allocation_Scheme_Type::PWDC:
			ChannelId = (slot_lpn / (Plane_no * Chip_no * Die_no)) % Channel_no;
			ChipId = (slot_lpn / Plane_no) % Chip_no;
			DieId = (slot_lpn / (Plane_no * Chip_no)) % Die_no;
			PlaneId = slot_lpn % Plane_no;
			break;
		case Flash_Plane_Allocation_Scheme_Type::PDCW:
			ChannelId = (slot_lpn / (Plane_no * Die_no)) % Channel_no;
			ChipId = (slot_lpn / (Plane_no * Die_no * Channel_no)) % Chip_no;
			DieId = (slot_lpn / Plane_no) % Die_no;
			PlaneId = slot_lpn % Plane_no;
			break;
		case Flash_Plane_Allocation_Scheme_Type::PDWC:
			ChannelId = (slot_lpn / (Plane_no * Die_no * Chip_no)) % Channel_no;
			ChipId = (slot_lpn / (Plane_no * Die_no)) % Chip_no;
			DieId = (slot_lpn / Plane_no) % Die_no;
			PlaneId = slot_lpn % Plane_no;
			break;
		default:
			PRINT_ERROR("Unknown plane allocation scheme type!")
		}


		NVM::FlashMemory::Physical_Page_Address plane_address(ChannelId, ChipId, DieId, PlaneId, 0, 0);
		return plane_address;
	}

	/**/bool Data_Cache_Flash::IsBelowAverageGCCount(NVM_Transaction_Flash* transaction)
	{
		unsigned int Plane_average_GC_count = cache_block_manager->get_average_plane_GC_and_WL_count();
		LPA_type current_tr_lpn = transaction->LPA;
		NVM::FlashMemory::Physical_Page_Address current_tr_plane_address = get_plane_address(current_tr_lpn);
		PlaneBookKeepingType* current_tr_plane_record = cache_block_manager->Get_plane_bookkeeping_entry(current_tr_plane_address);
		unsigned int current_tr_GC_count = current_tr_plane_record->Total_GC_count;
		unsigned int current_tr_WL_count = current_tr_plane_record->Total_WL_count;

		if ((current_tr_GC_count + current_tr_WL_count) <= Plane_average_GC_count) {
			return true;
		}
		else {
			return false;
		}
	}




	/**/bool Data_Cache_Flash::IsBelowAverageEraseCount(NVM_Transaction_Flash* transaction)
	{
		unsigned int Plane_average_Erase_count = cache_block_manager->get_average_Erase_count();
		NVM::FlashMemory::Physical_Page_Address current_tr_plane_address = get_plane_address(transaction->LPA);

		unsigned int current_Plane_Erase_Count = cache_block_manager->Get_total_erase_count_in_plane(current_tr_plane_address);


		if (current_Plane_Erase_Count < Plane_average_Erase_count) {
			return true;
		}
		else {
			return false;
		}
	}

	/**/bool Data_Cache_Flash::HasLessGCsThanLRUEnd(NVM_Transaction_Flash* transaction)
	{
		assert(slots.size() >= 1);
		Data_Cache_Slot_Type& lru_end_slot = *lru_list.back().second;
		LPA_type lru_end_slot_lpn = lru_end_slot.LPA;
		NVM::FlashMemory::Physical_Page_Address lru_end_slot_plane_address = get_plane_address(lru_end_slot_lpn);
		PlaneBookKeepingType* lru_end_slot_plane_record = cache_block_manager->Get_plane_bookkeeping_entry(lru_end_slot_plane_address);
		unsigned int lru_end_slot_GC_and_WL_count = lru_end_slot_plane_record->Total_GC_count + lru_end_slot_plane_record->Total_WL_count;


		LPA_type current_tr_lpn = transaction->LPA;
		NVM::FlashMemory::Physical_Page_Address current_tr_plane_address = get_plane_address(current_tr_lpn);
		PlaneBookKeepingType* current_tr_plane_record = cache_block_manager->Get_plane_bookkeeping_entry(current_tr_plane_address);
		unsigned int current_tr_GC_and_WL_count = current_tr_plane_record->Total_GC_count + current_tr_plane_record->Total_WL_count;

		if (lru_end_slot_GC_and_WL_count > current_tr_GC_and_WL_count) {
			return true;
		}
		else {
			return false;
		}
	}
}
