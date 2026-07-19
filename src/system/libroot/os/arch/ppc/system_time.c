/*
 * Copyright 2006, Ingo Weinhold <bonefish@cs.tu-berlin.de>.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include <OS.h>

#include <arch_cpu.h>
#include <libroot_private.h>
#include <real_time_data.h>


static vint32 *sConversionFactor;

void
__ppc_setup_system_time(vint32 *cvFactor)
{
	sConversionFactor = cvFactor;
}


bigtime_t
system_time(void)
{
	// On ppc, the kernel calls system_time() (e.g. for early memory-
	// reservation timeouts in commpage_init(), which itself runs before
	// _start() gets to timer_init()/rtc_init()) before
	// __ppc_setup_system_time() has ever been called - sConversionFactor
	// is still its zero-initialized default (NULL) at that point. Guard
	// against dereferencing it; returning 0 is harmless this early (no
	// caller at this stage depends on real elapsed time, only on not
	// crashing), and every later call - once rtc_init() has run - gets the
	// real calibrated value as before.
	vint32 *conversionFactor = sConversionFactor;
	if (conversionFactor == NULL)
		return 0;

	uint64 timeBase = __ppc_get_time_base();

	uint32 cv = *conversionFactor;
	return (timeBase >> 32) * cv + (((timeBase & 0xffffffff) * cv) >> 32);
}
