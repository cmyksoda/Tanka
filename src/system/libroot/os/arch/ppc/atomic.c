/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*! GCC emits calls to __atomic_*_8 whenever it needs a lock-free 64-bit
	atomic operation that the target CPU cannot provide natively - which is
	always the case on 32-bit PowerPC, since lwarx/stwcx only reserve a
	single 32-bit word. These implement that fallback with a small spinlock
	built from lwarx/stwcx (which *is* natively atomic at 32-bit width)
	guarding a plain, non-atomic 64-bit load/modify/store.

	Used by both the kernel (src/system/kernel/lib/arch/ppc/Jamfile) and
	userland libroot (src/system/libroot/os/arch/ppc/Jamfile).
*/

#include <stdint.h>


static volatile uint32_t sAtomic64Lock = 0;


static inline void
acquire_lock(void)
{
	uint32_t temp;
	__asm__ __volatile__(
		"1:  lwarx   %0, 0, %1\n"
		"    cmpwi   0, %0, 0\n"
		"    bne-    1b\n"
		"    stwcx.  %2, 0, %1\n"
		"    bne-    1b\n"
		"    isync\n"
		: "=&r" (temp)
		: "r" (&sAtomic64Lock), "r" (1)
		: "cc", "memory");
}


static inline void
release_lock(void)
{
	__asm__ __volatile__("sync" ::: "memory");
	sAtomic64Lock = 0;
}


uint64_t
__atomic_load_8(const uint64_t* ptr, int memorder)
{
	(void)memorder;
	acquire_lock();
	uint64_t value = *ptr;
	release_lock();
	return value;
}


void
__atomic_store_8(uint64_t* ptr, uint64_t value, int memorder)
{
	(void)memorder;
	acquire_lock();
	*ptr = value;
	release_lock();
}


uint64_t
__atomic_fetch_add_8(uint64_t* ptr, uint64_t value, int memorder)
{
	(void)memorder;
	acquire_lock();
	uint64_t oldValue = *ptr;
	*ptr = oldValue + value;
	release_lock();
	return oldValue;
}
