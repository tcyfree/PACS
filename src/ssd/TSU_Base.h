#ifndef TSU_H
#define TSU_H

#include <list>
#include <memory>
#include "../sim/Sim_Defs.h"
#include "../sim/Sim_Object.h"
#include "../nvm_chip/flash_memory/Flash_Chip.h"
#include "../sim/Sim_Reporter.h"
#include "FTL.h"
#include "NVM_PHY_ONFI_NVDDR2.h"
#include "Flash_Transaction_Queue.h"
#include "LEPA.h"

namespace SSD_Components
{
enum class Flash_Scheduling_Type
{
	OUT_OF_ORDER,
	PRIORITY_OUT_OF_ORDER,
	FLIN
};
class FTL;
class QSTR_MED;
class TSU_Base : public MQSimEngine::Sim_Object
{
public:
	TSU_Base(const sim_object_id_type &id, FTL *ftl, NVM_PHY_ONFI_NVDDR2 *NVMController, Flash_Scheduling_Type Type,
			 unsigned int Channel_no, unsigned int chip_no_per_channel, unsigned int DieNoPerChip, unsigned int PlaneNoPerDie,
			 sim_time_type WriteReasonableSuspensionTimeForRead,
			 sim_time_type EraseReasonableSuspensionTimeForRead,
			 sim_time_type EraseReasonableSuspensionTimeForWrite,
			 bool EraseSuspensionEnabled, bool ProgramSuspensionEnabled);
	virtual ~TSU_Base();
	void Setup_triggers();

	/*When an MQSim needs to send a set of transactions for execution, the following 
		* three funcitons should be invoked in this order:
		* Prepare_for_transaction_submit()
		* Submit_transaction(transaction)
		* .....
		* Submit_transaction(transaction)
		* Schedule()
		*
		* The above mentioned mechanism helps to exploit die-level and plane-level parallelism.
		* More precisely, first the transactions are queued and then, when the Schedule function
		* is invoked, the TSU has that opportunity to schedule them together to exploit multiplane
		* and die-interleaved execution.
		*/
	void Prepare_for_transaction_submit()
	{
		opened_scheduling_reqs++;
		if (opened_scheduling_reqs > 1)
		{
			return;
		}
		transaction_receive_slots.clear();
	}

	void Submit_transaction(NVM_Transaction_Flash *transaction)
	{
		transaction_receive_slots.push_back(transaction);
		if (auto* pacs = _NVMController->Get_pacs()) {
			if (transaction->Type == Transaction_Type::WRITE) {
				pacs->Queue_program(transaction->Address);
			}
			if (pacs->Request_level_enabled()) {
				_NVMController->Update_pacs_queue(*transaction, true);
			}
		}
		if (lepa) {
			lepa->Submit(*transaction);
		}
	}

	void Set_independent_wordline_multiplane(bool enabled) { independent_wordline_multiplane = enabled; }
	void Enable_lepa(sim_time_type read_time, sim_time_type program_time, sim_time_type erase_time, bool wordline_aware);
	LEPA* Get_lepa() const { return lepa.get(); }
	void Set_qstr_med(QSTR_MED* policy) { qstr_med = policy; }
	const QSTR_MED* Get_qstr_med() const { return qstr_med; }

	/* Shedules the transactions currently stored in inputTransactionSlots. The transactions could
		* be mixes of reads, writes, and erases.
		*/
	virtual void Schedule() = 0;
	virtual void Report_results_in_XML(std::string name_prefix, Utils::XmlWriter &xmlwriter);

protected:
	FTL *ftl;
	NVM_PHY_ONFI_NVDDR2 *_NVMController;
	Flash_Scheduling_Type type;
	unsigned int channel_count;
	unsigned int chip_no_per_channel;
	unsigned int die_no_per_chip;
	unsigned int plane_no_per_die;
	bool eraseSuspensionEnabled, programSuspensionEnabled;
	sim_time_type writeReasonableSuspensionTimeForRead;
	sim_time_type eraseReasonableSuspensionTimeForRead; //the time period
	sim_time_type eraseReasonableSuspensionTimeForWrite;
	flash_chip_ID_type *Round_robin_turn_of_channel; //Used for round-robin service of the chips in channels

	static TSU_Base *_my_instance;
	std::list<NVM_Transaction_Flash *> transaction_receive_slots;  //Stores the transactions that are received for sheduling
	std::list<NVM_Transaction_Flash *> transaction_dispatch_slots; //Used to submit transactions to the channel controller
	virtual bool service_read_transaction(NVM::FlashMemory::Flash_Chip *chip) = 0;
	virtual bool service_write_transaction(NVM::FlashMemory::Flash_Chip *chip) = 0;
	virtual bool service_erase_transaction(NVM::FlashMemory::Flash_Chip *chip) = 0;
	bool issue_command_to_chip(Flash_Transaction_Queue *sourceQueue1, Flash_Transaction_Queue *sourceQueue2, Transaction_Type transactionType, bool suspensionRequired);
	static void handle_transaction_serviced_signal_from_PHY(NVM_Transaction_Flash *transaction);
	static void handle_channel_idle_signal(flash_channel_ID_type);
	static void handle_chip_idle_signal(NVM::FlashMemory::Flash_Chip *chip);
	int opened_scheduling_reqs;
	void process_chip_requests(NVM::FlashMemory::Flash_Chip* chip)
	{
		if (!_my_instance->service_read_transaction(chip)) {
			if (!_my_instance->service_write_transaction(chip)) {
				_my_instance->service_erase_transaction(chip);
			}
		}
	}

private:
	bool independent_wordline_multiplane = false;
	std::unique_ptr<LEPA> lepa;
	QSTR_MED* qstr_med = nullptr;
	bool issue_independent_wordline_command(Flash_Transaction_Queue& queue, Transaction_Type type, bool suspensionRequired);

	bool transaction_is_ready(NVM_Transaction_Flash* transaction)
	{
		switch (transaction->Type)
		{
		case Transaction_Type::READ:
			return true;
		case Transaction_Type::WRITE:
			return static_cast<NVM_Transaction_Flash_WR*>(transaction)->RelatedRead == NULL;
		case Transaction_Type::ERASE:
			return static_cast<NVM_Transaction_Flash_ER*>(transaction)->Page_movement_activities.size() == 0;
		default:
			return true;
		}
	}
};
} // namespace SSD_Components

#endif //TSU_H
