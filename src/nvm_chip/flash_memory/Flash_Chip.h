#ifndef FLASH_CHIP_H
#define FLASH_CHIP_H

#include <algorithm>
#include <memory>
#include "../../sim/Sim_Defs.h"
#include "../../sim/Sim_Event.h"
#include "../../sim/Engine.h"
#include "../../sim/Sim_Reporter.h"
#include "../NVM_Chip.h"
#include "FlashTypes.h"
#include "Die.h"
#include "Flash_Command.h"
#include "Block_Latency_Profile.h"
#include <vector>
#include <stdexcept>

namespace NVM
{
	namespace FlashMemory
	{
		class Flash_Chip : public NVM_Chip
		{
			enum class Internal_Status { IDLE, BUSY };
			enum class Chip_Sim_Event_Type { COMMAND_FINISHED };
		public:
			Flash_Chip(const sim_object_id_type&, flash_channel_ID_type channelID, flash_chip_ID_type localChipID,
				Flash_Technology_Type flash_technology, 
				unsigned int dieNo, unsigned int PlaneNoPerDie, unsigned int Block_no_per_plane, unsigned int Page_no_per_block,
				sim_time_type *readLatency, sim_time_type *programLatency, const std::vector<sim_time_type>& wordlineProgramLatencies, sim_time_type eraseLatency,
				sim_time_type suspendProgramLatency, sim_time_type suspendEraseLatency,
				sim_time_type commProtocolDelayRead = 20, sim_time_type commProtocolDelayWrite = 0, sim_time_type commProtocolDelayErase = 0);
			~Flash_Chip();
			flash_channel_ID_type ChannelID;
			flash_chip_ID_type ChipID;         //Flashchip position in its related channel

			void Set_block_latency_profile(const std::shared_ptr<const Block_Latency_Profile>& profile)
			{
				block_latency_profile = profile;
			}

			void StartCMDXfer()
			{
				this->lastTransferStart = Simulator->Time();
			}
			void StartCMDDataInXfer()
			{
				this->lastTransferStart = Simulator->Time();
			}
			void StartDataOutXfer()
			{
				this->lastTransferStart = Simulator->Time();
			}
			void EndCMDXfer(Flash_Command* command)//End transferring write data to the Flash chip
			{
				this->STAT_totalXferTime += (Simulator->Time() - this->lastTransferStart);
				if (this->idleDieNo != die_no)
					STAT_totalOverlappedXferExecTime += (Simulator->Time() - lastTransferStart);
				this->Dies[command->Address[0].DieID]->STAT_TotalXferTime += (Simulator->Time() - lastTransferStart);

				start_command_execution(command);

				this->lastTransferStart = INVALID_TIME;
			}
			void EndCMDDataInXfer(Flash_Command* command)//End transferring write data of a group of multi-plane transactions to the Flash chip
			{
				this->STAT_totalXferTime += (Simulator->Time() - this->lastTransferStart);
				if (this->idleDieNo != die_no)
					STAT_totalOverlappedXferExecTime += (Simulator->Time() - lastTransferStart);
				this->Dies[command->Address[0].DieID]->STAT_TotalXferTime += (Simulator->Time() - lastTransferStart);

				start_command_execution(command);

				this->lastTransferStart = INVALID_TIME;
			}
			void EndDataOutXfer(Flash_Command* command)
			{
				this->STAT_totalXferTime += (Simulator->Time() - this->lastTransferStart);
				if (this->idleDieNo != die_no)
					STAT_totalOverlappedXferExecTime += (Simulator->Time() - lastTransferStart);
				this->Dies[command->Address[0].DieID]->STAT_TotalXferTime += (Simulator->Time() - lastTransferStart);

				this->lastTransferStart = INVALID_TIME;
			}
			void Change_memory_status_preconditioning(const NVM_Memory_Address* address, const void* status_info);

			/*Inconsistency*/void Change_fake_memory_status_preconditioning(const NVM_Memory_Address* address, const void* status_info);

			void Start_simulation();
			void Validate_simulation_config();
			void Setup_triggers();
			void Execute_simulator_event(MQSimEngine::Sim_Event*);
			typedef void(*ChipReadySignalHandlerType) (Flash_Chip* targetChip, Flash_Command* command);
			void Connect_to_chip_ready_signal(ChipReadySignalHandlerType);
			
			unsigned int Get_page_type(flash_page_ID_type pageID) const
			{
				if (flash_technology == Flash_Technology_Type::MLC) {
					return pageID % 2;
				}
				if (flash_technology == Flash_Technology_Type::TLC) {
					//From: Yaakobi et al., "Characterization and Error-Correcting Codes for TLC Flash Memories", ICNC 2012
					return (pageID <= 5) ? 0 : ((pageID <= 7) ? 1 : (((pageID - 8) >> 1) % 3));
				}
				return 0;
			}

			sim_time_type Get_read_latency(flash_page_ID_type page_id) const
			{
				return _readLatency[Get_page_type(page_id)] + _RBSignalDelayRead;
			}

			sim_time_type Get_nominal_program_latency(unsigned int page_type) const
			{
				return _programLatency[page_type] + _RBSignalDelayWrite;
			}

			sim_time_type Get_configured_program_latency(flash_page_ID_type page_id) const
			{
				return get_configured_program_time(page_id) + _RBSignalDelayWrite;
			}

			sim_time_type Get_nominal_erase_latency() const { return _eraseLatency + _RBSignalDelayErase; }

			sim_time_type Get_die_execution_time(flash_die_ID_type die_id) const
			{
				const auto* die = Dies[die_id];
				return die->STAT_TotalReadTime + die->STAT_TotalProgramTime + die->STAT_TotalEraseTime;
			}

			sim_time_type Get_command_execution_latency(const Flash_Command& command)
			{
				switch (command.CommandCode)
				{
					case CMD_READ_PAGE:
					case CMD_READ_PAGE_MULTIPLANE:
					case CMD_READ_PAGE_COPYBACK:
					case CMD_READ_PAGE_COPYBACK_MULTIPLANE: {
						sim_time_type latency = 0;
						for (const auto& address : command.Address) {
							latency = std::max(latency, _readLatency[Get_page_type(address.PageID)]);
						}
						return latency + _RBSignalDelayRead;
					}
					case CMD_PROGRAM_PAGE:
					case CMD_PROGRAM_PAGE_MULTIPLANE:
					case CMD_PROGRAM_PAGE_COPYBACK:
					case CMD_PROGRAM_PAGE_COPYBACK_MULTIPLANE: {
						// Consecutive pages share a wordline. A multi-plane command waits for its slowest plane.
						const auto pages_per_wordline = static_cast<unsigned int>(flash_technology);
						sim_time_type latency = 0;
						for (const auto& address : command.Address) {
							const auto page_time = block_latency_profile
								? block_latency_profile->Program_times(address)[address.PageID / pages_per_wordline]
								: get_configured_program_time(address.PageID);
							latency = std::max(latency, page_time);
						}
						return latency + _RBSignalDelayWrite;
					}
					case CMD_ERASE_BLOCK:
					case CMD_ERASE_BLOCK_MULTIPLANE: {
						if (!block_latency_profile) {
							return _eraseLatency + _RBSignalDelayErase;
						}
						sim_time_type latency = 0;
						for (const auto& address : command.Address) {
							latency = std::max(latency, block_latency_profile->Erase_time(address));
						}
						return latency + _RBSignalDelayErase;
					}
					default:
						throw std::invalid_argument("Unsupported command for flash chip.");
				}
			}

			void Suspend(flash_die_ID_type dieID);
			void Resume(flash_die_ID_type dieID);
			sim_time_type GetSuspendProgramTime();
			sim_time_type GetSuspendEraseTime();
			void Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter);
			LPA_type Get_metadata(flash_die_ID_type die_id, flash_plane_ID_type plane_id, flash_block_ID_type block_id, flash_page_ID_type page_id);//A simplification to decrease the complexity of GC execution! The GC unit may need to know the metadata of a page to decide if a page is valid or invalid. 
			
			void Set_metadata(flash_die_ID_type die_id, flash_plane_ID_type plane_id, flash_block_ID_type block_id, flash_page_ID_type page_id, LPA_type LPA);
			
			/*Inconsistency*/LPA_type Get_fake_metadata(flash_die_ID_type die_id, flash_plane_ID_type plane_id, flash_block_ID_type block_id, flash_page_ID_type page_id);//A simplification to decrease the complexity of GC execution! The GC unit may need to know the metadata of a page to decide if a page is valid or invalid. 

			/**/unsigned int get_die_no_in_chip();
			/**/unsigned int get_plane_no_in_die();
			/**/unsigned int get_block_no_in_plane();
				
		private:
			sim_time_type get_configured_program_time(flash_page_ID_type page_id) const
			{
				return _wordlineProgramLatencies.empty() ? _programLatency[Get_page_type(page_id)]
					: _wordlineProgramLatencies[page_id / static_cast<unsigned int>(flash_technology)];
			}

			Flash_Technology_Type flash_technology;
			Internal_Status status;
			unsigned int idleDieNo;
			Die** Dies;
			unsigned int die_no;
			unsigned int plane_no_in_die;                  //indicate how many planes in a die
			unsigned int block_no_in_plane;                //indicate how many blocks in a plane
			unsigned int page_no_per_block;                 //indicate how many pages in a block
			const std::vector<sim_time_type> _wordlineProgramLatencies;
			std::shared_ptr<const Block_Latency_Profile> block_latency_profile;
			sim_time_type *_readLatency, *_programLatency, _eraseLatency;
			sim_time_type _suspendProgramLatency, _suspendEraseLatency;
			sim_time_type _RBSignalDelayRead, _RBSignalDelayWrite, _RBSignalDelayErase;
			sim_time_type lastTransferStart;
			sim_time_type executionStartTime, expectedFinishTime;

			unsigned long STAT_readCount, STAT_progamCount, STAT_eraseCount;
			unsigned long STAT_totalSuspensionCount, STAT_totalResumeCount;
			sim_time_type STAT_totalExecTime, STAT_totalXferTime, STAT_totalOverlappedXferExecTime;

			void start_command_execution(Flash_Command* command);
			void finish_command_execution(Flash_Command* command);
			void broadcast_ready_signal(Flash_Command* command);
			std::vector<ChipReadySignalHandlerType> connectedReadyHandlers;
		};
	}
}

#endif // !FLASH_CHIP_H
