/*
** Copyright 2003, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
** Distributed under the terms of the MIT License.
*/
#ifndef KERNEL_ARCH_PPC_KERNEL_ARGS_H
#define KERNEL_ARCH_PPC_KERNEL_ARGS_H

#ifndef KERNEL_BOOT_KERNEL_ARGS_H
#	error This file is included from <boot/kernel_args.h> only
#endif

#define _PACKED __attribute__((packed))

#define MAX_VIRTUAL_RANGES_TO_KEEP	32

// kernel args
typedef struct {
	// architecture specific
	uint64		cpu_frequency;
	uint64		bus_frequency;
	uint64		time_base_frequency;

	addr_range	page_table;		// virtual address and size of the page table
	addr_range	exception_handlers;
	addr_range	framebuffer;		// maps where the framebuffer is located, in physical memory
	int 		screen_x, screen_y, screen_depth;

	// The virtual ranges we want to keep in the kernel. E.g. those belonging
	// to the Open Firmware.
	uint32		num_virtual_ranges_to_keep;
	addr_range	virtual_ranges_to_keep[MAX_VIRTUAL_RANGES_TO_KEEP];

	// platform type we booted from
	int			platform;

	// PCI host bridge, detected by the boot loader from Open Firmware.
	// pci_host_bridge_type: 0 = Grackle (MPC106), 1 = UniNorth.
	uint32		pci_host_bridge_type;
	uint64		pci_config_address;
	uint64		pci_config_data;
} arch_kernel_args;

#endif	/* KERNEL_ARCH_PPC_KERNEL_ARGS_H */
