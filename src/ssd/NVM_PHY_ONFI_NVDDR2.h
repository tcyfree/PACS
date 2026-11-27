#ifndef NVM_PHY_ONFI_NVDDR2_H
#define NVM_PHY_ONFI_NVDDR2_H

#include <queue>
#include <list>
#include <memory>
#include "../sim/Sim_Defs.h"
#include "../nvm_chip/flash_memory/FlashTypes.h"
#include "../nvm_chip/flash_memory/Flash_Command.h"
#include "NVM_PHY_ONFI.h"
#include "ONFI_Channel_NVDDR2.h"
#include "Flash_Transaction_Queue.h"
#include "PACS.h"

namespace SSD_Components
{
	enum class NVDDR2_SimEventType
	{
		READ_DATA_TRANSFERRED, READ_CMD_ADDR_TRANSFERRED,
		PROGRAM_CMD_ADDR_DATA_TRANSFERRED, 
		PROGRAM_COPYBACK_CMD_ADDR_TRANSFERRED, 
		ERASE_SETUP_COMPLETED
	};

	class DieBookKeepingEntry
	{
	public:
		NVM::FlashMemory::Flash_Command* ActiveCommand; //The current command that is executing on the die

		/*The current transactions that are being serviced. For the set of transactions in ActiveTransactions,
		there is one ActiveCommand that is geting executed on the die. Transaction is a FTL-level concept, and
		command is a flash chip-level concept*/
		std::list<NVM_Transaction_Flash*> ActiveTransactions; 
		NVM::FlashMemory::Flash_Command* SuspendedCommand;
		std::list<NVM_Transaction_Flash*> SuspendedTransactions;
		NVM_Transaction_Flash* ActiveTransfer; //The current transaction 
		bool Free;
		bool Suspended;
		sim_time_type Expected_finish_time;
		sim_time_type Program_observation_start = INVALID_TIME;
		sim_time_type PACS_program_start = T0;
		std::array<sim_time_type, 3> PACS_queued_programs{};
		sim_time_type PACS_queued_fixed_work = 0;
		sim_time_type PACS_queued_program_time = 0;
		sim_time_type PACS_queued_program_count = 0;
		struct Queue_Statistics {
			sim_time_type last_update = 0;
			uint64_t queued = 0, queued_user_writes = 0, dispatched = 0, dispatched_user_writes = 0;
			long double area = 0, user_write_area = 0;
			void Update(bool enqueue, bool user_write, sim_time_type now);
		} queue_stats;
		sim_time_type RemainingExecTime;
		sim_time_type DieInterleavedTime;//If the command transfer is done in die-interleaved mode, the transfer time is recorded in this temporary variable

		void PrepareSuspend()
		{
			SuspendedCommand = ActiveCommand;
			RemainingExecTime = Expected_finish_time - Simulator->Time();
			SuspendedTransactions.insert(SuspendedTransactions.begin(), ActiveTransactions.begin(), ActiveTransactions.end());
			Suspended = true;
			ActiveCommand = NULL;
			ActiveTransactions.clear();
			Free = true;
		}

		void PrepareResume()
		{
			ActiveCommand = SuspendedCommand;
			Expected_finish_time = Simulator->Time() + RemainingExecTime;
			ActiveTransactions.insert(ActiveTransactions.begin(), SuspendedTransactions.begin(), SuspendedTransactions.end());
			Suspended = false;
			SuspendedCommand = NULL;
			SuspendedTransactions.clear();
			Free = false;
		}

		void ClearCommand()
		{
			delete ActiveCommand;
			ActiveCommand = NULL;
			ActiveTransactions.clear();
			Program_observation_start = INVALID_TIME;
			Free = true;
		}
	};

	class ChipBookKeepingEntry
	{
	public:
		ChipStatus Status;
		DieBookKeepingEntry* Die_book_keeping_records;
		sim_time_type Expected_command_exec_finish_time;
		sim_time_type Last_transfer_finish_time;
		bool HasSuspend;
		/**/ //bool SuspendLock; //void suspend on relate read
		std::queue<DieBookKeepingEntry*> OngoingDieCMDTransfers;
		unsigned int WaitingReadTXCount;
		unsigned int No_of_active_dies;

		void PrepareSuspend() { HasSuspend = true; No_of_active_dies = 0; }
		void PrepareResume() { HasSuspend = false;}

		/**/	//void PrepareResume() { HasSuspend = false; No_of_active_dies++; }
	};

	class NVM_PHY_ONFI_NVDDR2 : public NVM_PHY_ONFI
	{
	public:
		NVM_PHY_ONFI_NVDDR2(const sim_object_id_type& id, ONFI_Channel_NVDDR2** channels,
			unsigned int ChannelCount, unsigned int chip_no_per_channel, unsigned int DieNoPerChip, unsigned int PlaneNoPerDie,
			bool pacs_enabled = false, bool pacs_request_level = false, bool pacs_cross_channel = false,
			PACS_Adaptive_Options pacs_adaptive = {}, bool workload_intensity_stats = false);
		PACS* Get_pacs() const { return pacs.get(); }
		struct PACS_Read_Timing {
			sim_time_type command, execution, data_out;
		};
		bool Tracks_flash_queue() const { return track_flash_queue; }
		bool Workload_intensity_stats_enabled() const { return workload_intensity_stats; }
		void Update_flash_queue(const NVM_Transaction_Flash& transaction, bool enqueue);
		struct Die_Load_Snapshot { bool idle; double ready_time; };
		Die_Load_Snapshot Get_die_load_snapshot(const NVM::FlashMemory::Physical_Page_Address& address) const;
		void Record_write_load(bool multipage, bool all_idle, double load_gap);
		void Record_write_placement(bool multipage, std::size_t pages, std::size_t crossed_pages);
		void Report_workload_intensity(Utils::XmlWriter& xmlwriter) const;
		double Estimate_pacs_die_ready(const NVM::FlashMemory::Physical_Page_Address& address, bool use_average) const;
		sim_time_type Estimate_pacs_channel_ready(flash_channel_ID_type channel) const;
		double Estimate_pacs_page_program(const NVM::FlashMemory::Physical_Page_Address& address, flash_page_ID_type page_id,
			bool use_average) const;
		sim_time_type Estimate_pacs_write_transfer(flash_channel_ID_type channel, unsigned int bytes) const;
		PACS_Read_Timing Estimate_pacs_read(const NVM::FlashMemory::Physical_Page_Address& address, unsigned int bytes) const;
		void Setup_triggers();
		void Validate_simulation_config();
		void Start_simulation();

		void Send_command_to_chip(std::list<NVM_Transaction_Flash*>& transactionList);
		void Change_flash_page_status_for_preconditioning(const NVM::FlashMemory::Physical_Page_Address& page_address, const LPA_type lpa);

		/*Inconsistency*/void Change_fake_flash_page_status_for_preconditioning(const NVM::FlashMemory::Physical_Page_Address& page_address, const LPA_type lpa);

		void Execute_simulator_event(MQSimEngine::Sim_Event*);
		BusChannelStatus Get_channel_status(flash_channel_ID_type channelID);
		NVM::FlashMemory::Flash_Chip* Get_chip(flash_channel_ID_type channel_id, flash_chip_ID_type chip_id);
		LPA_type Get_metadata(flash_channel_ID_type channe_id, flash_chip_ID_type chip_id, flash_die_ID_type die_id, flash_plane_ID_type plane_id, flash_block_ID_type block_id, flash_page_ID_type page_id);//A simplification to decrease the complexity of GC execution! The GC unit may need to know the metadata of a page to decide if a page is valid or invalid. 
		
		void Set_metadata(flash_channel_ID_type channe_id, flash_chip_ID_type chip_id, flash_die_ID_type die_id, flash_plane_ID_type plane_id, flash_block_ID_type block_id, flash_page_ID_type page_id, LPA_type LPA);
		
		/*Inconsistency*/LPA_type Get_fake_metadata(flash_channel_ID_type channe_id, flash_chip_ID_type chip_id, flash_die_ID_type die_id, flash_plane_ID_type plane_id, flash_block_ID_type block_id, flash_page_ID_type page_id);//A simplification to decrease the complexity of GC execution! The GC unit may need to know the metadata of a page to decide if a page is valid or invalid. 
		
		bool HasSuspendedCommand(NVM::FlashMemory::Flash_Chip* chip);
		ChipStatus GetChipStatus(NVM::FlashMemory::Flash_Chip* chip);
		sim_time_type Expected_finish_time(NVM::FlashMemory::Flash_Chip* chip);
		sim_time_type Expected_finish_time(NVM_Transaction_Flash* transaction);
		sim_time_type Expected_transfer_time(NVM_Transaction_Flash* transaction);
		NVM_Transaction_Flash* Is_chip_busy_with_stream(NVM_Transaction_Flash* transaction);
		bool Is_chip_busy(NVM_Transaction_Flash* transaction);
		void Change_memory_status_preconditioning(const NVM::NVM_Memory_Address* address, const void* status_info);

		/**/	//bool CheckSuspendLock(NVM::FlashMemory::Flash_Chip* chip);

	private:
		double estimate_pacs_program_for_type(const NVM::FlashMemory::Physical_Page_Address& address, unsigned int page_type) const;
		void transfer_read_data_from_chip(ChipBookKeepingEntry* chipBKE, DieBookKeepingEntry* dieBKE, NVM_Transaction_Flash* tr);
		void perform_interleaved_cmd_data_transfer(NVM::FlashMemory::Flash_Chip* chip, DieBookKeepingEntry* bookKeepingEntry);
		void send_resume_command_to_chip(NVM::FlashMemory::Flash_Chip* chip, ChipBookKeepingEntry* chipBKE);
		static void handle_ready_signal_from_chip(NVM::FlashMemory::Flash_Chip* chip, NVM::FlashMemory::Flash_Command* command);

		static NVM_PHY_ONFI_NVDDR2* _my_instance;
		ONFI_Channel_NVDDR2** channels;
		std::unique_ptr<PACS> pacs;
		const bool workload_intensity_stats, track_flash_queue;
		struct Write_Load_Statistics {
			uint64_t observed = 0, all_idle = 0, gap_under_460us = 0;
			uint64_t placed_requests = 0, placed_pages = 0, crossed_requests = 0, crossed_pages = 0;
		};
		std::array<Write_Load_Statistics, 2> write_load_stats;
		ChipBookKeepingEntry** bookKeepingTable;
		Flash_Transaction_Queue *WaitingReadTX, *WaitingGCRead_TX, *WaitingMappingRead_TX;
		std::list<DieBookKeepingEntry*> *WaitingCopybackWrites;
	};
}

#endif // !NVM_PHY_ONFI_NVDDR2_H
