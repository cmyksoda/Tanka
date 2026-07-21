/*
** Copyright 2001, Travis Geiselbrecht. All rights reserved.
** Copyright 2026, Haiku, Inc. All rights reserved.
** Distributed under the terms of the NewOS License.
*/

/*
	PowerPC (32-bit SysV ABI) userland system call stubs.

	The C caller has already placed the first eight integer/pointer arguments
	in r3-r10 and any overflow in its parameter save area (sp+8), exactly the
	layout the kernel's syscall dispatcher expects. A 64-bit argument occupies
	the next consecutive register pair (no even-register alignment on this
	ABI). So a stub only has to load the syscall number into r0 and trap with
	`sc`; on return the value is already in r3 (r3:r4 for 64-bit returns),
	right where the C ABI wants it, and we simply branch back to the caller.
*/

#define SYSCALL_STUB(name, n) \
.globl name; \
.type name,@function; \
.align 4; \
name: \
	li		%r0, n; \
	sc; \
	blr

#define SYSCALL0(name, n)  SYSCALL_STUB(name, n)
#define SYSCALL1(name, n)  SYSCALL_STUB(name, n)
#define SYSCALL2(name, n)  SYSCALL_STUB(name, n)
#define SYSCALL3(name, n)  SYSCALL_STUB(name, n)
#define SYSCALL4(name, n)  SYSCALL_STUB(name, n)
#define SYSCALL5(name, n)  SYSCALL_STUB(name, n)
#define SYSCALL6(name, n)  SYSCALL_STUB(name, n)
#define SYSCALL7(name, n)  SYSCALL_STUB(name, n)
#define SYSCALL8(name, n)  SYSCALL_STUB(name, n)
#define SYSCALL9(name, n)  SYSCALL_STUB(name, n)
#define SYSCALL10(name, n) SYSCALL_STUB(name, n)
#define SYSCALL11(name, n) SYSCALL_STUB(name, n)
#define SYSCALL12(name, n) SYSCALL_STUB(name, n)
#define SYSCALL13(name, n) SYSCALL_STUB(name, n)
