/*
** Copyright 2026, Tanka Contributors. All rights reserved.
** Distributed under the terms of the MIT License.
*/
#ifndef KERNEL_BOOT_PLATFORM_WII_KERNEL_ARGS_H
#define KERNEL_BOOT_PLATFORM_WII_KERNEL_ARGS_H


#ifndef KERNEL_BOOT_KERNEL_ARGS_H
#	error This file is included from <boot/kernel_args.h> only
#endif

#define SMP_MAX_CPUS 1
	// Broadway is a single-core 750CL

// MEM1, MEM2 and whatever the loader carves out of them.
#define MAX_PHYSICAL_MEMORY_RANGE 8
#define MAX_PHYSICAL_ALLOCATED_RANGE 32
#define MAX_VIRTUAL_ALLOCATED_RANGE 32


typedef struct {
	// Flattened device tree pointer. The Wii has no firmware-provided tree;
	// the field exists only because the shared ppc kernel code also builds
	// against the u-boot platform, which does.
	void	*fdt;
} platform_kernel_args;

#endif	/* KERNEL_BOOT_PLATFORM_WII_KERNEL_ARGS_H */
