/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <platform/wii/wii.h>

#include <KernelExport.h>
#include <arch/cpu.h>


// EXI channel 0 device 1 is the RTC/SRAM/UART chip. The RTC is a free-running
// 32 bit second counter zeroed at 2000-01-01; user-visible time is that
// counter plus the bias word kept in SRAM, so we read the bias once and apply
// it in both directions rather than rewriting (and re-checksumming) SRAM.

#define EXI_CSR				0x00
#define EXI_CR				0x0c
#define EXI_DATA			0x10
#define EXI_CHANNEL_SIZE	0x14

#define EXI_CSR_CLK_8MHZ	(3 << 4)
#define EXI_CSR_CS(device)	(1 << (7 + (device)))

#define EXI_CR_TSTART		(1 << 0)
#define EXI_CR_READ			(0 << 2)
#define EXI_CR_WRITE		(1 << 2)
#define EXI_CR_LEN(bytes)	(((bytes) - 1) << 4)

#define EXI_RTC_READ		0x20000000
#define EXI_RTC_WRITE		0xa0000000
#define EXI_SRAM_READ		0x20000100

#define SRAM_COUNTER_BIAS	0x0c

// Seconds between the Unix epoch and the console epoch (2000-01-01).
#define WII_RTC_EPOCH_OFFSET	946684800UL

#define EXI_TRANSFER_TIMEOUT	100000


static addr_t sEXIBase;
static uint32 sCounterBias;
static bool sInitialized;


static inline volatile uint32 *
exi_reg(uint32 offset)
{
	return (volatile uint32 *)(sEXIBase + offset);
}


static bool
exi_wait(void)
{
	for (int i = 0; i < EXI_TRANSFER_TIMEOUT; i++) {
		if ((*exi_reg(EXI_CR) & EXI_CR_TSTART) == 0)
			return true;
	}
	return false;
}


static bool
exi_imm(uint32 *data, uint32 length, uint32 direction)
{
	if (direction == EXI_CR_WRITE)
		*exi_reg(EXI_DATA) = *data;

	eieio();
	*exi_reg(EXI_CR) = EXI_CR_TSTART | direction | EXI_CR_LEN(length);
	eieio();

	if (!exi_wait())
		return false;

	if (direction == EXI_CR_READ)
		*data = *exi_reg(EXI_DATA);

	return true;
}


static bool
exi_command(uint32 command, uint32 *value, uint32 direction)
{
	*exi_reg(EXI_CSR) = EXI_CSR_CLK_8MHZ | EXI_CSR_CS(1);
	eieio();

	bool ok = exi_imm(&command, 4, EXI_CR_WRITE)
		&& exi_imm(value, 4, direction);

	*exi_reg(EXI_CSR) = 0;
	eieio();

	return ok;
}


status_t
wii_rtc_init(void)
{
	if (sInitialized)
		return B_OK;

	addr_t hollywood = wii_hollywood_registers();
	if (hollywood == 0)
		return B_NO_INIT;

	sEXIBase = hollywood + WII_HW_EXI;

	// The bias sits 12 bytes into SRAM; the chip auto-increments, so step the
	// read address word by word up to it.
	*exi_reg(EXI_CSR) = EXI_CSR_CLK_8MHZ | EXI_CSR_CS(1);
	eieio();

	uint32 command = EXI_SRAM_READ;
	bool ok = exi_imm(&command, 4, EXI_CR_WRITE);
	for (uint32 offset = 0; ok && offset <= SRAM_COUNTER_BIAS; offset += 4)
		ok = exi_imm(&sCounterBias, 4, EXI_CR_READ);

	*exi_reg(EXI_CSR) = 0;
	eieio();

	if (!ok) {
		dprintf("wii_rtc_init(): SRAM read failed, assuming zero bias\n");
		sCounterBias = 0;
	}

	sInitialized = true;
	return B_OK;
}


uint32
wii_rtc_get(void)
{
	if (!sInitialized)
		return 0;

	uint32 counter = 0;
	if (!exi_command(EXI_RTC_READ, &counter, EXI_CR_READ)) {
		dprintf("wii_rtc_get(): RTC read failed\n");
		return 0;
	}

	return counter + sCounterBias + WII_RTC_EPOCH_OFFSET;
}


void
wii_rtc_set(uint32 seconds)
{
	if (!sInitialized)
		return;

	if (seconds < WII_RTC_EPOCH_OFFSET + sCounterBias)
		return;

	uint32 counter = seconds - WII_RTC_EPOCH_OFFSET - sCounterBias;
	if (!exi_command(EXI_RTC_WRITE, &counter, EXI_CR_WRITE))
		dprintf("wii_rtc_set(): RTC write failed\n");
}
