/*
 * Copyright 2007, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <commpage.h>

#include <string.h>

#include <KernelExport.h>

#include <cpu.h>
#include <elf.h>
#include <smp.h>

#include "syscall_numbers.h"


extern "C" void ppc_userspace_thread_exit(void);
extern "C" void ppc_end_userspace_thread_exit(void);


/*!	The signal handler trampoline that lives in the commpage. When the kernel
	delivers a signal it points the interrupted thread here (see
	arch_setup_signal_frame()), with r3 holding the signal_frame_data pointer.
	This calls the user's handler and then returns to the kernel via the
	_kern_restore_signal_frame() syscall, which restores the interrupted
	context. Because it is copied verbatim into the commpage it must be
	position independent - it only dereferences the passed pointer and issues
	an inline syscall, so it is.
*/
extern "C" void __attribute__((noreturn))
arch_user_signal_handler(signal_frame_data* data)
{
	if (data->siginfo_handler) {
		auto handler = (void (*)(int, siginfo_t*, void*, void*))data->handler;
		handler(data->info.si_signo, &data->info, &data->context,
			data->user_data);
	} else {
		auto handler = (void (*)(int, void*, vregs*))data->handler;
		handler(data->info.si_signo, data->user_data,
			&data->context.uc_mcontext);
	}

	// _kern_restore_signal_frame(data)
	register signal_frame_data* r3 asm("r3") = data;
	register uint32 r0 asm("r0") = SYSCALL_RESTORE_SIGNAL_FRAME;
	asm volatile("sc" : : "r"(r0), "r"(r3) : "memory");

	__builtin_unreachable();
}


static void
register_commpage_function(const char* functionName, int32 commpageIndex,
	const char* commpageSymbolName, addr_t expectedAddress)
{
	// get address and size of function
	elf_symbol_info symbolInfo;
	if (elf_lookup_kernel_symbol(functionName, &symbolInfo) != B_OK) {
		panic("register_commpage_function(): Failed to find "
			"function \"%s\"!", functionName);
	}

	ASSERT(expectedAddress == symbolInfo.address);

	// fill in the commpage table entry
	addr_t position = fill_commpage_entry(commpageIndex,
		(void*)symbolInfo.address, symbolInfo.size);

	// add symbol to the commpage image
	image_id image = get_commpage_image();
	elf_add_memory_image_symbol(image, commpageSymbolName, position,
		symbolInfo.size, B_SYMBOL_TYPE_TEXT);
}


status_t
arch_commpage_init(void)
{
	return B_OK;
}


status_t
arch_commpage_init_post_cpus(void)
{
	/* The thread-exit stub is essential: it is where a returning userland
	   thread lands (via its initial LR) to call exit_thread() instead of
	   branching to a null link register. */
	size_t threadExitLen = (addr_t)ppc_end_userspace_thread_exit
		- (addr_t)ppc_userspace_thread_exit;
	fill_commpage_entry(COMMPAGE_ENTRY_PPC_THREAD_EXIT,
		(const void*)ppc_userspace_thread_exit, threadExitLen);

	// signal handler trampoline
	register_commpage_function("arch_user_signal_handler",
		COMMPAGE_ENTRY_PPC_SIGNAL_HANDLER, "commpage_signal_handler",
		(addr_t)&arch_user_signal_handler);

	return B_OK;
}
