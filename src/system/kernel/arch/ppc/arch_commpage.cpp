/*
 * Copyright 2007, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <commpage.h>

#include <string.h>

#include <KernelExport.h>

#include <cpu.h>
#include <smp.h>


extern "C" void ppc_userspace_thread_exit(void);
extern "C" void ppc_end_userspace_thread_exit(void);


status_t
arch_commpage_init(void)
{
	/* no optimized memcpy or anything yet, and we don't use it for the
	   syscall path either - but the thread-exit stub is essential: it is
	   where a returning userland thread lands (via its initial LR) to call
	   exit_thread() instead of branching to a null link register. */
	size_t threadExitLen = (addr_t)ppc_end_userspace_thread_exit
		- (addr_t)ppc_userspace_thread_exit;
	fill_commpage_entry(COMMPAGE_ENTRY_PPC_THREAD_EXIT,
		(const void*)ppc_userspace_thread_exit, threadExitLen);

	return B_OK;
}


status_t
arch_commpage_init_post_cpus(void)
{
	return B_OK;
}
