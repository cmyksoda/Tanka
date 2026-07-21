/*
 * Copyright 2005, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/**	This file contains the final part of the ".init" and ".fini" sections in
 *	the ELF executable. It is tightly connected to crti.S.
 *	Have a look at crti.S to find a description of what happens here.
 *
 *	Restore the return address (LR) that crti.S saved - the pasted-in body
 *	clobbered it with `bl` - tear the stack frame back down, and return.
 */

.section .init
	lwz	%r0, 20(%r1)
	mtlr	%r0
	addi	%r1, %r1, 16
	blr

.section .fini
	lwz	%r0, 20(%r1)
	mtlr	%r0
	addi	%r1, %r1, 16
	blr
