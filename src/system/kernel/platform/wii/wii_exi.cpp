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
// EXI channel 1 device 0 is the USB Gecko the debug console writes to.

#define EXI_CSR				0x00
#define EXI_CR				0x0c
#define EXI_DATA			0x10
#define EXI_CHANNEL_SIZE	0x14

#define EXI_CSR_CLK_8MHZ	(3 << 4)
#define EXI_CSR_CLK_32MHZ	(5 << 4)
#define EXI_CSR_CS(device)	(1 << (7 + (device)))

#define EXI_CR_TSTART		(1 << 0)
#define EXI_CR_READ			(0 << 2)
#define EXI_CR_WRITE		(1 << 2)
#define EXI_CR_READWRITE	(2 << 2)
#define EXI_CR_LEN(bytes)	(((bytes) - 1) << 4)

#define EXI_RTC_READ		0x20000000
#define EXI_RTC_WRITE		0xa0000000
#define EXI_SRAM_READ		0x20000100

#define EXI_GECKO_CHANNEL	1

#define SRAM_COUNTER_BIAS	0x0c

// Seconds between the Unix epoch and the console epoch (2000-01-01).
#define WII_RTC_EPOCH_OFFSET	946684800UL

#define EXI_TRANSFER_TIMEOUT	100000


static addr_t sEXIBase;
static uint32 sCounterBias;
static bool sInitialized;


static inline volatile uint32 *
exi_reg(uint32 channel, uint32 offset)
{
	return (volatile uint32 *)(sEXIBase + channel * EXI_CHANNEL_SIZE + offset);
}


static bool
exi_wait(uint32 channel)
{
	for (int i = 0; i < EXI_TRANSFER_TIMEOUT; i++) {
		if ((*exi_reg(channel, EXI_CR) & EXI_CR_TSTART) == 0)
			return true;
	}
	return false;
}


static bool
exi_imm(uint32 channel, uint32 *data, uint32 length, uint32 direction)
{
	if (direction != EXI_CR_READ)
		*exi_reg(channel, EXI_DATA) = *data;

	eieio();
	*exi_reg(channel, EXI_CR) = EXI_CR_TSTART | direction | EXI_CR_LEN(length);
	eieio();

	if (!exi_wait(channel))
		return false;

	if (direction != EXI_CR_WRITE)
		*data = *exi_reg(channel, EXI_DATA);

	return true;
}


static bool
exi_command(uint32 command, uint32 *value, uint32 direction)
{
	*exi_reg(0, EXI_CSR) = EXI_CSR_CLK_8MHZ | EXI_CSR_CS(1);
	eieio();

	bool ok = exi_imm(0, &command, 4, EXI_CR_WRITE)
		&& exi_imm(0, value, 4, direction);

	*exi_reg(0, EXI_CSR) = 0;
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
	*exi_reg(0, EXI_CSR) = EXI_CSR_CLK_8MHZ | EXI_CSR_CS(1);
	eieio();

	uint32 command = EXI_SRAM_READ;
	bool ok = exi_imm(0, &command, 4, EXI_CR_WRITE);
	for (uint32 offset = 0; ok && offset <= SRAM_COUNTER_BIAS; offset += 4)
		ok = exi_imm(0, &sCounterBias, 4, EXI_CR_READ);

	*exi_reg(0, EXI_CSR) = 0;
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


// #pragma mark - USB Gecko debug console


/*!	TX is the 16 bit command 0xB000 with the byte in bits 4-11; bit 26 of the
	reply is set once the adapter's FIFO has accepted the byte.
*/
static bool
usbgecko_send_byte(char c)
{
	uint32 data = (uint32)(0xB000 | ((uint8)c << 4)) << 16;

	*exi_reg(EXI_GECKO_CHANNEL, EXI_CSR)
		= EXI_CSR_CLK_32MHZ | EXI_CSR_CS(0);
	eieio();

	bool ok = exi_imm(EXI_GECKO_CHANNEL, &data, 2, EXI_CR_READWRITE);

	*exi_reg(EXI_GECKO_CHANNEL, EXI_CSR) = 0;
	eieio();

	return ok && (data & 0x04000000) != 0;
}


/*!	RX is the 16 bit command 0xA000; bit 27 of the reply is set when a byte
	is waiting, which then sits in reply bits 16-23.
*/
static bool
usbgecko_receive_byte(char* _c)
{
	uint32 data = 0xA0000000;

	*exi_reg(EXI_GECKO_CHANNEL, EXI_CSR)
		= EXI_CSR_CLK_32MHZ | EXI_CSR_CS(0);
	eieio();

	bool ok = exi_imm(EXI_GECKO_CHANNEL, &data, 2, EXI_CR_READWRITE);

	*exi_reg(EXI_GECKO_CHANNEL, EXI_CSR) = 0;
	eieio();

	if (!ok || (data & 0x08000000) == 0)
		return false;

	*_c = (char)(data >> 16);
	return true;
}


status_t
wii_serial_debug_init(void)
{
	// Runs long before the VM can map the register area; until then the
	// device window the loader left mapped keeps the EXI block reachable.
	if (sEXIBase == 0)
		sEXIBase = 0xc0000000 + WII_HOLLYWOOD_PHYS_BASE + WII_HW_EXI;

	return B_OK;
}


void
wii_serial_debug_put_char(char c)
{
	if (sEXIBase == 0)
		return;

	// Bounded retry: the adapter's FIFO drains at USB pace mid-burst.
	for (int i = 0; i < 10000; i++) {
		if (usbgecko_send_byte(c))
			break;
	}
}


char
wii_serial_debug_get_char(void)
{
	if (sEXIBase == 0)
		return 0;

	// The kernel debugger expects a blocking read.
	char c;
	while (!usbgecko_receive_byte(&c))
		;
	return c;
}
