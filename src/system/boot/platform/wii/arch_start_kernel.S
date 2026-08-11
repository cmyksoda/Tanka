/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include <asm_defs.h>


#define MSR_EE	(1 << 15)
#define MSR_FP	(1 << 13)
#define MSR_ME	(1 << 12)
#define MSR_IR	(1 << 5)
#define MSR_DR	(1 << 4)

#define KERNEL_MSR	(MSR_FP | MSR_ME | MSR_IR | MSR_DR)

// IBAT0U..DBAT3L and, on the Broadway, the second set behind HID4[SBE]
#define FIRST_BAT_SPR		528
#define FIRST_HIGH_BAT_SPR	560


/*	status_t arch_start_kernel(struct kernel_args *kernelArgs,
		addr_t kernelEntry, addr_t kernelStackTop, uint32 sdr1);

	r3	- kernelArgs, a kernel address
	r4	- kernelEntry
	r5	- kernelStackTop
	r6	- SDR1 for the page table the loader built

	Does not return: the loader's own code and stack are unmapped on the way.
*/
FUNCTION(arch_start_kernel):
	// nothing may interrupt us between here and the kernel
	mfmsr	%r0
	rlwinm	%r0, %r0, 0, 17, 15
	mtmsr	%r0
	isync

	// drop to real mode - the loader runs out of libogc's cached MEM1 window,
	// so clearing the top two address bits gives the same code its physical
	// address, the trick libogc's own PPCEarlyInit uses
	lis		%r7, .Lreal_mode@h
	ori		%r7, %r7, .Lreal_mode@l
	clrlwi	%r7, %r7, 2
	mtsrr0	%r7
	mfmsr	%r7
	rlwinm	%r7, %r7, 0, 28, 25
	mtsrr1	%r7
	sync
	rfi

.Lreal_mode:
	// r1 still points into the loader's stack, which real mode cannot reach:
	// no memory access at all from here until the kernel takes over

	// libogc's BATs would shadow the page table for everything they cover
	li		%r0, 0
	.set	bat, FIRST_BAT_SPR
	.rept	16
	mtspr	bat, %r0
	.set	bat, bat + 1
	.endr
	.set	bat, FIRST_HIGH_BAT_SPR
	.rept	16
	mtspr	bat, %r0
	.set	bat, bat + 1
	.endr
	isync

	// install the kernel's page table
	sync
	mtsdr1	%r6
	sync

	// VSID == segment number, which is what the loader tagged its PTEs with
	li		%r7, 0
	li		%r8, 0
.Lnext_segment:
	mtsrin	%r7, %r8
	addi	%r7, %r7, 1
	addis	%r8, %r8, 0x1000
	cmpwi	%r7, 16
	blt		.Lnext_segment
	isync

	// 256 pages covers every congruence class of the Broadway's TLBs
	li		%r7, 256
	mtctr	%r7
	li		%r7, 0
.Lnext_tlb_entry:
	tlbie	%r7
	addi	%r7, %r7, 0x1000
	bdnz	.Lnext_tlb_entry
	sync
	tlbsync
	sync

	// back to translated mode, but into the loader, which kept its addresses:
	// the console has to answer here before any of this is the kernel's fault
	lis		%r7, .Ltranslated@h
	ori		%r7, %r7, .Ltranslated@l
	mtsrr0	%r7
	lis		%r7, KERNEL_MSR@h
	ori		%r7, %r7, KERNEL_MSR@l
	mtsrr1	%r7
	sync
	rfi

.Ltranslated:
	// r1 still points at the loader's stack, mapped where it always was
	mr		%r29, %r3
	mr		%r30, %r4
	mr		%r31, %r5
	bl		wii_mmu_translation_on

	// r3 is kernelArgs; the kernel wants the CPU number in r4. Its stack's
	// back chain was zeroed by the loader before the switch.
	mr		%r3, %r29
	li		%r4, 0
	mtctr	%r30
	subi	%r1, %r31, 16
	li		%r0, 0
	mtlr	%r0
	bctr
FUNCTION_END(arch_start_kernel)
