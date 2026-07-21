/*
** Copyright 2003, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
** Copyright 2026, Haiku, Inc. All rights reserved.
** Distributed under the terms of the MIT License.
*/

#include "support/TLS.h"
#include "tls.h"


/*	On 32-bit PowerPC (SysV ABI) r2 is the reserved thread pointer. The kernel
	points it at this thread's TLS slot array: thread creation fills
	TLS_BASE_ADDRESS_SLOT and copies the array to user_local_storage (see
	thread.cpp), and arch_thread_enter_userspace() / the context switch load r2
	with user_local_storage. So a slot is simply r2[index] - the same model the
	arm and x86 ports use, only with a different thread-pointer register. (The
	previous implementation kept the slots in a single process-global array,
	which was not per-thread and left pthread_self() etc. reading a bogus
	value.) */

static int32 gNextSlot = TLS_FIRST_FREE_SLOT;


static inline void**
get_tls(void)
{
	void** tls;
	__asm__ __volatile__("mr %0, 2" : "=r" (tls));
	return tls;
}


int32
tls_allocate(void)
{
	int32 next = atomic_add(&gNextSlot, 1);
	if (next >= TLS_MAX_KEYS)
		return B_NO_MEMORY;

	return next;
}


void *
tls_get(int32 index)
{
	return get_tls()[index];
}


void **
tls_address(int32 index)
{
	return get_tls() + index;
}


void
tls_set(int32 index, void *value)
{
	get_tls()[index] = value;
}
