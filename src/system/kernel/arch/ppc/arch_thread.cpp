/*
 * Copyright 2003-2011, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 * 		Axel Dörfler <axeld@pinc-software.de>
 * 		Ingo Weinhold <bonefish@cs.tu-berlin.de>
 *
 * Copyright 2001, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */


#include <arch/cpu.h>
#include <arch/int.h>
#include <interrupts.h>
#include <smp.h>
#include <arch/thread.h>
#include <boot/stage2.h>
#include <kernel.h>
#include <thread.h>
#include <vm/vm_types.h>
#include <vm/VMAddressSpace.h>
//#include <arch/vm_translation_map.h>

#include <string.h>

// Valid initial arch_thread state. We just memcpy() it when initializing
// a new thread structure.
static struct arch_thread sInitialState;

// Helper function for thread creation, defined in arch_asm.S.
extern "C" void ppc_kernel_thread_root();


void
ppc_push_iframe(struct iframe_stack *stack, struct iframe *frame)
{
	ASSERT(stack->index < IFRAME_TRACE_DEPTH);
	stack->frames[stack->index++] = frame;
}


void
ppc_pop_iframe(struct iframe_stack *stack)
{
	ASSERT(stack->index > 0);
	stack->index--;
}


/**	Returns the current iframe structure of the running thread.
 *	This function must only be called in a context where it's actually
 *	sure that such iframe exists; ie. from syscalls, but usually not
 *	from standard kernel threads.
 */
static struct iframe *
ppc_get_current_iframe(void)
{
	Thread *thread = thread_get_current_thread();

	ASSERT(thread->arch_info.iframes.index >= 0);
	return thread->arch_info.iframes.frames[thread->arch_info.iframes.index - 1];
}


/** \brief Returns the current thread's topmost (i.e. most recent)
 *  userland->kernel transition iframe (usually the first one, save for
 *  interrupts in signal handlers).
 *  \return The iframe, or \c NULL, if there is no such iframe (e.g. when
 *          the thread is a kernel thread).
 */
struct iframe *
ppc_get_user_iframe(void)
{
	Thread *thread = thread_get_current_thread();
	int i;

	for (i = thread->arch_info.iframes.index - 1; i >= 0; i--) {
		struct iframe *frame = thread->arch_info.iframes.frames[i];
		if (frame->srr1 & MSR_PRIVILEGE_LEVEL)
			return frame;
	}

	return NULL;
}


// #pragma mark -


status_t
arch_thread_init(struct kernel_args *args)
{
	// Initialize the static initial arch_thread state (sInitialState).
	// Currently nothing to do, i.e. zero initialized is just fine.

	return B_OK;
}


status_t
arch_team_init_team_struct(Team *team, bool kernel)
{
	// Nothing to do. The structure is empty.
	return B_OK;
}


status_t
arch_thread_init_thread_struct(Thread *thread)
{
	// set up an initial state (stack & fpu)
	memcpy(&thread->arch_info, &sInitialState, sizeof(struct arch_thread));

	return B_OK;
}


void
arch_thread_init_kthread_stack(Thread* thread, void* _stack, void* _stackTop,
	void (*function)(void*), const void* data)
{
	addr_t *kstackTop = (addr_t *)_stackTop;

	// space for frame pointer and return address, and stack frames must be
	// 16 byte aligned
	kstackTop -= 2;
	kstackTop = (addr_t*)((addr_t)kstackTop & ~0xf);

	// LR, CR, r2, r3, r13-r31, f13-f31, as restored by ppc_context_switch()
	kstackTop -= 23 + 2 * 19;
	memset(kstackTop, 0, (23 + 2 * 19) * sizeof(addr_t));

	// Let LR point directly at the thread's entry function, and smuggle
	// "data" into r3 (the first argument register in the PowerPC SVR4
	// calling convention) - when ppc_context_switch() restores this brand
	// new thread for the first time, its closing "blr" branches straight
	// into function(data). This mirrors every other working architecture's
	// implementation of this function (e.g. arch/arm/arch_thread.cpp); the
	// old three-function ppc_kernel_thread_root() trampoline this function
	// used to target is a leftover from a since-removed thread-creation
	// API and no longer matches this function's (function, data) signature.
	kstackTop[0] = (addr_t)function;
	kstackTop[3] = (addr_t)data;

	// save this stack position
	thread->arch_info.sp = (void *)kstackTop;
}


status_t
arch_thread_init_tls(Thread *thread)
{
	thread->user_local_storage =
		thread->user_stack_base + thread->user_stack_size;
	return B_OK;
}


void
arch_thread_context_switch(Thread *t_from, Thread *t_to)
{
	// Record the new thread's kernel stack in this CPU's exception context.
	// The exception entry code (ppc_exception_tail) reads context->kernel_stack
	// to know which stack to switch to when an exception is taken from user
	// mode. (The previous EAR write was dead - nothing ever read EAR back.)
	ppc_get_cpu_exception_context(smp_get_current_cpu())->kernel_stack
		= (void*)(t_to->kernel_stack_top - 8);

    // switch the asids if we need to
	if (t_to->team->address_space != NULL) {
		// the target thread has is user space
		if (t_from->team != t_to->team) {
			// switching to a new address space
			ppc_translation_map_change_asid(
				t_to->team->address_space->TranslationMap());
		}
	}

	ppc_context_switch(&t_from->arch_info.sp, t_to->arch_info.sp);
}


void
arch_thread_dump_info(void *info)
{
	struct arch_thread *at = (struct arch_thread *)info;

	dprintf("\tsp: %p\n", at->sp);
}


extern "C" void ppc_enter_userspace(struct iframe *frame);

status_t
arch_thread_enter_userspace(Thread *thread, addr_t entry, void *arg1, void *arg2)
{
	addr_t stackTop = thread->user_stack_base + thread->user_stack_size;

	// Make sure this CPU's exception context points at our kernel stack, so
	// the first syscall/interrupt taken from user mode lands on a valid stack.
	ppc_get_cpu_exception_context(smp_get_current_cpu())->kernel_stack
		= (void*)(thread->kernel_stack_top - 8);

	// Build a synthetic iframe describing the initial user state and let the
	// standard exception-return path (rfi) drop us into user mode.
	struct iframe frame;
	memset(&frame, 0, sizeof(frame));

	frame.srr0 = entry;
	frame.srr1 = MSR_PRIVILEGE_LEVEL | MSR_EXCEPTIONS_ENABLED
		| MSR_FP_AVAILABLE | MSR_MACHINE_CHECK_ENABLED
		| MSR_INST_ADDRESS_TRANSLATION | MSR_DATA_ADDRESS_TRANSLATION;

	// 16-byte align the user stack and lay down an empty initial frame
	// (back chain NULL) per the SysV ABI.
	stackTop &= ~0xf;
	stackTop -= 16;
	*(uint32*)stackTop = 0;

	frame.r1 = stackTop;					// user stack pointer
	frame.r2 = thread->user_local_storage;	// TLS pointer (ppc32)
	frame.r3 = (uint32)arg1;
	frame.r4 = (uint32)arg2;

	disable_interrupts();
	ppc_enter_userspace(&frame);

	// never reached
	return B_ERROR;
}


bool
arch_on_signal_stack(Thread *thread)
{
	return false;
}


status_t
arch_setup_signal_frame(Thread *thread, struct sigaction *sa,
	struct signal_frame_data *signalFrameData)
{
	return B_ERROR;
}


int64
arch_restore_signal_frame(struct signal_frame_data* signalFrameData)
{
	return 0;
}



/**	Saves everything needed to restore the frame in the child fork in the
 *	arch_fork_arg structure to be passed to arch_restore_fork_frame().
 *	Also makes sure to return the right value.
 */

void
arch_store_fork_frame(struct arch_fork_arg *arg)
{
}


/** Restores the frame from a forked team as specified by the provided
 *	arch_fork_arg structure.
 *	Needs to be called from within the child team, ie. instead of
 *	arch_thread_enter_uspace() as thread "starter".
 *	This function does not return to the caller, but will enter userland
 *	in the child team at the same position where the parent team left of.
 */

void
arch_restore_fork_frame(struct arch_fork_arg *arg)
{
}

