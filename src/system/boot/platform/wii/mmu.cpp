/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Antigravity
 */

#include <boot/platform.h>
#include <boot/stage2.h>
#include <boot/stdio.h>
#include <arch/cpu.h>
#include <arch_mmu.h>
#include <string.h>


// 88MB Total RAM: 24MB MEM1, 64MB MEM2
#define WII_MEM1_BASE 0x00000000
#define WII_MEM1_SIZE (24 * 1024 * 1024)

#define WII_MEM2_BASE 0x10000000
#define WII_MEM2_SIZE (64 * 1024 * 1024)


static addr_t sKernelPageTableStart = 0;
static size_t sKernelPageTableSize = 0;


extern "C" status_t
boot_arch_mmu_init(void)
{
	dprintf("boot_arch_mmu_init()\n");

	// Populate kernel_args with physical memory boundaries
	gKernelArgs.num_physical_memory_ranges = 2;

	// MEM1
	gKernelArgs.physical_memory_range[0].start = WII_MEM1_BASE;
	gKernelArgs.physical_memory_range[0].size = WII_MEM1_SIZE;

	// MEM2
	gKernelArgs.physical_memory_range[1].start = WII_MEM2_BASE;
	gKernelArgs.physical_memory_range[1].size = WII_MEM2_SIZE;

	gKernelArgs.num_physical_allocated_ranges = 0;
	gKernelArgs.num_virtual_allocated_ranges = 0;

	// Note: We leave the BAT configuration to the libogc runtime for the bootloader.
	// We will allocate the page tables for the Haiku kernel to use.

	return B_OK;
}


extern "C" void
boot_arch_mmu_allocate_kernel_page_tables(void)
{
	dprintf("boot_arch_mmu_allocate_kernel_page_tables()\n");

	// For 88MB RAM, the PowerPC HTAB minimal size rule (RAM / 128) yields small tables.
	// We allocate 1MB of page table space which is more than enough for a 32-bit OS in 88MB.
	sKernelPageTableSize = 1024 * 1024;

	// Find free space in MEM1 for the page table, alignment must be equal to its size.
	// The bootloader is currently running inside MEM1. We should ideally allocate this 
	// using Haiku's standard bootloader physical allocator, but here we place it at 
	// a safe offset (e.g., end of MEM1 or start of MEM2). Let's use MEM2 for the HTAB 
	// to save precious MEM1 space (MEM1 is faster and used for DMA, but MEM2 is fine for HTAB).
	
	sKernelPageTableStart = WII_MEM2_BASE; // At 0x10000000

	// Mark it as allocated so the kernel doesn't stomp on its own page tables
	gKernelArgs.physical_allocated_range[gKernelArgs.num_physical_allocated_ranges].start = sKernelPageTableStart;
	gKernelArgs.physical_allocated_range[gKernelArgs.num_physical_allocated_ranges].size = sKernelPageTableSize;
	gKernelArgs.num_physical_allocated_ranges++;
	
	// Clear the HTAB
	memset((void*)(sKernelPageTableStart + 0x90000000), 0, sKernelPageTableSize); // libogc caches MEM2 at 0x90000000
}


extern "C" addr_t
boot_arch_mmu_get_kernel_page_tables_start(void)
{
	return sKernelPageTableStart;
}


extern "C" size_t
boot_arch_mmu_get_kernel_page_tables_size(void)
{
	return sKernelPageTableSize;
}


extern "C" void*
arch_mmu_allocate(void* virtualAddress, size_t size, uint8 protection,
	bool exactAddress)
{
	// TODO: Support dynamic allocation during bootloader phase if required by Haiku kernel loader.
	return NULL;
}


extern "C" status_t
arch_mmu_free(void* address, size_t size)
{
	return B_OK;
}


extern "C" void*
arch_mmu_map_device(void* physicalAddress, size_t size)
{
	// Assume libogc already maps HW registers via BAT (0xCD000000 or similar)
	return physicalAddress;
}


extern "C" status_t
platform_allocate_region(void **_address, size_t size, uint8 protection)
{
	if (size == 0)
		return B_BAD_VALUE;

	void *address = arch_mmu_allocate(*_address, size, protection,
		false);
	if (address == NULL)
		return B_NO_MEMORY;

	*_address = address;
	return B_OK;
}


extern "C" ssize_t
platform_allocate_heap_region(size_t size, void **_base)
{
	*_base = NULL;
	status_t error = platform_allocate_region(_base, size,
		B_READ_AREA | B_WRITE_AREA);
	if (error != B_OK)
		return error;

	return size;
}


extern "C" status_t
platform_bootloader_address_to_kernel_address(void *address, addr_t *_result)
{
	*_result = (addr_t)address;
	return B_OK;
}


extern "C" status_t
platform_kernel_address_to_bootloader_address(addr_t address, void **_result)
{
	*_result = (void*)address;
	return B_OK;
}


extern "C" status_t
platform_free_region(void *address, size_t size)
{
	return arch_mmu_free(address, size);
}

extern "C" void
platform_free_heap_region(void *_base, size_t size)
{
	// no-op for now, Wii bootloader doesn't have complex heap region freeing implemented yet
}
