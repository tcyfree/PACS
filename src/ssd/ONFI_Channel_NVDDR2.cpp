#include "ONFI_Channel_NVDDR2.h"

namespace SSD_Components
{
	ONFI_Channel_NVDDR2::ONFI_Channel_NVDDR2(flash_channel_ID_type channelID, unsigned int chipCount, NVM::FlashMemory::Flash_Chip** flashChips, unsigned int ChannelWidth, unsigned int plane_count,
		sim_time_type t_RC, sim_time_type t_DSC,
		sim_time_type t_DBSY, sim_time_type t_CS, sim_time_type t_RR,
		sim_time_type t_WB, sim_time_type t_WC, sim_time_type t_ADL, sim_time_type t_CALS,
		sim_time_type t_DQSRE, sim_time_type t_RPRE, sim_time_type t_RHW, sim_time_type t_CCS,
		sim_time_type t_WPST, sim_time_type t_WPSTH)
		: ONFI_Channel_Base(channelID, chipCount, flashChips, ONFI_Protocol::NVDDR2),
		ReadCommandTime(plane_count + 1), ProgramCommandTime(plane_count + 1), EraseCommandTime(plane_count + 1),
		ChannelWidth(ChannelWidth),
		t_RC(t_RC), t_DSC(t_DSC), t_DBSY(t_DBSY), t_CS(t_CS), t_RR(t_RR), t_WB(t_WB), t_WC(t_WC), t_ADL(t_ADL),
		t_CALS(t_CALS), t_DQSRE(t_DQSRE), t_RPRE(t_RPRE), t_RHW(t_RHW), t_CCS(t_CCS), t_WPST(t_WPST), t_WPSTH(t_WPSTH)
	{
		for (unsigned int planes = 1; planes <= plane_count; ++planes) {
			const auto intermediate_planes = planes - 1;
			ReadCommandTime[planes] = t_CS + planes * 6 * t_WC + intermediate_planes * t_DBSY + t_WB + t_RR;
			ProgramCommandTime[planes] = t_CS + intermediate_planes * (t_WC * 5 + t_ADL + t_WPST + t_CALS + t_WB + t_DBSY)
				+ t_WC * 6 + t_ADL + t_WPST + t_WPSTH + t_WB;
			EraseCommandTime[planes] = t_CS + t_WC * 4 + t_WB + intermediate_planes * (t_DBSY + t_WC * 4 + t_WB);
		}
		ReadDataOutSetupTime = t_RPRE + t_DQSRE;
		ReadDataOutSetupTime_TwoPlane = t_RPRE + t_DQSRE + t_RHW + 6 * t_WC + t_CCS + t_RPRE + t_DQSRE;
		ReadDataOutSetupTime_ThreePlane = t_RPRE + t_DQSRE + t_RHW + 6 * t_WC + t_CCS + t_RPRE + t_DQSRE + t_RHW + 6 * t_WC + t_CCS + t_RPRE + t_DQSRE;
		ReadDataOutSetupTime_FourPlane = t_RPRE + t_DQSRE + t_RHW + 6 * t_WC + t_CCS + t_RPRE + t_DQSRE + t_RHW + 6 * t_WC + t_CCS + t_RPRE + t_DQSRE + t_RHW + 6 * t_WC + t_CCS + t_RPRE + t_DQSRE;
		TwoUnitDataInTime = t_RC;

		TwoUnitDataOutTime = t_DSC;
		ProgramSuspendCommandTime = t_CS + t_WC * 3;

		EraseSuspendCommandTime = t_CS + t_WC * 3;
	}
}
