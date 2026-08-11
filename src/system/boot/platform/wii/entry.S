/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include <asm_defs.h>


#define BOOT_STACK_SIZE	0x10000


/*	Entry point of the DOL. Mirrors what libogc's crt0 (__app_start) does before
	it would call main(): PPCEarlyInit() puts the CPU into the state libogc
	expects (HID setup, caches, the standard MEM1/MEM2 BATs, translation on),
	r2/r13 are the EABI small data bases every libogc object addresses its
	globals through, and .sbss/.bss have to be zeroed before the first C code
	runs. We keep our own stack rather than the one the Homebrew Channel left
	in r1, which points into memory the loader hands to the kernel later.
*/
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
