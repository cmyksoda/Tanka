/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */


#include <stdarg.h>
#include <string.h>

#include <boot/platform.h>
#include <boot/stdio.h>

#include <gccore.h>

#include "debug.h"


// libogc's console sink. Writes to the framebuffer console set up by CON_Init()
// and, once CON_EnableGecko() has run, mirrors everything to a USB Gecko in the
// memory card slot. No public libogc header declares it.
extern "C" ssize_t __console_write(void *reent, void *fd, const char *buffer,
	size_t length);


static char sBuffer[16384];
static uint32 sBufferPosition;
static bool sConsoleReady;


/*!	Keeps the output in RAM as well: the Wii has no serial port by default, so
	on a hang this buffer is the only record of how far the loader got. It is
	handed to the kernel as the boot loader's debug syslog.
*/
static inline void
syslog_write(const char* buffer, size_t length)
{
	if (sBufferPosition + length > sizeof(sBuffer))
		return;
	memcpy(sBuffer + sBufferPosition, buffer, length);
	sBufferPosition += length;
}


void
debug_init(void)
{
	sConsoleReady = true;

	// Harmless if no USB Gecko is plugged in - libogc only sends to it when
	// the channel answers.
	CON_EnableGecko(CARD_SLOTB, false);
}


void
debug_write(const char* buffer, size_t length)
{
	if (length == 0)
		return;

#ifdef WII_TRACE_TO_MMIO
	// Dolphin logs every access to an unimplemented Hollywood register at
	// error level, address included, which is the only output channel that
	// works in a headless emulator with no USB Gecko.
	for (size_t i = 0; i < length; i++)
		*(volatile uint8 *)(0xcd00f000 + (uint8)buffer[i]) = 0;
#endif

	syslog_write(buffer, length);

	if (sConsoleReady)
		__console_write(NULL, NULL, buffer, length);
}


static inline void
dprintf_args(const char *format, va_list args)
{
	char buffer[512];
	int length = vsnprintf(buffer, sizeof(buffer), format, args);
	if (length <= 0)
		return;
	if ((size_t)length >= sizeof(buffer))
		length = sizeof(buffer) - 1;

	debug_write(buffer, length);
}


extern "C" void
dprintf(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	dprintf_args(format, args);
	va_end(args);
}


extern "C" void
panic(const char* format, ...)
{
	va_list args;

	debug_write("\n*** PANIC ***\n", 15);

	va_start(args, format);
	dprintf_args(format, args);
	va_end(args);

	debug_write("\nboot loader stopped.\n", 22);

	while (true)
		;
}


char*
platform_debug_get_log_buffer(size_t* _size)
{
	if (_size != NULL)
		*_size = sizeof(sBuffer);

	return sBuffer;
}
