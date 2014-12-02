/*
 * This file is part of the coreboot project.
 *
 * Copyright (C) 2011 The Chromium OS Authors. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 of
 * the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * This is a ramstage driver for the Intel Management Engine found in the
 * 6-series chipset.  It handles the required boot-time messages over the
 * MMIO-based Management Engine Interface to tell the ME that the BIOS is
 * finished with POST.  Additional messages are defined for debug but are
 * not used unless the console loglevel is high enough.
 */

#include <arch/acpi.h>
#include <arch/io.h>
#include <console/console.h>
#include <device/device.h>
#include <device/pci.h>
#include <device/pci_ids.h>
#include <device/pci_def.h>
#include <string.h>
#include <delay.h>
#include <elog.h>
#include <halt.h>

#include "me.h"
#include "pch.h"

#if IS_ENABLED(CONFIG_CHROMEOS)
#include <vendorcode/google/chromeos/chromeos.h>
#include <vendorcode/google/chromeos/gnvs.h>
#endif

#ifndef __SMM__
/* Path that the BIOS should take based on ME state */
static const char *me_bios_path_values[] = {
	[ME_NORMAL_BIOS_PATH]		= "Normal",
	[ME_S3WAKE_BIOS_PATH]		= "S3 Wake",
	[ME_ERROR_BIOS_PATH]		= "Error",
	[ME_RECOVERY_BIOS_PATH]		= "Recovery",
	[ME_DISABLE_BIOS_PATH]		= "Disable",
	[ME_FIRMWARE_UPDATE_BIOS_PATH]	= "Firmware Update",
};
static int intel_me_read_mbp(me_bios_payload *mbp_data, struct device *dev);
#endif

/* MMIO base address for MEI interface */
static u32 *mei_base_address;
#ifdef __SIMPLE_DEVICE__
void intel_me_mbp_clear(pci_devfn_t dev);
#else
void intel_me_mbp_clear(struct device *dev);
#endif

#if IS_ENABLED(CONFIG_DEBUG_INTEL_ME)
static void mei_dump(void *ptr, int dword, int offset, const char *type)
{
	struct mei_csr *csr;

	printk(BIOS_SPEW, "%-9s[%02x] : ", type, offset);

	switch (offset) {
	case MEI_H_CSR:
	case MEI_ME_CSR_HA:
		csr = ptr;
		if (!csr) {
			printk(BIOS_SPEW, "ERROR: 0x%08x\n", dword);
			break;
		}
		printk(BIOS_SPEW, "cbd=%u cbrp=%02u cbwp=%02u ready=%u "
		       "reset=%u ig=%u is=%u ie=%u\n", csr->buffer_depth,
		       csr->buffer_read_ptr, csr->buffer_write_ptr,
		       csr->ready, csr->reset, csr->interrupt_generate,
		       csr->interrupt_status, csr->interrupt_enable);
		break;
	case MEI_ME_CB_RW:
	case MEI_H_CB_WW:
		printk(BIOS_SPEW, "CB: 0x%08x\n", dword);
		break;
	default:
		printk(BIOS_SPEW, "0x%08x\n", offset);
		break;
	}
}
#else
# define mei_dump(ptr,dword,offset,type) do {} while (0)
#endif

/*
 * ME/MEI access helpers using memcpy to avoid aliasing.
 */

static inline void mei_read_dword_ptr(void *ptr, int offset)
{
	u32 dword = read32(mei_base_address + (offset/sizeof(u32)));
	memcpy(ptr, &dword, sizeof(dword));
	mei_dump(ptr, dword, offset, "READ");
}

static inline void mei_write_dword_ptr(void *ptr, int offset)
{
	u32 dword = 0;
	memcpy(&dword, ptr, sizeof(dword));
	write32(mei_base_address + (offset/sizeof(u32)), dword);
	mei_dump(ptr, dword, offset, "WRITE");
}

#ifdef __SIMPLE_DEVICE__
static inline void pci_read_dword_ptr(pci_devfn_t dev, void *ptr, int offset)
#else
static inline void pci_read_dword_ptr(struct device *dev, void *ptr, int offset)
#endif
{
	u32 dword = pci_read_config32(dev, offset);
	memcpy(ptr, &dword, sizeof(dword));
	mei_dump(ptr, dword, offset, "PCI READ");
}

static inline void read_host_csr(struct mei_csr *csr)
{
	mei_read_dword_ptr(csr, MEI_H_CSR);
}

static inline void write_host_csr(struct mei_csr *csr)
{
	mei_write_dword_ptr(csr, MEI_H_CSR);
}

static inline void read_me_csr(struct mei_csr *csr)
{
	mei_read_dword_ptr(csr, MEI_ME_CSR_HA);
}

static inline void write_cb(u32 dword)
{
	write32(mei_base_address + (MEI_H_CB_WW/sizeof(u32)), dword);
	mei_dump(NULL, dword, MEI_H_CB_WW, "WRITE");
}

static inline u32 read_cb(void)
{
	u32 dword = read32(mei_base_address + (MEI_ME_CB_RW/sizeof(u32)));
	mei_dump(NULL, dword, MEI_ME_CB_RW, "READ");
	return dword;
}

/* Wait for ME ready bit to be asserted */
static int mei_wait_for_me_ready(void)
{
	struct mei_csr me;
	unsigned try = ME_RETRY;

	while (try--) {
		read_me_csr(&me);
		if (me.ready)
			return 0;
		udelay(ME_DELAY);
	}

	printk(BIOS_ERR, "ME: failed to become ready\n");
	return -1;
}

static void mei_reset(void)
{
	struct mei_csr host;

	if (mei_wait_for_me_ready() < 0)
		return;

	/* Reset host and ME circular buffers for next message */
	read_host_csr(&host);
	host.reset = 1;
	host.interrupt_generate = 1;
	write_host_csr(&host);

	if (mei_wait_for_me_ready() < 0)
		return;

	/* Re-init and indicate host is ready */
	read_host_csr(&host);
	host.interrupt_generate = 1;
	host.ready = 1;
	host.reset = 0;
	write_host_csr(&host);
}

static int mei_send_packet(struct mei_header *mei, void *req_data)
{
	struct mei_csr host;
	unsigned ndata, n;
	u32 *data;

	/* Number of dwords to write */
	ndata = mei->length >> 2;

	/* Pad non-dword aligned request message length */
	if (mei->length & 3)
		ndata++;
	if (!ndata) {
		printk(BIOS_DEBUG, "ME: request has no data\n");
		return -1;
	}
	ndata++; /* Add MEI header */

	/*
	 * Make sure there is still room left in the circular buffer.
	 * Reset the buffer pointers if the requested message will not fit.
	 */
	read_host_csr(&host);
	if ((host.buffer_depth - host.buffer_write_ptr) < ndata) {
		printk(BIOS_ERR, "ME: circular buffer full, resetting...\n");
		mei_reset();
		read_host_csr(&host);
	}

	/* Ensure the requested length will fit in the circular buffer. */
	if ((host.buffer_depth - host.buffer_write_ptr) < ndata) {
		printk(BIOS_ERR, "ME: message (%u) too large for buffer (%u)\n",
		       ndata + 2, host.buffer_depth);
		return -1;
	}

	/* Write MEI header */
	mei_write_dword_ptr(mei, MEI_H_CB_WW);
	ndata--;

	/* Write message data */
	data = req_data;
	for (n = 0; n < ndata; ++n)
		write_cb(*data++);

	/* Generate interrupt to the ME */
	read_host_csr(&host);
	host.interrupt_generate = 1;
	write_host_csr(&host);

	/* Make sure ME is ready after sending request data */
	return mei_wait_for_me_ready();
}

static int mei_send_data(u8 me_address, u8 host_address,
			 void *req_data, int req_bytes)
{
	struct mei_header header = {
		.client_address = me_address,
		.host_address = host_address,
	};
	struct mei_csr host;
	int current = 0;
	u8 *req_ptr = req_data;

	while (!header.is_complete) {
		int remain = req_bytes - current;
		int buf_len;

		read_host_csr(&host);
		buf_len = host.buffer_depth - host.buffer_write_ptr;

		if (buf_len > remain) {
			/* Send all remaining data as final message */
			header.length = req_bytes - current;
			header.is_complete = 1;
		} else {
			/* Send as much data as the buffer can hold */
			header.length = buf_len;
		}

		mei_send_packet(&header, req_ptr);

		req_ptr += header.length;
		current += header.length;
	}

	return 0;
}

static int mei_send_header(u8 me_address, u8 host_address,
			   void *header, int header_len, int complete)
{
	struct mei_header mei = {
		.client_address = me_address,
		.host_address   = host_address,
		.length         = header_len,
		.is_complete    = complete,
	};
	return mei_send_packet(&mei, header);
}

static int mei_recv_msg(void *header, int header_bytes,
			void *rsp_data, int rsp_bytes)
{
	struct mei_header mei_rsp;
	struct mei_csr me, host;
	unsigned ndata, n;
	unsigned expected;
	u32 *data;

	/* Total number of dwords to read from circular buffer */
	expected = (rsp_bytes + sizeof(mei_rsp) + header_bytes) >> 2;
	if (rsp_bytes & 3)
		expected++;

	if (mei_wait_for_me_ready() < 0)
		return -1;

	/*
	 * The interrupt status bit does not appear to indicate that the
	 * message has actually been received.  Instead we wait until the
	 * expected number of dwords are present in the circular buffer.
	 */
	for (n = ME_RETRY; n; --n) {
		read_me_csr(&me);
		if ((me.buffer_write_ptr - me.buffer_read_ptr) >= expected)
			break;
		udelay(ME_DELAY);
	}
	if (!n) {
		printk(BIOS_ERR, "ME: timeout waiting for data: expected "
		       "%u, available %u\n", expected,
		       me.buffer_write_ptr - me.buffer_read_ptr);
		return -1;
	}

	/* Read and verify MEI response header from the ME */
	mei_read_dword_ptr(&mei_rsp, MEI_ME_CB_RW);
	if (!mei_rsp.is_complete) {
		printk(BIOS_ERR, "ME: response is not complete\n");
		return -1;
	}

	/* Handle non-dword responses and expect at least the header */
	ndata = mei_rsp.length >> 2;
	if (mei_rsp.length & 3)
		ndata++;
	if (ndata != (expected - 1)) {
		printk(BIOS_ERR, "ME: response is missing data %d != %d\n",
		       ndata, (expected - 1));
		return -1;
	}

	/* Read response header from the ME */
	data = header;
	for (n = 0; n < (header_bytes >> 2); ++n)
		*data++ = read_cb();
	ndata -= header_bytes >> 2;

	/* Make sure caller passed a buffer with enough space */
	if (ndata != (rsp_bytes >> 2)) {
		printk(BIOS_ERR, "ME: not enough room in response buffer: "
		       "%u != %u\n", ndata, rsp_bytes >> 2);
		return -1;
	}

	/* Read response data from the circular buffer */
	data = rsp_data;
	for (n = 0; n < ndata; ++n)
		*data++ = read_cb();

	/* Tell the ME that we have consumed the response */
	read_host_csr(&host);
	host.interrupt_status = 1;
	host.interrupt_generate = 1;
	write_host_csr(&host);

	return mei_wait_for_me_ready();
}

#if IS_ENABLED (CONFIG_DEBUG_INTEL_ME) || defined(__SMM__)
static inline int mei_sendrecv_mkhi(struct mkhi_header *mkhi,
				    void *req_data, int req_bytes,
				    void *rsp_data, int rsp_bytes)
{
	struct mkhi_header mkhi_rsp;

	/* Send header */
	if (mei_send_header(MEI_ADDRESS_MKHI, MEI_HOST_ADDRESS,
			    mkhi, sizeof(*mkhi), req_bytes ? 0 : 1) < 0)
		return -1;

	/* Send data if available */
	if (req_bytes && mei_send_data(MEI_ADDRESS_MKHI, MEI_HOST_ADDRESS,
				     req_data, req_bytes) < 0)
		return -1;

	/* Return now if no response expected */
	if (!rsp_bytes)
		return 0;

	/* Read header and data */
	if (mei_recv_msg(&mkhi_rsp, sizeof(mkhi_rsp),
			 rsp_data, rsp_bytes) < 0)
		return -1;

	if (!mkhi_rsp.is_response ||
	    mkhi->group_id != mkhi_rsp.group_id ||
	    mkhi->command != mkhi_rsp.command) {
		printk(BIOS_ERR, "ME: invalid response, group %u ?= %u,"
		       "command %u ?= %u, is_response %u\n", mkhi->group_id,
		       mkhi_rsp.group_id, mkhi->command, mkhi_rsp.command,
		       mkhi_rsp.is_response);
		return -1;
	}

	return 0;
}
#endif /* CONFIG_DEBUG_INTEL_ME || __SMM__ */

/*
 * mbp give up routine. This path is taken if hfs.mpb_rdy is 0 or the read
 * state machine on the BIOS end doesn't match the ME's state machine.
 */
#ifdef __SIMPLE_DEVICE__
static void intel_me_mbp_give_up(pci_devfn_t dev)
#else
static void intel_me_mbp_give_up(struct device *dev)
#endif
{
	struct mei_csr csr;

	pci_write_config32(dev, PCI_ME_H_GS2, PCI_ME_MBP_GIVE_UP);

	read_host_csr(&csr);
	csr.reset = 1;
	csr.interrupt_generate = 1;
	write_host_csr(&csr);
}

/*
 * mbp clear routine. This will wait for the ME to indicate that
 * the MBP has been read and cleared.
 */
#ifdef __SIMPLE_DEVICE__
void intel_me_mbp_clear(pci_devfn_t dev)
#else
void intel_me_mbp_clear(struct device *dev)
#endif
{
	int count;
	struct me_hfs2 hfs2;

	/* Wait for the mbp_cleared indicator */
	for (count = ME_RETRY; count > 0; --count) {
		pci_read_dword_ptr(dev, &hfs2, PCI_ME_HFS2);
		if (hfs2.mbp_cleared)
			break;
		udelay(ME_DELAY);
	}

	if (count == 0) {
		printk(BIOS_WARNING, "ME: Timeout waiting for mbp_cleared\n");
		intel_me_mbp_give_up(dev);
	} else {
		printk(BIOS_INFO, "ME: MBP cleared\n");
	}
}

#if (CONFIG_DEFAULT_CONSOLE_LOGLEVEL >= BIOS_DEBUG) && !defined(__SMM__)
static void me_print_fw_version(mbp_fw_version_name *vers_name)
{
	if (!vers_name) {
		printk(BIOS_ERR, "ME: mbp missing version report\n");
		return;
	}

	printk(BIOS_DEBUG, "ME: found version %d.%d.%d.%d\n",
	       vers_name->major_version, vers_name->minor_version,
	       vers_name->hotfix_version, vers_name->build_version);
}

#if IS_ENABLED (CONFIG_DEBUG_INTEL_ME)
static inline void print_cap(const char *name, int state)
{
	printk(BIOS_DEBUG, "ME Capability: %-41s : %sabled\n",
	       name, state ? " en" : "dis");
}

/* Get ME Firmware Capabilities */
static int mkhi_get_fwcaps(mbp_mefwcaps *cap)
{
	u32 rule_id = 0;
	struct me_fwcaps cap_msg;
	struct mkhi_header mkhi = {
		.group_id       = MKHI_GROUP_ID_FWCAPS,
		.command        = MKHI_FWCAPS_GET_RULE,
	};

	/* Send request and wait for response */
	if (mei_sendrecv_mkhi(&mkhi, &rule_id, sizeof(u32),
			      &cap_msg, sizeof(cap_msg)) < 0) {
		printk(BIOS_ERR, "ME: GET FWCAPS message failed\n");
		return -1;
	}
	*cap = cap_msg.caps_sku;
	return 0;
}

/* Get ME Firmware Capabilities */
static void me_print_fwcaps(mbp_mefwcaps *cap)
{
	mbp_mefwcaps local_caps;
	if (!cap) {
		cap = &local_caps;
		printk(BIOS_ERR, "ME: mbp missing fwcaps report\n");
		if (mkhi_get_fwcaps(cap))
			return;
	}

	print_cap("Full Network manageability", cap->full_net);
	print_cap("Regular Network manageability", cap->std_net);
	print_cap("Manageability", cap->manageability);
	print_cap("IntelR Anti-Theft (AT)", cap->intel_at);
	print_cap("IntelR Capability Licensing Service (CLS)", cap->intel_cls);
	print_cap("IntelR Power Sharing Technology (MPC)", cap->intel_mpc);
	print_cap("ICC Over Clocking", cap->icc_over_clocking);
	print_cap("Protected Audio Video Path (PAVP)", cap->pavp);
	print_cap("IPV6", cap->ipv6);
	print_cap("KVM Remote Control (KVM)", cap->kvm);
	print_cap("Outbreak Containment Heuristic (OCH)", cap->och);
	print_cap("Virtual LAN (VLAN)", cap->vlan);
	print_cap("TLS", cap->tls);
	print_cap("Wireless LAN (WLAN)", cap->wlan);
}
#endif /* CONFIG_DEBUG_INTEL_ME */
#endif

#if IS_ENABLED(CONFIG_CHROMEOS) && 0 /* DISABLED */
/* Tell ME to issue a global reset */
static int mkhi_global_reset(void)
{
	struct me_global_reset reset = {
		.request_origin	= GLOBAL_RESET_BIOS_POST,
		.reset_type	= CBM_RR_GLOBAL_RESET,
	};
	struct mkhi_header mkhi = {
		.group_id	= MKHI_GROUP_ID_CBM,
		.command	= MKHI_GLOBAL_RESET,
	};

	/* Send request and wait for response */
	printk(BIOS_NOTICE, "ME: %s\n", __FUNCTION__);
	if (mei_sendrecv_mkhi(&mkhi, &reset, sizeof(reset), NULL, 0) < 0) {
		/* No response means reset will happen shortly... */
		halt();
	}

	/* If the ME responded it rejected the reset request */
	printk(BIOS_ERR, "ME: Global Reset failed\n");
	return -1;
}
#endif

#ifdef __SMM__

/* Send END OF POST message to the ME */
static int mkhi_end_of_post(void)
{
	struct mkhi_header mkhi = {
		.group_id	= MKHI_GROUP_ID_GEN,
		.command	= MKHI_END_OF_POST,
	};
	u32 eop_ack;

	/* Send request and wait for response */
	printk(BIOS_NOTICE, "ME: %s\n", __FUNCTION__);
	if (mei_sendrecv_mkhi(&mkhi, NULL, 0, &eop_ack, sizeof(eop_ack)) < 0) {
		printk(BIOS_ERR, "ME: END OF POST message failed\n");
		return -1;
	}

	printk(BIOS_INFO, "ME: END OF POST message successful (%d)\n", eop_ack);
	return 0;
}

void intel_me_finalize_smm(void)
{
	struct me_hfs hfs;
	u32 reg32;

	mei_base_address = (u32 *)
		(pci_read_config32(PCH_ME_DEV, PCI_BASE_ADDRESS_0) & ~0xf);

	/* S3 path will have hidden this device already */
	if (!mei_base_address || mei_base_address == (u32 *)0xfffffff0)
		return;

#if IS_ENABLED(CONFIG_ME_MBP_CLEAR_LATE)
	/* Wait for ME MBP Cleared indicator */
	intel_me_mbp_clear(PCH_ME_DEV);
#endif

	/* Make sure ME is in a mode that expects EOP */
	reg32 = pci_read_config32(PCH_ME_DEV, PCI_ME_HFS);
	memcpy(&hfs, &reg32, sizeof(u32));

	/* Abort and leave device alone if not normal mode */
	if (hfs.fpt_bad ||
	    hfs.working_state != ME_HFS_CWS_NORMAL ||
	    hfs.operation_mode != ME_HFS_MODE_NORMAL)
		return;

	/* Try to send EOP command so ME stops accepting other commands */
	mkhi_end_of_post();

}

#else /* !__SMM__ */

static inline int mei_sendrecv_icc(struct icc_header *icc,
				   void *req_data, int req_bytes,
				   void *rsp_data, int rsp_bytes)
{
	struct icc_header icc_rsp;

	/* Send header */
	if (mei_send_header(MEI_ADDRESS_ICC, MEI_HOST_ADDRESS,
			    icc, sizeof(*icc), req_bytes ? 0 : 1) < 0)
		return -1;

	/* Send data if available */
	if (req_bytes && mei_send_data(MEI_ADDRESS_ICC, MEI_HOST_ADDRESS,
				       req_data, req_bytes) < 0)
		return -1;

	/* Read header and data, if needed */
	if (rsp_bytes && mei_recv_msg(&icc_rsp, sizeof(icc_rsp),
				      rsp_data, rsp_bytes) < 0)
		return -1;

	return 0;
}

static int me_icc_set_clock_enables(u32 mask)
{
	struct icc_clock_enables_msg clk = {
		.clock_enables	= 0, /* Turn off specified clocks */
		.clock_mask	= mask,
		.no_response	= 1, /* Do not expect response */
	};
	struct icc_header icc = {
		.api_version	= ICC_API_VERSION_LYNXPOINT,
		.icc_command	= ICC_SET_CLOCK_ENABLES,
		.length		= sizeof(clk),
	};

	/* Send request and wait for response */
	if (mei_sendrecv_icc(&icc, &clk, sizeof(clk), NULL, 0) < 0) {
		printk(BIOS_ERR, "ME: ICC SET CLOCK ENABLES message failed\n");
		return -1;
	}

	printk(BIOS_INFO, "ME: ICC SET CLOCK ENABLES 0x%08x\n", mask);
	return 0;
}

/* Determine the path that we should take based on ME status */
static me_bios_path intel_me_path(struct device *dev)
{
	me_bios_path path = ME_DISABLE_BIOS_PATH;
	struct me_hfs hfs;
	struct me_hfs2 hfs2;

	pci_read_dword_ptr(dev, &hfs, PCI_ME_HFS);
	pci_read_dword_ptr(dev, &hfs2, PCI_ME_HFS2);

	/* Check and dump status */
	intel_me_status(&hfs, &hfs2);

	/* Check Current Working State */
	switch (hfs.working_state) {
	case ME_HFS_CWS_NORMAL:
		path = ME_NORMAL_BIOS_PATH;
		break;
	case ME_HFS_CWS_REC:
		path = ME_RECOVERY_BIOS_PATH;
		break;
	default:
		path = ME_DISABLE_BIOS_PATH;
		break;
	}

	/* Check Current Operation Mode */
	switch (hfs.operation_mode) {
	case ME_HFS_MODE_NORMAL:
		break;
	case ME_HFS_MODE_DEBUG:
	case ME_HFS_MODE_DIS:
	case ME_HFS_MODE_OVER_JMPR:
	case ME_HFS_MODE_OVER_MEI:
	default:
		path = ME_DISABLE_BIOS_PATH;
		break;
	}

	/* Check for any error code and valid firmware and MBP */
	if (hfs.error_code || hfs.fpt_bad)
		path = ME_ERROR_BIOS_PATH;

	/* Check if the MBP is ready */
	if (!hfs2.mbp_rdy) {
		printk(BIOS_CRIT, "%s: mbp is not ready!\n",
		       __FUNCTION__);
		path = ME_ERROR_BIOS_PATH;
	}

#if IS_ENABLED(CONFIG_ELOG)
	if (path != ME_NORMAL_BIOS_PATH) {
		struct elog_event_data_me_extended data = {
			.current_working_state = hfs.working_state,
			.operation_state       = hfs.operation_state,
			.operation_mode        = hfs.operation_mode,
			.error_code            = hfs.error_code,
			.progress_code         = hfs2.progress_code,
			.current_pmevent       = hfs2.current_pmevent,
			.current_state         = hfs2.current_state,
		};
		elog_add_event_byte(ELOG_TYPE_MANAGEMENT_ENGINE, path);
		elog_add_event_raw(ELOG_TYPE_MANAGEMENT_ENGINE_EXT,
				   &data, sizeof(data));
	}
#endif

	return path;
}

/* Prepare ME for MEI messages */
static int intel_mei_setup(struct device *dev)
{
	struct resource *res;
	struct mei_csr host;
	u32 reg32;

	/* Find the MMIO base for the ME interface */
	res = find_resource(dev, PCI_BASE_ADDRESS_0);
	if (!res || res->base == 0 || res->size == 0) {
		printk(BIOS_DEBUG, "ME: MEI resource not present!\n");
		return -1;
	}
	mei_base_address = (u32 *)(uintptr_t)res->base;

	/* Ensure Memory and Bus Master bits are set */
	reg32 = pci_read_config32(dev, PCI_COMMAND);
	reg32 |= PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY;
	pci_write_config32(dev, PCI_COMMAND, reg32);

	/* Clean up status for next message */
	read_host_csr(&host);
	host.interrupt_generate = 1;
	host.ready = 1;
	host.reset = 0;
	write_host_csr(&host);

	return 0;
}

/* Read the Extend register hash of ME firmware */
static int intel_me_extend_valid(struct device *dev)
{
	struct me_heres status;
	u32 extend[8] = {0};
	int i, count = 0;

	pci_read_dword_ptr(dev, &status, PCI_ME_HERES);
	if (!status.extend_feature_present) {
		printk(BIOS_ERR, "ME: Extend Feature not present\n");
		return -1;
	}

	if (!status.extend_reg_valid) {
		printk(BIOS_ERR, "ME: Extend Register not valid\n");
		return -1;
	}

	switch (status.extend_reg_algorithm) {
	case PCI_ME_EXT_SHA1:
		count = 5;
		printk(BIOS_DEBUG, "ME: Extend SHA-1: ");
		break;
	case PCI_ME_EXT_SHA256:
		count = 8;
		printk(BIOS_DEBUG, "ME: Extend SHA-256: ");
		break;
	default:
		printk(BIOS_ERR, "ME: Extend Algorithm %d unknown\n",
		       status.extend_reg_algorithm);
		return -1;
	}

	for (i = 0; i < count; ++i) {
		extend[i] = pci_read_config32(dev, PCI_ME_HER(i));
		printk(BIOS_DEBUG, "%08x", extend[i]);
	}
	printk(BIOS_DEBUG, "\n");

#if IS_ENABLED(CONFIG_CHROMEOS)
	/* Save hash in NVS for the OS to verify */
	chromeos_set_me_hash(extend, count);
#endif

	return 0;
}

/* Check whether ME is present and do basic init */
static void intel_me_init(struct device *dev)
{
	struct southbridge_intel_lynxpoint_config *config = dev->chip_info;
	me_bios_path path = intel_me_path(dev);
	me_bios_payload mbp_data;

	/* Do initial setup and determine the BIOS path */
	printk(BIOS_NOTICE, "ME: BIOS path: %s\n", me_bios_path_values[path]);

	if (path == ME_NORMAL_BIOS_PATH) {
		/* Validate the extend register */
		intel_me_extend_valid(dev);
	}

	memset(&mbp_data, 0, sizeof(mbp_data));

	/*
	 * According to the ME9 BWG, BIOS is required to fetch MBP data in
	 * all boot flows except S3 Resume.
	 */

	/* Prepare MEI MMIO interface */
	if (intel_mei_setup(dev) < 0)
		return;

	if (intel_me_read_mbp(&mbp_data, dev))
		return;

#if (CONFIG_DEFAULT_CONSOLE_LOGLEVEL >= BIOS_DEBUG)
	me_print_fw_version(mbp_data.fw_version_name);
#if IS_ENABLED(CONFIG_DEBUG_INTEL_ME)
	me_print_fwcaps(mbp_data.fw_capabilities);
#endif

	if (mbp_data.plat_time) {
		printk(BIOS_DEBUG, "ME: Wake Event to ME Reset:      %u ms\n",
		       mbp_data.plat_time->wake_event_mrst_time_ms);
		printk(BIOS_DEBUG, "ME: ME Reset to Platform Reset:  %u ms\n",
		       mbp_data.plat_time->mrst_pltrst_time_ms);
		printk(BIOS_DEBUG, "ME: Platform Reset to CPU Reset: %u ms\n",
		       mbp_data.plat_time->pltrst_cpurst_time_ms);
	}
#endif

	/* Set clock enables according to devicetree */
	if (config && config->icc_clock_disable)
		me_icc_set_clock_enables(config->icc_clock_disable);

	/*
	 * Leave the ME unlocked. It will be locked via SMI command later.
	 */
}

static void set_subsystem(struct device *dev, unsigned int vendor,
			  unsigned int device)
{
	if (!vendor || !device) {
		pci_write_config32(dev, PCI_SUBSYSTEM_VENDOR_ID,
			   pci_read_config32(dev, PCI_VENDOR_ID));
	} else {
		pci_write_config32(dev, PCI_SUBSYSTEM_VENDOR_ID,
			   ((device & 0xffff) << 16) | (vendor & 0xffff));
	}
}

static struct pci_operations pci_ops = {
	.set_subsystem = set_subsystem,
};

static struct device_operations device_ops = {
	.read_resources		= pci_dev_read_resources,
	.set_resources		= pci_dev_set_resources,
	.enable_resources	= pci_dev_enable_resources,
	.init			= intel_me_init,
	.ops_pci		= &pci_ops,
};

static const unsigned short pci_device_ids[] = {
	0x8c3a, /* Mobile */
	0x9c3a, /* Low Power */
	0
};

static const struct pci_driver intel_me __pci_driver = {
	.ops	= &device_ops,
	.vendor	= PCI_VENDOR_ID_INTEL,
	.devices= pci_device_ids,
};

/******************************************************************************
 *									     */
static u32 me_to_host_words_pending(void)
{
	struct mei_csr me;
	read_me_csr(&me);
	if (!me.ready)
		return 0;
	return (me.buffer_write_ptr - me.buffer_read_ptr) &
		(me.buffer_depth - 1);
}

#if 0
/* This function is not yet being used, keep it in for the future. */
static u32 host_to_me_words_room(void)
{
	struct mei_csr csr;

	read_me_csr(&csr);
	if (!csr.ready)
		return 0;

	read_host_csr(&csr);
	return (csr.buffer_read_ptr - csr.buffer_write_ptr - 1) &
		(csr.buffer_depth - 1);
}
#endif

struct mbp_payload {
	mbp_header header;
	u32 data[0];
};

/*
 * mbp seems to be following its own flow, let's retrieve it in a dedicated
 * function.
 */
static int intel_me_read_mbp(me_bios_payload *mbp_data, struct device *dev)
{
	mbp_header mbp_hdr;
	u32 me2host_pending;
	struct mei_csr host;
	struct me_hfs2 hfs2;
	struct mbp_payload *mbp;
	int i;

	pci_read_dword_ptr(dev, &hfs2, PCI_ME_HFS2);

	if (!hfs2.mbp_rdy) {
		printk(BIOS_ERR, "ME: MBP not ready\n");
		goto mbp_failure;
	}

	me2host_pending = me_to_host_words_pending();
	if (!me2host_pending) {
		printk(BIOS_ERR, "ME: no mbp data!\n");
		goto mbp_failure;
	}

	/* we know for sure that at least the header is there */
	mei_read_dword_ptr(&mbp_hdr, MEI_ME_CB_RW);

	if ((mbp_hdr.num_entries > (mbp_hdr.mbp_size / 2)) ||
	    (me2host_pending < mbp_hdr.mbp_size)) {
		printk(BIOS_ERR, "ME: mbp of %d entries, total size %d words"
		       " buffer contains %d words\n",
		       mbp_hdr.num_entries, mbp_hdr.mbp_size,
		       me2host_pending);
		goto mbp_failure;
	}
	mbp = malloc(mbp_hdr.mbp_size * sizeof(u32));
	if (!mbp)
		goto mbp_failure;

	mbp->header = mbp_hdr;
	me2host_pending--;

	i = 0;
	while (i != me2host_pending) {
		mei_read_dword_ptr(&mbp->data[i], MEI_ME_CB_RW);
		i++;
	}

	/* Signal to the ME that the host has finished reading the MBP. */
	read_host_csr(&host);
	host.interrupt_generate = 1;
	write_host_csr(&host);

#if !IS_ENABLED(CONFIG_ME_MBP_CLEAR_LATE)
	/* Wait for the mbp_cleared indicator. */
	intel_me_mbp_clear(dev);
#endif

	/* Dump out the MBP contents. */
#if (CONFIG_DEFAULT_CONSOLE_LOGLEVEL >= BIOS_DEBUG)
	printk(BIOS_INFO, "ME MBP: Header: items: %d, size dw: %d\n",
	       mbp->header.num_entries, mbp->header.mbp_size);
#if IS_ENABLED(CONFIG_DEBUG_INTEL_ME)
	for (i = 0; i < mbp->header.mbp_size - 1; i++) {
		printk(BIOS_INFO, "ME MBP: %04x: 0x%08x\n", i, mbp->data[i]);
	}
#endif
#endif

	#define ASSIGN_FIELD_PTR(field_,val_) \
		{ \
		mbp_data->field_ = (typeof(mbp_data->field_))(void *)val_; \
		break; \
		}
	/* Setup the pointers in the me_bios_payload structure. */
	for (i = 0; i < mbp->header.mbp_size - 1;) {
		mbp_item_header *item = (void *)&mbp->data[i];

		switch (MBP_MAKE_IDENT(item->app_id, item->item_id)) {
		case MBP_IDENT(KERNEL, FW_VER):
			ASSIGN_FIELD_PTR(fw_version_name, &mbp->data[i+1]);

		case MBP_IDENT(ICC, PROFILE):
			ASSIGN_FIELD_PTR(icc_profile, &mbp->data[i+1]);

		case MBP_IDENT(INTEL_AT, STATE):
			ASSIGN_FIELD_PTR(at_state, &mbp->data[i+1]);

		case MBP_IDENT(KERNEL, FW_CAP):
			ASSIGN_FIELD_PTR(fw_capabilities, &mbp->data[i+1]);

		case MBP_IDENT(KERNEL, ROM_BIST):
			ASSIGN_FIELD_PTR(rom_bist_data, &mbp->data[i+1]);

		case MBP_IDENT(KERNEL, PLAT_KEY):
			ASSIGN_FIELD_PTR(platform_key, &mbp->data[i+1]);

		case MBP_IDENT(KERNEL, FW_TYPE):
			ASSIGN_FIELD_PTR(fw_plat_type, &mbp->data[i+1]);

		case MBP_IDENT(KERNEL, MFS_FAILURE):
			ASSIGN_FIELD_PTR(mfsintegrity, &mbp->data[i+1]);

		case MBP_IDENT(KERNEL, PLAT_TIME):
			ASSIGN_FIELD_PTR(plat_time, &mbp->data[i+1]);

		case MBP_IDENT(NFC, SUPPORT_DATA):
			ASSIGN_FIELD_PTR(nfc_data, &mbp->data[i+1]);

		default:
			printk(BIOS_ERR, "ME MBP: unknown item 0x%x @ "
			       "dw offset 0x%x\n", mbp->data[i], i);
			break;
		}
		i += item->length;
	}
	#undef ASSIGN_FIELD_PTR

	return 0;

mbp_failure:
	intel_me_mbp_give_up(dev);
	return -1;
}

#endif /* !__SMM__ */
