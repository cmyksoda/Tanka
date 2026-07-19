/*
 * Copyright 2005, Ingo Weinhold <bonefish@cs.tu-berlin.de>.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include <boot/platform/openfirmware/platform_arch.h>

#include <stdio.h>

#include <KernelExport.h>

#include <boot/kernel_args.h>
#include <boot/stage2.h>
#include <kernel.h>
#include <platform/openfirmware/devices.h>
#include <platform/openfirmware/openfirmware.h>

#define TRACE_CPU
#ifdef TRACE_CPU
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


status_t
boot_arch_cpu_init(void)
{
	int32 busFrequency = 0;

	int root = of_finddevice("/");
	if (root == OF_FAILED) {
		printf("boot_arch_cpu_init(): Failed to open \"/\"!\n");
		return B_ERROR;
	}

	of_getprop(root, "clock-frequency", &busFrequency, 4);
		// we might find it in /cpus instead

	// iterate through the "/cpus" node to find all CPUs
	int cpus = of_finddevice("/cpus");
	if (cpus == OF_FAILED) {
		printf("boot_arch_cpu_init(): Failed to open \"/cpus\"!\n");
		return B_ERROR;
	}

	char cpuPath[256];
	intptr_t cookie = 0;
	int cpuCount = 0;
	while (of_get_next_device(&cookie, cpus, "cpu", cpuPath,
			sizeof(cpuPath)) == B_OK) {
		TRACE(("found CPU: %s\n", cpuPath));

		// For the first CPU get the frequencies of CPU, bus, and time base.
		// We assume they are the same for all CPUs.
		if (cpuCount == 0) {
			int cpu = of_finddevice(cpuPath);
			if (cpu == OF_FAILED) {
				printf("boot_arch_cpu_init: Failed get CPU device node!\n");
				return B_ERROR;
			}

			// TODO: Does encode-int really encode quadlet (32 bit numbers)
			// only?
			int32 clockFrequency;
			if (of_getprop(cpu, "clock-frequency", &clockFrequency, 4)
					== OF_FAILED) {
				printf("boot_arch_cpu_init: Failed to get CPU clock "
					"frequency!\n");
				return B_ERROR;
			}
			if (busFrequency == 0
				&& of_getprop(cpu, "bus-frequency", &busFrequency, 4)
					== OF_FAILED) {
				printf("boot_arch_cpu_init: Failed to get bus clock "
					"frequency!\n");
				return B_ERROR;
			}
			int32 timeBaseFrequency;
			if (of_getprop(cpu, "timebase-frequency", &timeBaseFrequency, 4)
					== OF_FAILED) {
				printf("boot_arch_cpu_init: Failed to get time base "
					"frequency!\n");
				return B_ERROR;
			}

			gKernelArgs.arch_args.cpu_frequency = clockFrequency;
			gKernelArgs.arch_args.bus_frequency = busFrequency;
			gKernelArgs.arch_args.time_base_frequency = timeBaseFrequency;

			TRACE(("  CPU clock frequency: %" B_PRId32 "\n", clockFrequency));
			TRACE(("  bus clock frequency: %" B_PRId32 "\n", busFrequency));
			TRACE(("  time base frequency: %" B_PRId32 "\n", timeBaseFrequency));
		}

		cpuCount++;
	}

	if (cpuCount == 0) {
		printf("boot_arch_cpu_init(): Found no CPUs!\n");
		return B_ERROR;
	}

	gKernelArgs.num_cpus = cpuCount;

	// allocate the kernel stacks (the memory stuff is already initialized
	// at this point)
	//
	// NOTE: this used to request the exact address 0x80000000 (KERNEL_BASE)
	// for the stack allocation. That collides with the address the kernel
	// image itself needs to load at: whichever of the two allocations runs
	// first claims 0x80000000, silently pushing the other one to a later
	// address. For the kernel specifically, being loaded away from its
	// intended KERNEL_BASE breaks every position-dependent (.got2,
	// R_PPC_RELATIVE) reference baked in by the relocator, which relocates
	// assuming the kernel runs from wherever it actually got allocated -
	// while the kernel later unconditionally maps itself to run from
	// KERNEL_BASE via its own MMU setup, without redoing those relocations.
	// (Passing NULL instead doesn't help: arch_mmu_allocate() explicitly
	// defaults a NULL address to KERNEL_BASE too, so it's the exact same
	// request under a different spelling.) The stack has no fixed-address
	// requirement of its own, so just point it well above where the kernel
	// image could plausibly reach, out of the way entirely.
	addr_t stack = (addr_t)arch_mmu_allocate((void*)(0x80000000 + 0x10000000),
		cpuCount * (KERNEL_STACK_SIZE + KERNEL_STACK_GUARD_PAGES * B_PAGE_SIZE),
		B_READ_AREA | B_WRITE_AREA, false);
	if (!stack) {
		printf("boot_arch_cpu_init(): Failed to allocate kernel stack(s)!\n");
		return B_NO_MEMORY;
	}

	for (int i = 0; i < cpuCount; i++) {
		gKernelArgs.cpu_kstack[i].start = stack;
		gKernelArgs.cpu_kstack[i].size = KERNEL_STACK_SIZE
			+ KERNEL_STACK_GUARD_PAGES * B_PAGE_SIZE;
		stack += KERNEL_STACK_SIZE + KERNEL_STACK_GUARD_PAGES * B_PAGE_SIZE;
	}

	return B_OK;
}

