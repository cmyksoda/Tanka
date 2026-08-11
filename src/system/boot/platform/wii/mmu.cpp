/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include <boot/platform.h>
#include <boot/addr_range.h>
#include <boot/stage2.h>
#include <boot/stdio.h>
#include <arch/cpu.h>
#include <arch_mmu.h>
#include <kernel.h>
#include <string.h>

#include <gccore.h>

#include "mmu.h"


//#define TRACE_MMU
#ifdef TRACE_MMU
#	define TRACE(x...) dprintf(x)
#else
#	define TRACE(x...) ;
#endif


#define MEM1_BASE	0x00000000
#define MEM1_SIZE	(24 * 1024 * 1024)
#define MEM2_BASE	0x10000000
#define MEM2_SIZE	(64 * 1024 * 1024)

// What is left to libogc when the loader takes the rest of the arenas for the
// kernel: MEM1 has to hold the scanout buffers, MEM2 the IOS transfer buffers
// the SD and USB drivers allocate while the loader is still running.
#define LIBOGC_MEM1_RESERVE	(4 * 1024 * 1024)
#define LIBOGC_MEM2_RESERVE	(8 * 1024 * 1024)


struct memory_pool {
	addr_t	next;
	addr_t	end;
};

static memory_pool sMem1Pool;
static memory_pool sMem2Pool;

static wii_address_mapping sMappings[128];
static uint32 sMappingCount;


/*!	The loader runs under the BATs libogc's crt0 installed: MEM1 is mapped
	cached at 0x80000000 and uncached at 0xc0000000, MEM2 cached at 0x90000000
	and uncached at 0xd0000000. Everything the loader touches goes through
	those, the kernel's own addresses only become valid at the hand-off.
*/
addr_t
wii_physical_to_loader(addr_t physical)
{
	if (physical < MEM1_SIZE)
		return 0x80000000 + physical;

	return 0x90000000 + (physical - MEM2_BASE);
}


addr_t
wii_loader_to_physical(addr_t address)
{
	if (address >= 0x90000000)
		return MEM2_BASE + (address - 0x90000000);

	return address - 0x80000000;
}


const wii_address_mapping*
wii_address_mappings(uint32* _count)
{
	*_count = sMappingCount;
	return sMappings;
}


static addr_t
allocate_physical(memory_pool& pool, size_t size, size_t alignment)
{
	addr_t address = ROUNDUP(pool.next, alignment);
	if (size > pool.end || address > pool.end - size)
		return 0;

	pool.next = address + size;

	if (insert_physical_allocated_range(address, size) != B_OK)
		panic("out of physical allocated ranges\n");

	return address;
}


addr_t
wii_allocate_physical(size_t size, size_t alignment, bool preferMem1)
{
	memory_pool& first = preferMem1 ? sMem1Pool : sMem2Pool;
	memory_pool& second = preferMem1 ? sMem2Pool : sMem1Pool;

	addr_t physical = allocate_physical(first, size, alignment);
	if (physical == 0)
		physical = allocate_physical(second, size, alignment);

	return physical;
}


static status_t
add_mapping(addr_t kernelAddress, addr_t physical, size_t size,
	uint8 protection)
{
	if (sMappingCount >= B_COUNT_OF(sMappings)) {
		dprintf("mmu: out of mapping slots\n");
		return B_NO_MEMORY;
	}

	wii_address_mapping& mapping = sMappings[sMappingCount++];
	mapping.kernel = kernelAddress;
	mapping.physical = physical;
	mapping.size = size;
	mapping.protection = protection;

	return B_OK;
}


extern "C" status_t
boot_arch_mmu_init(void)
{
	insert_physical_memory_range(MEM1_BASE, MEM1_SIZE);
	insert_physical_memory_range(MEM2_BASE, MEM2_SIZE);

	// Take what the kernel gets off the top of libogc's arenas and hand the
	// shrunk arenas back, so libogc keeps allocating below us.
	addr_t arena1Lo = (addr_t)SYS_GetArena1Lo();
	addr_t arena1Hi = (addr_t)SYS_GetArena1Hi();
	addr_t arena2Lo = (addr_t)SYS_GetArena2Lo();
	addr_t arena2Hi = (addr_t)SYS_GetArena2Hi();

	if (arena1Hi <= arena1Lo + LIBOGC_MEM1_RESERVE
		|| arena2Hi <= arena2Lo + LIBOGC_MEM2_RESERVE) {
		dprintf("mmu: arenas too small (MEM1 %p-%p, MEM2 %p-%p)\n",
			(void *)arena1Lo, (void *)arena1Hi, (void *)arena2Lo,
			(void *)arena2Hi);
		return B_NO_MEMORY;
	}

	addr_t mem1PoolStart = ROUNDUP(arena1Lo + LIBOGC_MEM1_RESERVE, B_PAGE_SIZE);
	addr_t mem2PoolStart = ROUNDUP(arena2Lo + LIBOGC_MEM2_RESERVE, B_PAGE_SIZE);

	SYS_SetArena1Hi((void *)mem1PoolStart);
	SYS_SetArena2Hi((void *)mem2PoolStart);

	sMem1Pool.next = wii_loader_to_physical(mem1PoolStart);
	sMem1Pool.end = ROUNDDOWN(wii_loader_to_physical(arena1Hi), B_PAGE_SIZE);
	sMem2Pool.next = wii_loader_to_physical(mem2PoolStart);
	sMem2Pool.end = ROUNDDOWN(wii_loader_to_physical(arena2Hi), B_PAGE_SIZE);

	// Everything below the pools belongs to libogc, the loader image or IOS,
	// and everything above MEM2's arena is IOS' as well. None of it is free
	// memory as far as the kernel is concerned.
	insert_physical_allocated_range(MEM1_BASE, sMem1Pool.next - MEM1_BASE);
	insert_physical_allocated_range(sMem1Pool.end,
		MEM1_BASE + MEM1_SIZE - sMem1Pool.end);
	insert_physical_allocated_range(MEM2_BASE, sMem2Pool.next - MEM2_BASE);
	insert_physical_allocated_range(sMem2Pool.end,
		MEM2_BASE + MEM2_SIZE - sMem2Pool.end);

	dprintf("mmu: MEM1 pool %p - %p, MEM2 pool %p - %p\n",
		(void *)sMem1Pool.next, (void *)sMem1Pool.end,
		(void *)sMem2Pool.next, (void *)sMem2Pool.end);

	return B_OK;
}


extern "C" void*
arch_mmu_allocate(void* virtualAddress, size_t size, uint8 protection,
	bool exactAddress)
{
	size = ROUNDUP(size, B_PAGE_SIZE);

	// A requested address is a kernel address - the kernel image has to end up
	// at the address it was linked for. Everything else is anonymous memory,
	// which the kernel simply keeps seeing where the loader sees it.
	addr_t kernelAddress = (addr_t)virtualAddress;
	bool wantsExact = kernelAddress != 0;

	if (wantsExact && is_address_range_covered(
			gKernelArgs.virtual_allocated_range,
			gKernelArgs.num_virtual_allocated_ranges, kernelAddress, 1)) {
		if (exactAddress) {
			dprintf("mmu: %p is not available\n", virtualAddress);
			return NULL;
		}
		wantsExact = false;
	}

	addr_t physical = wii_allocate_physical(size, B_PAGE_SIZE, wantsExact);
	if (physical == 0) {
		dprintf("mmu: out of memory allocating %" B_PRIuSIZE " bytes\n", size);
		return NULL;
	}

	addr_t loaderAddress = wii_physical_to_loader(physical);
	if (!wantsExact)
		kernelAddress = loaderAddress;

	if (add_mapping(kernelAddress, physical, size, protection) != B_OK)
		return NULL;

	if (insert_virtual_allocated_range(kernelAddress, size) != B_OK) {
		dprintf("mmu: out of virtual allocated ranges\n");
		return NULL;
	}

	TRACE("mmu: alloc %" B_PRIuSIZE " bytes: kernel %p, physical %p, "
		"loader %p\n", size, (void *)kernelAddress, (void *)physical,
		(void *)loaderAddress);

	return (void *)loaderAddress;
}


extern "C" status_t
arch_mmu_free(void* address, size_t size)
{
	return B_OK;
}


extern "C" void*
arch_mmu_map_device(void* physicalAddress, size_t size)
{
	addr_t physical = (addr_t)physicalAddress;

	// the uncached, guarded BAT windows libogc set up
	if (physical < 0x10000000)
		return (void *)(0xc0000000 + physical);

	return (void *)(0xd0000000 + (physical - MEM2_BASE));
}


extern "C" status_t
platform_allocate_region(void **_address, size_t size, uint8 protection)
{
	if (size == 0)
		return B_BAD_VALUE;

	void *address = arch_mmu_allocate(*_address, size, protection, false);
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
	addr_t loaderAddress = (addr_t)address;

	for (uint32 i = 0; i < sMappingCount; i++) {
		addr_t base = wii_physical_to_loader(sMappings[i].physical);
		if (loaderAddress >= base && loaderAddress < base + sMappings[i].size) {
			*_result = sMappings[i].kernel + (loaderAddress - base);
			return B_OK;
		}
	}

	*_result = loaderAddress;
	return B_OK;
}


extern "C" status_t
platform_kernel_address_to_bootloader_address(addr_t address, void **_result)
{
	for (uint32 i = 0; i < sMappingCount; i++) {
		if (address >= sMappings[i].kernel
			&& address < sMappings[i].kernel + sMappings[i].size) {
			*_result = (void *)(wii_physical_to_loader(sMappings[i].physical)
				+ (address - sMappings[i].kernel));
			return B_OK;
		}
	}

	*_result = (void *)address;
	return B_OK;
}


extern "C" status_t
platform_free_region(void *address, size_t size)
{
	return B_OK;
}


extern "C" void
platform_free_heap_region(void *_base, size_t size)
{
}
