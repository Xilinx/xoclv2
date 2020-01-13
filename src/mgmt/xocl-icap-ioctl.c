// SPDX-License-Identifier: GPL-2.0
/*
 * Platform driver for Xilinx acclerator FPGA image download feature.
 * Bulk of the code borrowed from XRT driver file icap.c
 *
 * Copyright (C) 2016-2019 Xilinx, Inc.
 *
 * Authors: sonal.santan@xilinx.com
 *          chien-wei.lan@xilinx.com
 */

#include <linux/delay.h>
#include <linux/slab.h>

#include "xocl-icap.h"
#include "xocl-lib.h"
#include "xocl-features.h"
#include "xocl-mailbox-proto.h"

static const u32 gate_free_user[] = {0xe, 0xc, 0xe, 0xf};

static unsigned find_matching_freq_config(unsigned freq)
{
	unsigned start = 0;
	unsigned end = ARRAY_SIZE(frequency_table) - 1;
	unsigned idx = ARRAY_SIZE(frequency_table) - 1;

	if (freq < frequency_table[0].ocl)
		return 0;

	if (freq > frequency_table[ARRAY_SIZE(frequency_table) - 1].ocl)
		return ARRAY_SIZE(frequency_table) - 1;

	while (start < end) {
		if (freq == frequency_table[idx].ocl)
			break;
		if (freq < frequency_table[idx].ocl)
			end = idx;
		else
			start = idx + 1;
		idx = start + (end - start) / 2;
	}
	if (freq < frequency_table[idx].ocl)
		idx--;

	return idx;
}

static unsigned find_matching_freq(unsigned freq)
{
	int idx = find_matching_freq_config(freq);

	return frequency_table[idx].ocl;
}

/*
 * Based on Clocking Wizard v5.1, section Dynamic Reconfiguration
 * through AXI4-Lite
 * Note: this is being protected by write_lock which is atomic context,
 *       we should only use n[m]delay instead of n[m]sleep.
 *       based on Linux doc of timers, mdelay may not be exactly accurate
 *       on non-PC devices.
 */
static int icap_ocl_freqscaling(struct xocl_icap *icap, bool force)
{
	unsigned curr_freq;
	u32 config;
	int i;
	int j = 0;
	u32 val = 0;
	unsigned idx = 0;
	long err = 0;

	BUG_ON(!mutex_is_locked(&icap->icap_lock));

	for (i = 0; i < ICAP_MAX_NUM_CLOCKS; ++i) {
		/* A value of zero means skip scaling for this clock index */
		if (!icap->icap_ocl_frequency[i])
			continue;
		/* skip if the io does not exist */
		if (!icap->icap_clock_bases[i])
			continue;

		idx = find_matching_freq_config(icap->icap_ocl_frequency[i]);
		curr_freq = icap_get_ocl_frequency(icap, i);
		ICAP_INFO(icap, "Clock %d, Current %d Mhz, New %d Mhz ",
				i, curr_freq, icap->icap_ocl_frequency[i]);

		/*
		 * If current frequency is in the same step as the
		 * requested frequency then nothing to do.
		 */
		if (!force && (find_matching_freq_config(curr_freq) == idx))
			continue;

		val = reg_rd(icap->icap_clock_bases[i] +
			OCL_CLKWIZ_STATUS_OFFSET);
		if (val != 1) {
			ICAP_ERR(icap, "clockwiz %d is busy", i);
			err = -EBUSY;
			break;
		}

		config = frequency_table[idx].config0;
		reg_wr(icap->icap_clock_bases[i] + OCL_CLKWIZ_CONFIG_OFFSET(0),
			config);
		config = frequency_table[idx].config2;
		reg_wr(icap->icap_clock_bases[i] + OCL_CLKWIZ_CONFIG_OFFSET(2),
			config);
		mdelay(10);
		reg_wr(icap->icap_clock_bases[i] + OCL_CLKWIZ_CONFIG_OFFSET(23),
			0x00000007);
		mdelay(1);
		reg_wr(icap->icap_clock_bases[i] + OCL_CLKWIZ_CONFIG_OFFSET(23),
			0x00000002);

		ICAP_INFO(icap, "clockwiz waiting for locked signal");
		mdelay(100);
		for (j = 0; j < 100; j++) {
			val = reg_rd(icap->icap_clock_bases[i] +
				OCL_CLKWIZ_STATUS_OFFSET);
			if (val != 1) {
				mdelay(100);
				continue;
			}
		}
		if (val != 1) {
			ICAP_ERR(icap, "clockwiz MMCM/PLL did not lock after %d"
				"ms, restoring the original configuration",
				100 * 100);
			/* restore the original clock configuration */
			reg_wr(icap->icap_clock_bases[i] +
				OCL_CLKWIZ_CONFIG_OFFSET(23), 0x00000004);
			mdelay(10);
			reg_wr(icap->icap_clock_bases[i] +
				OCL_CLKWIZ_CONFIG_OFFSET(23), 0x00000000);
			err = -ETIMEDOUT;
			break;
		}
		val = reg_rd(icap->icap_clock_bases[i] +
			OCL_CLKWIZ_CONFIG_OFFSET(0));
		ICAP_INFO(icap, "clockwiz CONFIG(0) 0x%x", val);
		val = reg_rd(icap->icap_clock_bases[i] +
			OCL_CLKWIZ_CONFIG_OFFSET(2));
		ICAP_INFO(icap, "clockwiz CONFIG(2) 0x%x", val);
	}

	return err;
}

static int icap_freeze_axi_gate(struct xocl_icap *icap)
{
	ICAP_INFO(icap, "freezing CL AXI gate");
	BUG_ON(icap->icap_axi_gate_frozen);
	BUG_ON(!mutex_is_locked(&icap->icap_lock));

	(void) reg_rd(&icap->icap_axi_gate->iag_rd);
	reg_wr(&icap->icap_axi_gate->iag_wr, GATE_FREEZE_USER);
	(void) reg_rd(&icap->icap_axi_gate->iag_rd);

	/* New ICAP reset sequence applicable only to unified dsa. */
	reg_wr(&icap->icap_regs->ir_cr, 0x8);
	ndelay(2000);
	reg_wr(&icap->icap_regs->ir_cr, 0x0);
	ndelay(2000);
	reg_wr(&icap->icap_regs->ir_cr, 0x4);
	ndelay(2000);
	reg_wr(&icap->icap_regs->ir_cr, 0x0);
	ndelay(2000);

	icap->icap_axi_gate_frozen = true;
	return 0;
}

static int icap_free_axi_gate(struct xocl_icap *icap)
{
	int i;

	BUG_ON(!mutex_is_locked(&icap->icap_lock));
	ICAP_INFO(icap, "freeing CL AXI gate");
	/*
	 * First pulse the OCL RESET. This is important for PR with multiple
	 * clocks as it resets the edge triggered clock converter FIFO
	 */

	if (!icap->icap_axi_gate_frozen)
		return 0;

	for (i = 0; i < ARRAY_SIZE(gate_free_user); i++) {
		(void) reg_rd(&icap->icap_axi_gate->iag_rd);
		reg_wr(&icap->icap_axi_gate->iag_wr, gate_free_user[i]);
		ndelay(500);
	}

	(void) reg_rd(&icap->icap_axi_gate->iag_rd);

	icap->icap_axi_gate_frozen = false;
	return 0;
}

static const struct axlf_section_header *get_axlf_section_hdr(
	struct xocl_icap *icap, const struct axlf *top, enum axlf_section_kind kind)
{
	int i;
	const struct axlf_section_header *hdr = NULL;

	for (i = 0; i < top->m_header.m_numSections; i++) {
		if (top->m_sections[i].m_sectionKind == kind) {
			hdr = &top->m_sections[i];
			break;
		}
	}

	if (hdr) {
		if ((hdr->m_sectionOffset + hdr->m_sectionSize) >
			top->m_header.m_length) {
			ICAP_ERR(icap, "found section %d is invalid", kind);
			hdr = NULL;
		} else {
			ICAP_INFO(icap, "section %d offset: %llu, size: %llu",
				kind, hdr->m_sectionOffset, hdr->m_sectionSize);
		}
	} else {
		ICAP_WARN(icap, "could not find section header %d", kind);
	}

	return hdr;
}

static int bitstream_parse_header(struct xocl_icap *icap, const unsigned char *data,
	unsigned int size, XHwIcap_Bit_Header *header)
{
	unsigned int i;
	unsigned int len;
	unsigned int tmp;
	unsigned int index;

	/* Start Index at start of bitstream */
	index = 0;

	/* Initialize HeaderLength.  If header returned early inidicates
	 * failure.
	 */
	header->HeaderLength = XHI_BIT_HEADER_FAILURE;

	/* Get "Magic" length */
	header->MagicLength = data[index++];
	header->MagicLength = (header->MagicLength << 8) | data[index++];

	/* Read in "magic" */
	for (i = 0; i < header->MagicLength - 1; i++) {
		tmp = data[index++];
		if (i%2 == 0 && tmp != XHI_EVEN_MAGIC_BYTE)
			return -1;   /* INVALID_FILE_HEADER_ERROR */

		if (i%2 == 1 && tmp != XHI_ODD_MAGIC_BYTE)
			return -1;   /* INVALID_FILE_HEADER_ERROR */

	}

	/* Read null end of magic data. */
	tmp = data[index++];

	/* Read 0x01 (short) */
	tmp = data[index++];
	tmp = (tmp << 8) | data[index++];

	/* Check the "0x01" half word */
	if (tmp != 0x01)
		return -1;	 /* INVALID_FILE_HEADER_ERROR */

	/* Read 'a' */
	tmp = data[index++];
	if (tmp != 'a')
		return -1;	  /* INVALID_FILE_HEADER_ERROR	*/

	/* Get Design Name length */
	len = data[index++];
	len = (len << 8) | data[index++];

	/* allocate space for design name and final null character. */
	header->DesignName = kmalloc(len, GFP_KERNEL);

	/* Read in Design Name */
	for (i = 0; i < len; i++)
		header->DesignName[i] = data[index++];


	if (header->DesignName[len-1] != '\0')
		return -1;

	/* Read 'b' */
	tmp = data[index++];
	if (tmp != 'b')
		return -1;	/* INVALID_FILE_HEADER_ERROR */

	/* Get Part Name length */
	len = data[index++];
	len = (len << 8) | data[index++];

	/* allocate space for part name and final null character. */
	header->PartName = kmalloc(len, GFP_KERNEL);

	/* Read in part name */
	for (i = 0; i < len; i++)
		header->PartName[i] = data[index++];

	if (header->PartName[len-1] != '\0')
		return -1;

	/* Read 'c' */
	tmp = data[index++];
	if (tmp != 'c')
		return -1;	/* INVALID_FILE_HEADER_ERROR */

	/* Get date length */
	len = data[index++];
	len = (len << 8) | data[index++];

	/* allocate space for date and final null character. */
	header->Date = kmalloc(len, GFP_KERNEL);

	/* Read in date name */
	for (i = 0; i < len; i++)
		header->Date[i] = data[index++];

	if (header->Date[len - 1] != '\0')
		return -1;

	/* Read 'd' */
	tmp = data[index++];
	if (tmp != 'd')
		return -1;	/* INVALID_FILE_HEADER_ERROR  */

	/* Get time length */
	len = data[index++];
	len = (len << 8) | data[index++];

	/* allocate space for time and final null character. */
	header->Time = kmalloc(len, GFP_KERNEL);

	/* Read in time name */
	for (i = 0; i < len; i++)
		header->Time[i] = data[index++];

	if (header->Time[len - 1] != '\0')
		return -1;

	/* Read 'e' */
	tmp = data[index++];
	if (tmp != 'e')
		return -1;	/* INVALID_FILE_HEADER_ERROR */

	/* Get byte length of bitstream */
	header->BitstreamLength = data[index++];
	header->BitstreamLength = (header->BitstreamLength << 8) | data[index++];
	header->BitstreamLength = (header->BitstreamLength << 8) | data[index++];
	header->BitstreamLength = (header->BitstreamLength << 8) | data[index++];
	header->HeaderLength = index;

	ICAP_INFO(icap, "Design \"%s\"", header->DesignName);
	ICAP_INFO(icap, "Part \"%s\"", header->PartName);
	ICAP_INFO(icap, "Timestamp \"%s %s\"", header->Time, header->Date);
	ICAP_INFO(icap, "Raw data size 0x%x", header->BitstreamLength);
	return 0;
}

static int wait_for_done(struct xocl_icap *icap)
{
	u32 w;
	int i = 0;

	BUG_ON(!mutex_is_locked(&icap->icap_lock));
	for (i = 0; i < 10; i++) {
		udelay(5);
		w = reg_rd(&icap->icap_regs->ir_sr);
		ICAP_INFO(icap, "XHWICAP_SR: %x", w);
		if (w & 0x5)
			return 0;
	}

	ICAP_ERR(icap, "bitstream download timeout");
	return -ETIMEDOUT;
}

static int icap_write(struct xocl_icap *icap, const u32 *word_buf, int size)
{
	int i;
	u32 value = 0;

	for (i = 0; i < size; i++) {
		value = be32_to_cpu(word_buf[i]);
		reg_wr(&icap->icap_regs->ir_wf, value);
	}

	reg_wr(&icap->icap_regs->ir_cr, 0x1);

	for (i = 0; i < 20; i++) {
		value = reg_rd(&icap->icap_regs->ir_cr);
		if ((value & 0x1) == 0)
			return 0;
		ndelay(50);
	}

	ICAP_ERR(icap, "writing %d dwords timeout", size);
	return -EIO;
}

static int bitstream_helper(struct xocl_icap *icap, const u32 *word_buffer,
	unsigned word_count)
{
	unsigned remain_word;
	unsigned word_written = 0;
	int wr_fifo_vacancy = 0;
	int err = 0;

	BUG_ON(!mutex_is_locked(&icap->icap_lock));
	for (remain_word = word_count; remain_word > 0;
		remain_word -= word_written, word_buffer += word_written) {
		wr_fifo_vacancy = reg_rd(&icap->icap_regs->ir_wfv);
		if (wr_fifo_vacancy <= 0) {
			ICAP_ERR(icap, "no vacancy: %d", wr_fifo_vacancy);
			err = -EIO;
			break;
		}
		word_written = (wr_fifo_vacancy < remain_word) ?
			wr_fifo_vacancy : remain_word;
		if (icap_write(icap, word_buffer, word_written) != 0) {
			ICAP_ERR(icap, "write failed remain %d, written %d",
					remain_word, word_written);
			err = -EIO;
			break;
		}
	}

	return err;
}

static long icap_download(struct xocl_icap *icap, const char *buffer,
	unsigned long length)
{
	long err = 0;
	XHwIcap_Bit_Header bit_header = { 0 };
	unsigned numCharsRead = DMA_HWICAP_BITFILE_BUFFER_SIZE;
	unsigned byte_read;

	BUG_ON(!buffer);
	BUG_ON(!length);

	if (bitstream_parse_header(icap, buffer,
		DMA_HWICAP_BITFILE_BUFFER_SIZE, &bit_header)) {
		err = -EINVAL;
		goto free_buffers;
	}

	if ((bit_header.HeaderLength + bit_header.BitstreamLength) > length) {
		err = -EINVAL;
		goto free_buffers;
	}

	buffer += bit_header.HeaderLength;

	for (byte_read = 0; byte_read < bit_header.BitstreamLength;
		byte_read += numCharsRead) {
		numCharsRead = bit_header.BitstreamLength - byte_read;
		if (numCharsRead > DMA_HWICAP_BITFILE_BUFFER_SIZE)
			numCharsRead = DMA_HWICAP_BITFILE_BUFFER_SIZE;

		err = bitstream_helper(icap, (u32 *)buffer,
			numCharsRead / sizeof(u32));
		if (err)
			goto free_buffers;
		buffer += numCharsRead;
	}

	err = wait_for_done(icap);

free_buffers:
	kfree(bit_header.DesignName);
	kfree(bit_header.PartName);
	kfree(bit_header.Date);
	kfree(bit_header.Time);
	return err;
}

static int icap_download_hw(struct xocl_icap *icap, const struct axlf *axlf)
{
	uint64_t primaryFirmwareOffset = 0;
	uint64_t primaryFirmwareLength = 0;
	const struct axlf_section_header *primaryHeader = 0;
	uint64_t length;
	int err = 0;
	char *buffer = (char *)axlf;

	if (!axlf) {
		err = -EINVAL;
		goto done;
	}

	length = axlf->m_header.m_length;

	primaryHeader = get_axlf_section_hdr(icap, axlf, BITSTREAM);

	if (primaryHeader) {
		primaryFirmwareOffset = primaryHeader->m_sectionOffset;
		primaryFirmwareLength = primaryHeader->m_sectionSize;
	}

	if ((primaryFirmwareOffset + primaryFirmwareLength) > length) {
		ICAP_ERR(icap, "Invalid BITSTREAM size");
		err = -EINVAL;
		goto done;
	}

	if (primaryFirmwareLength) {
		ICAP_INFO(icap,
			"found second stage bitstream of size 0x%llx",
			primaryFirmwareLength);
		err = icap_download(icap, buffer + primaryFirmwareOffset,
			primaryFirmwareLength);
		if (err) {
			ICAP_ERR(icap, "Dowload bitstream failed");
			goto done;
		}
	}

done:
	ICAP_INFO(icap, "%s, err = %d", __func__, err);
	return err;
}

static int icap_download_bitstream(struct xocl_icap *icap, const struct axlf *axlf)
{
	long err = 0;

	icap_freeze_axi_gate(icap);

	err = icap_download_hw(icap, axlf);
	/*
	 * Perform frequency scaling since PR download can silenty overwrite
	 * MMCM settings in static region changing the clock frequencies
	 * although ClockWiz CONFIG registers will misleading report the older
	 * configuration from before bitstream download as if nothing has
	 * changed.
	 */
	if (!err)
		err = icap_ocl_freqscaling(icap, true);

	icap_free_axi_gate(icap);
	return err;
}




long icap_ioctl(struct platform_device *pdev, unsigned int cmd, unsigned long arg)
{
	xocl_info(&pdev->dev, "Subdev %s ioctl %d %ld\n", pdev->name, cmd, arg);
	return 0;
}
