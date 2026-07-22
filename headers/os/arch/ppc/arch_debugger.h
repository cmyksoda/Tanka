/*
 * Copyright 2005-2009, Haiku Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef _ARCH_PPC_DEBUGGER_H
#define _ARCH_PPC_DEBUGGER_H


struct ppc_debug_cpu_state {
	uint32	pc;			// srr0
	uint32	msr;		// srr1
	uint32	lr;
	uint32	ctr;
	uint32	cr;
	uint32	xer;
	uint32	fpscr;
	uint32	r0;
	uint32	r1;
	uint32	r2;
	uint32	r3;
	uint32	r4;
	uint32	r5;
	uint32	r6;
	uint32	r7;
	uint32	r8;
	uint32	r9;
	uint32	r10;
	uint32	r11;
	uint32	r12;
	uint32	r13;
	uint32	r14;
	uint32	r15;
	uint32	r16;
	uint32	r17;
	uint32	r18;
	uint32	r19;
	uint32	r20;
	uint32	r21;
	uint32	r22;
	uint32	r23;
	uint32	r24;
	uint32	r25;
	uint32	r26;
	uint32	r27;
	uint32	r28;
	uint32	r29;
	uint32	r30;
	uint32	r31;
} __attribute__((aligned(8)));


#endif	// _ARCH_PPC_DEBUGGER_H
