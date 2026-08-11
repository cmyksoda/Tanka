/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include <asm_defs.h>


#define BOOT_STACK_SIZE	0x10000


// DOL entry point: libogc's crt0 contract - CPU/BAT setup, EABI small data
// bases in r2/r13, cleared bss, and a stack of our own.
.text

FUNCTION(_start):
	bl		PPCEarlyInit

	lis		%r2, _SDA2_BASE_@h
	ori		%r2, %r2, _SDA2_BASE_@l
	lis		%r13, _SDA_BASE_@h
	ori		%r13, %r13, _SDA_BASE_@l

	lis		%r3, __bss_start@h
	ori		%r3, %r3, __bss_start@l
	lis		%r4, _end@h
	ori		%r4, %r4, _end@l
	li		%r0, 0
2:
	cmplw	%r3, %r4
	bge		3f
	stw		%r0, 0(%r3)
	addi	%r3, %r3, 4
	b		2b
3:
	lis		%r1, (sBootStack + BOOT_STACK_SIZE - 16)@h
	ori		%r1, %r1, (sBootStack + BOOT_STACK_SIZE - 16)@l
	stw		%r0, 0(%r1)
	stw		%r0, 4(%r1)
	mtlr	%r0

	b		wii_start


.bss
.balign 32
sBootStack:
	.space	BOOT_STACK_SIZE
