/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_PLATFORM_WII_SDIO_H
#define _KERNEL_PLATFORM_WII_SDIO_H

#include <SupportDefs.h>


// The IOS resource manager the boot loader and the kernel driver both talk to.
#define WII_SDIO_DEVICE					"/dev/sdio/slot0"
#define WII_SDIO_SECTOR_SIZE			512

// /dev/sdio/slot0 ioctls.
#define WII_SDIO_IOCTL_WRITEHCREG		0x01
#define WII_SDIO_IOCTL_READHCREG		0x02
#define WII_SDIO_IOCTL_READCREG			0x03
#define WII_SDIO_IOCTL_RESETCARD		0x04
#define WII_SDIO_IOCTL_WRITECREG		0x05
#define WII_SDIO_IOCTL_SETCLK			0x06
#define WII_SDIO_IOCTL_SENDCMD			0x07
#define WII_SDIO_IOCTL_SETBUSWIDTH		0x08
#define WII_SDIO_IOCTL_READMCREG		0x09
#define WII_SDIO_IOCTL_WRITEMCREG		0x0a
#define WII_SDIO_IOCTL_GETSTATUS		0x0b
#define WII_SDIO_IOCTL_GETOCR			0x0c
#define WII_SDIO_IOCTL_READDATA			0x0d
#define WII_SDIO_IOCTL_WRITEDATA		0x0e

// SD host controller registers, reached through the HCREG ioctls.
#define WII_SDIOHCR_RESPONSE			0x10
#define WII_SDIOHCR_HOSTCONTROL			0x28
#define WII_SDIOHCR_POWERCONTROL		0x29
#define WII_SDIOHCR_CLOCKCONTROL		0x2c
#define WII_SDIOHCR_TIMEOUTCONTROL		0x2e
#define WII_SDIOHCR_SOFTWARERESET		0x2f

#define WII_SDIOHCR_HOSTCONTROL_4BIT	0x02
#define WII_SDIOHCR_HOSTCONTROL_HS		0x04
#define WII_SDIOHCR_CLOCK_STABLE		0x02

#define WII_SDIO_DEFAULT_TIMEOUT		0x0e

// Status word returned by GETSTATUS and, with the RCA on top, by RESETCARD.
#define WII_SDIO_STATUS_CARD_INSERTED		0x00000001
#define WII_SDIO_STATUS_CARD_LOCKED			0x00000004
#define WII_SDIO_STATUS_CARD_INITIALIZED	0x00010000
#define WII_SDIO_STATUS_CARD_SDHC			0x00100000

// Command classes, as IOS expects them in wii_sdio_request::cmd_type.
#define WII_SDIO_TYPE_BC				1
#define WII_SDIO_TYPE_BCR				2
#define WII_SDIO_TYPE_AC				3
#define WII_SDIO_TYPE_ADTC				4

#define WII_SDIO_RESPONSE_NONE			0
#define WII_SDIO_RESPONSE_R1			1
#define WII_SDIO_RESPONSE_R1B			2
#define WII_SDIO_RESPONSE_R2			3
#define WII_SDIO_RESPONSE_R3			4
#define WII_SDIO_RESPONSE_R4			5
#define WII_SDIO_RESPONSE_R5			6
#define WII_SDIO_RESPONSE_R6			7

#define WII_SDIO_CMD_GOIDLE				0x00
#define WII_SDIO_CMD_ALL_SENDCID		0x02
#define WII_SDIO_CMD_SENDRCA			0x03
#define WII_SDIO_CMD_SWITCHFUNC			0x06
#define WII_SDIO_CMD_SELECT				0x07
#define WII_SDIO_CMD_DESELECT			0x07
#define WII_SDIO_CMD_SENDIFCOND			0x08
#define WII_SDIO_CMD_SENDCSD			0x09
#define WII_SDIO_CMD_SENDCID			0x0a
#define WII_SDIO_CMD_SENDSTATUS			0x0d
#define WII_SDIO_CMD_SETBLOCKLEN		0x10
#define WII_SDIO_CMD_READBLOCK			0x11
#define WII_SDIO_CMD_READMULTIBLOCK		0x12
#define WII_SDIO_CMD_WRITEBLOCK			0x18
#define WII_SDIO_CMD_WRITEMULTIBLOCK	0x19
#define WII_SDIO_CMD_APPCMD				0x37

#define WII_SDIO_ACMD_SETBUSWIDTH		0x06
#define WII_SDIO_ACMD_SENDOPCOND		0x29
#define WII_SDIO_ACMD_SENDSCR			0x33

// ACMD6 bus width selection, and the CMD8 check pattern echoed back in R6.
#define WII_SDIO_BUSWIDTH_1BIT			0x0000
#define WII_SDIO_BUSWIDTH_4BIT			0x0002
#define WII_SDIO_IFCOND_ARG				0x000001aa
#define WII_SDIO_IFCOND_PATTERN			0xaa

// ACMD41 host capacity support argument, and the OCR bits it answers with.
#define WII_SDIO_OPCOND_ARG				0x40300000
#define WII_SDIO_OCR_BUSY				0x80000000
#define WII_SDIO_OCR_CCS				0x40000000


// The 0x24 byte SENDCMD payload; field order and width are fixed by IOS.
struct wii_sdio_request {
	uint32	cmd;
	uint32	cmd_type;
	uint32	rsp_type;
	uint32	arg;
	uint32	blk_cnt;
	uint32	blk_size;
	uint32	dma_addr;
	uint32	isdma;
	uint32	pad0;
};

// The 0x10 byte reply buffer IOS fills in for every SENDCMD.
struct wii_sdio_response {
	uint32	rsp_fields[3];
	uint32	acmd12_response;
};


// Capacity from a CMD9 reply: CSD in natural order, word 0 is bits 127:96.
static inline uint64
wii_sdio_csd_capacity(const uint32* csd)
{
	if (((csd[0] >> 30) & 0x3) == 1) {
		// CSD version 2: C_SIZE at bits 69:48 counts 512KB units.
		uint32 size = ((csd[1] & 0x3f) << 16) | (csd[2] >> 16);
		return ((uint64)size + 1) * 512 * 1024;
	}

	// CSD version 1: C_SIZE 73:62, C_SIZE_MULT 49:47, READ_BL_LEN 83:80.
	uint32 size = ((csd[1] & 0x3ff) << 2) | (csd[2] >> 30);
	uint32 sizeMult = (csd[2] >> 15) & 0x7;
	uint32 readBlockLen = (csd[1] >> 16) & 0xf;

	return ((uint64)size + 1) << (sizeMult + 2 + readBlockLen);
}

#endif	// _KERNEL_PLATFORM_WII_SDIO_H
