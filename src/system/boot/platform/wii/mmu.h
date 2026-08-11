/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef WII_MMU_H
#define WII_MMU_H


#include <SupportDefs.h>


struct kernel_args;


struct wii_address_mapping {
	addr_t	kernel;
	addr_t	physical;
	size_t	size;
	uint8	protection;
};


addr_t wii_physical_to_loader(addr_t physical);
addr_t wii_loader_to_physical(addr_t address);
addr_t wii_allocate_physical(size_t size, size_t alignment, bool preferMem1);
const wii_address_mapping* wii_address_mappings(uint32* _count);
status_t wii_mmu_prepare_handoff(kernel_args** _kernelArgs, uint32* _sdr1);


#endif	// WII_MMU_H
