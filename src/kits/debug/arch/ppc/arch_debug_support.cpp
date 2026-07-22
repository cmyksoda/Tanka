/*
 * Copyright 2005, Ingo Weinhold, bonefish@users.sf.net.
 * Distributed under the terms of the MIT License.
 */


#include <debug_support.h>

#include "arch_debug_support.h"


// PowerPC stack frame: the stack pointer (r1) points at a word holding the
// back chain (the caller's frame pointer), immediately followed by the saved
// link register (the return address). This matches the kernel's own stack
// walker (see src/system/kernel/arch/ppc/arch_debug.cpp).
struct stack_frame {
	struct stack_frame*	previous;
	void*				return_address;
};


status_t
arch_debug_get_instruction_pointer(debug_context *context, thread_id thread,
	void **ip, void **stackFrameAddress)
{
	// get the CPU state
	debug_cpu_state cpuState;
	status_t error = debug_get_cpu_state(context, thread, NULL, &cpuState);
	if (error != B_OK)
		return error;

	*ip = (void*)cpuState.pc;
	*stackFrameAddress = (void*)cpuState.r1;

	return B_OK;
}


status_t
arch_debug_get_stack_frame(debug_context *context, void *stackFrameAddress,
	debug_stack_frame_info *stackFrameInfo)
{
	stack_frame stackFrame;
	ssize_t bytesRead = debug_read_memory(context, stackFrameAddress,
		&stackFrame, sizeof(stackFrame));
	if (bytesRead < 0)
		return bytesRead;
	if (bytesRead != sizeof(stackFrame))
		return B_ERROR;

	stackFrameInfo->frame = stackFrameAddress;
	stackFrameInfo->parent_frame = stackFrame.previous;
	stackFrameInfo->return_address = stackFrame.return_address;

	return B_OK;
}
