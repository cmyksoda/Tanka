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


// libogc's console sink, undeclared by any public header; also feeds the gecko
extern "C" ssize_t __console_write(void *reent, void *fd, const char *buffer,
	size_t length);


static char sBuffer[16384];
static uint32 sBufferPosition;
static bool sConsoleReady;


//! Kept in RAM too: on a hang this is the only record of how far we got.
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
	// Dolphin logs unimplemented register accesses, address included
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
