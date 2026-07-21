/*
 * Copyright 2005, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/**	This file contains the first part of the ".init" and ".fini" sections in
 *	the ELF executable.
 *	The functions defined here will be called during initialization/termination
 *	of the loaded executable/library. The ".init" and ".fini" sections are
 *	stacked together like this:
 *
 *	crti.S		entry point
 *				call to _init_before/_term_before
 *	crtbegin.S	GCC specific: constructors/destructors are called, ...
 *	crtend.S
 *	crtn.S		call to _init_after/_term_after
 *				exit
 *
 *	crti.S opens a stack frame and saves the return address (LR); the body that
 *	crtbegin/crtend paste in between makes `bl` calls that clobber LR, and
 *	crtn.S restores it before returning. Without this prologue/epilogue pair the
 *	final `blr` would branch to whatever the last internal `bl` left in LR - i.e.
 *	back into _init itself - and spin forever.
 */

#define FUNCTION(x) .global x; .type x,@function; x

.section .init
FUNCTION(_init):
	stwu	%r1, -16(%r1)
	mflr	%r0
	stw	%r0, 20(%r1)
	// crtbegin.o stuff comes here

.section .fini
FUNCTION(_fini):
	stwu	%r1, -16(%r1)
	mflr	%r0
	stw	%r0, 20(%r1)
	// crtbegin.o stuff comes here
