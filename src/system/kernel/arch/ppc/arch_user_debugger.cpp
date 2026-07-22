/*
 * Copyright 2005, Axel Dörfler, axeld@pinc-softare.de
 * Distributed under the terms of the MIT License.
 */


#include <debugger.h>
#include <interrupts.h>
#include <thread.h>
#include <arch/cpu.h>
#include <arch/user_debugger.h>


static void
copy_iframe_to_cpu_state(struct iframe* frame, debug_cpu_state* cpuState)
{
	cpuState->pc = frame->srr0;
	cpuState->msr = frame->srr1;
	cpuState->lr = frame->lr;
	cpuState->ctr = frame->ctr;
	cpuState->cr = frame->cr;
	cpuState->xer = frame->xer;
	cpuState->fpscr = frame->fpscr;
	cpuState->r0 = frame->r0;
	cpuState->r1 = frame->r1;
	cpuState->r2 = frame->r2;
	cpuState->r3 = frame->r3;
	cpuState->r4 = frame->r4;
	cpuState->r5 = frame->r5;
	cpuState->r6 = frame->r6;
	cpuState->r7 = frame->r7;
	cpuState->r8 = frame->r8;
	cpuState->r9 = frame->r9;
	cpuState->r10 = frame->r10;
	cpuState->r11 = frame->r11;
	cpuState->r12 = frame->r12;
	cpuState->r13 = frame->r13;
	cpuState->r14 = frame->r14;
	cpuState->r15 = frame->r15;
	cpuState->r16 = frame->r16;
	cpuState->r17 = frame->r17;
	cpuState->r18 = frame->r18;
	cpuState->r19 = frame->r19;
	cpuState->r20 = frame->r20;
	cpuState->r21 = frame->r21;
	cpuState->r22 = frame->r22;
	cpuState->r23 = frame->r23;
	cpuState->r24 = frame->r24;
	cpuState->r25 = frame->r25;
	cpuState->r26 = frame->r26;
	cpuState->r27 = frame->r27;
	cpuState->r28 = frame->r28;
	cpuState->r29 = frame->r29;
	cpuState->r30 = frame->r30;
	cpuState->r31 = frame->r31;
}


// Returns the given thread's most recent userland->kernel transition
// iframe, or NULL if there is none (e.g. a pure kernel thread).
static struct iframe*
get_thread_user_iframe(Thread* thread)
{
	for (int32 i = thread->arch_info.iframes.index - 1; i >= 0; i--) {
		struct iframe* frame = thread->arch_info.iframes.frames[i];
		if (frame->srr1 & MSR_PRIVILEGE_LEVEL)
			return frame;
	}
	return NULL;
}


void
arch_clear_team_debug_info(struct arch_team_debug_info *info)
{
}


void
arch_destroy_team_debug_info(struct arch_team_debug_info *info)
{
	arch_clear_team_debug_info(info);
}


void
arch_clear_thread_debug_info(struct arch_thread_debug_info *info)
{
}


void
arch_destroy_thread_debug_info(struct arch_thread_debug_info *info)
{
	arch_clear_thread_debug_info(info);
}


void
arch_update_thread_single_step()
{
}


void
arch_set_debug_cpu_state(const debug_cpu_state *cpuState)
{
}


void
arch_get_debug_cpu_state(debug_cpu_state *cpuState)
{
	struct iframe* frame = get_thread_user_iframe(thread_get_current_thread());
	if (frame != NULL)
		copy_iframe_to_cpu_state(frame, cpuState);
}


status_t
arch_get_thread_debug_cpu_state(Thread* thread, debug_cpu_state* cpuState)
{
	struct iframe* frame = get_thread_user_iframe(thread);
	if (frame == NULL)
		return B_BAD_VALUE;

	copy_iframe_to_cpu_state(frame, cpuState);
	return B_OK;
}


status_t
arch_set_breakpoint(void *address)
{
	return B_ERROR;
}


status_t
arch_clear_breakpoint(void *address)
{
	return B_ERROR;
}


status_t
arch_set_watchpoint(void *address, uint32 type, int32 length)
{
	return B_ERROR;
}


status_t
arch_clear_watchpoint(void *address)
{
	return B_ERROR;
}

bool
arch_has_breakpoints(struct arch_team_debug_info *info)
{
	return false;
}
