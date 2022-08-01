#include "definitions.h"
#include "hw.h"

void CCsAudioCatptSSTHW::ipc_init() {
	this->ipc_ready = false;
	this->ipc_done = false;
	this->ipc_busy = false;
}

NTSTATUS CCsAudioCatptSSTHW::ipc_arm(struct catpt_fw_ready* config)
{
	/*
	 * Both tx and rx are put into and received from outbox. Inbox is
	 * only used for notifications where payload size is known upfront,
	 * thus no separate buffer is allocated for it.
	 */
	this->ipc_rx.data = ExAllocatePool2(POOL_FLAG_NON_PAGED, config->outbox_size, CSAUDIOCATPTSST_POOLTAG);
	if (this->ipc_rx.data)
		return STATUS_NO_MEMORY;

	memcpy(&ipc_config, config, sizeof(*config));
	this->ipc_ready = true;

	return STATUS_SUCCESS;
}

void CCsAudioCatptSSTHW::dsp_copy_rx(UINT32 header)
{
	this->ipc_rx.header = header;
	if (this->ipc_rx.rsp.status != CATPT_REPLY_SUCCESS)
		return;

	memcpy(this->ipc_rx.data, catpt_outbox_addr(this), this->ipc_rx.size);
}

void CCsAudioCatptSSTHW::dsp_process_response(UINT32 header)
{
	union catpt_notify_msg msg = CATPT_MSG(header);

	if (msg.fw_ready) {
		struct catpt_fw_ready config;
		/* to fit 32b header original address is shifted right by 3 */
		UINT32 off = msg.mailbox_address << 3;

		memcpy(&config, this->lpe_ba + off, sizeof(config));

		ipc_arm(&config);
		this->fw_ready = true;
		return;
	}

	switch (msg.global_msg_type) {
	case CATPT_GLB_REQUEST_CORE_DUMP:
		DbgPrint("ADSP device coredump received\n");
		this->ipc_ready = false;
		//catpt_coredump();
		/* TODO: attempt recovery */
		break;

	case CATPT_GLB_STREAM_MESSAGE:
		switch (msg.stream_msg_type) {
		case CATPT_STRM_NOTIFICATION:
			DbgPrint("DSP notify stream\n");
			//TODO: dsp_notify_stream
			//dsp_notify_stream(msg);
			break;
		default:
			//TODO: dsp_copy_rx
			DbgPrint("DSP copy RX\n");
			dsp_copy_rx(header);
			/* signal completion of delayed reply */
			this->ipc_busy = FALSE;
			break;
		}
		break;

	default:
		DbgPrint("unknown response: %d received\n",
			msg.global_msg_type);
		break;
	}
}

void CCsAudioCatptSSTHW::dsp_irq_thread() {
	UINT32 ipcd;

	ipcd = catpt_readl_shim(this, IPCD);

	/* ensure there is delayed reply or notification to process */
	if (!(ipcd & CATPT_IPCD_BUSY))
		return;

	dsp_process_response(ipcd);


	/* tell DSP processing is completed */
	catpt_updatel_shim(this, IPCD, CATPT_IPCD_BUSY | CATPT_IPCD_DONE,
		CATPT_IPCD_DONE);
	/* unmask dsp BUSY interrupt */
	catpt_updatel_shim(this, IMC, CATPT_IMC_IPCDB, 0);
}

NTSTATUS CCsAudioCatptSSTHW::dsp_irq_handler() {
	NTSTATUS status;
	UINT32 isc, ipcc;
	isc = catpt_readl_shim(this, ISC);

	/* immediate reply */
	if (isc & CATPT_ISC_IPCCD) {
		/* mask host DONE interrupt */
		catpt_updatel_shim(this, IMC, CATPT_IMC_IPCCD, CATPT_IMC_IPCCD);

		ipcc = catpt_readl_shim(this, IPCC);
		dsp_copy_rx(ipcc);

		this->ipc_done = true;

		/* tell DSP processing is completed */
		catpt_updatel_shim(this, IPCC, CATPT_IPCC_DONE, 0);
		/* unmask host DONE interrupt */
		catpt_updatel_shim(this, IMC, CATPT_IMC_IPCCD, 0);
		status = STATUS_SUCCESS;
	}

	/* delayed reply or notification */
	if (isc & CATPT_ISC_IPCDB) {
		/* mask dsp BUSY interrupt */
		catpt_updatel_shim(this, IMC, CATPT_IMC_IPCDB, CATPT_IMC_IPCDB);
		ExQueueWorkItem(this->m_WorkQueueItem, DelayedWorkQueue);
		status = STATUS_SUCCESS;
	}

	return STATUS_INVALID_PARAMETER;
}