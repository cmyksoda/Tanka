/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include <boot/platform.h>
#include <boot/addr_range.h>
#include <boot/kernel_args.h>
#include <boot/stage2.h>
#include <boot/stdio.h>
#include <arch/cpu.h>
#include <arch_cpu.h>
#include <arch_mmu.h>
#include <kernel.h>
#include <string.h>

#include <gccore.h>

#include "mmu.h"


extern "C" void* arch_mmu_allocate(void* virtualAddress, size_t size,
	uint8 protection, bool exactAddress);


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

// left to libogc: MEM1 for scanout buffers, MEM2 for IOS transfer buffers
#define LIBOGC_MEM1_RESERVE	(4 * 1024 * 1024)
#define LIBOGC_MEM2_RESERVE	(8 * 1024 * 1024)

// WIMGxxPP, the low byte of a PTE: 10 is read/write for either protection key
#define PAGE_READ_WRITE		0x02
// cache inhibited and guarded, the WIMG libogc's uncached BATs used
#define PAGE_DEVICE			0x2a

// Hollywood's registers, the only physical device range libogc touches
#define DEVICE_BASE		0x0c000000
#define DEVICE_SIZE		0x01800000

// a kernel window onto the PPC exception vectors at physical zero
#define EXCEPTION_HANDLERS_BASE	0xa0000000
#define EXCEPTION_HANDLERS_SIZE	0x4000


struct memory_pool {
	addr_t	next;
	addr_t	end;
};

static memory_pool sMem1Pool;
static memory_pool sMem2Pool;

static wii_address_mapping sMappings[128];
static uint32 sMappingCount;

static page_table_entry_group *sPageTable;
static uint32 sPageTableHashMask;


//! Loader addresses are libogc's BAT windows: MEM1 at 0x80000000, MEM2 at
//! 0x90000000 cached, 0xc0000000/0xd0000000 uncached.
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


static void
fill_page_table_entry(page_table_entry *entry, uint32 virtualSegmentID,
	addr_t virtualAddress, addr_t physicalAddress, uint8 mode,
	bool secondaryHash)
{
	((uint32 *)entry)[1] = ((physicalAddress / B_PAGE_SIZE) << 12) | mode;
	eieio();
		// the low word has to have landed before the entry becomes valid

	entry->virtual_segment_id = virtualSegmentID;
	entry->secondary_hash = secondaryHash;
	entry->abbr_page_index = (virtualAddress >> 22) & 0x3f;
	entry->valid = true;
}


//! The kernel assumes at most one page table entry per virtual address: its
//! lookup returns the first match and every unmap path clears exactly that one,
//! so a second entry for the same address outlives the unmap, sits in what the
//! kernel believes is free address space, and is later charged to whatever area
//! lands there. The MEM1/MEM2 windows below deliberately re-map everything
//! arch_mmu_allocate() already mapped, so drop any existing entry first. The
//! table is not live yet (translation still runs off libogc's BATs), so no TLB
//! invalidation is needed.
static void
remove_page_table_entries(addr_t virtualAddress)
{
	uint32 virtualSegmentID = virtualAddress >> 28;
	uint32 hash = page_table_entry::PrimaryHash(virtualSegmentID,
		(uint32)virtualAddress);
	uint32 abbrPageIndex = (virtualAddress >> 22) & 0x3f;

	for (int32 pass = 0; pass < 2; pass++) {
		page_table_entry_group *group = &sPageTable[(pass == 0
			? hash : page_table_entry::SecondaryHash(hash)) & sPageTableHashMask];

		for (int32 i = 0; i < 8; i++) {
			page_table_entry *entry = &group->entry[i];

			if (entry->valid
				&& entry->virtual_segment_id == virtualSegmentID
				&& entry->secondary_hash == (uint32)pass
				&& entry->abbr_page_index == abbrPageIndex) {
				entry->valid = false;
			}
		}
	}
}


//! The segment registers get VSID == segment number, so the VSID is implied.
static void
map_page(addr_t virtualAddress, addr_t physicalAddress, uint8 mode)
{
	uint32 virtualSegmentID = virtualAddress >> 28;

	remove_page_table_entries(virtualAddress);

	uint32 hash = page_table_entry::PrimaryHash(virtualSegmentID,
		(uint32)virtualAddress);
	page_table_entry_group *group = &sPageTable[hash & sPageTableHashMask];

	for (int32 i = 0; i < 8; i++) {
		if (group->entry[i].valid)
			continue;

		fill_page_table_entry(&group->entry[i], virtualSegmentID,
			virtualAddress, physicalAddress, mode, false);
		return;
	}

	group = &sPageTable[page_table_entry::SecondaryHash(hash)
		& sPageTableHashMask];

	for (int32 i = 0; i < 8; i++) {
		if (group->entry[i].valid)
			continue;

		fill_page_table_entry(&group->entry[i], virtualSegmentID,
			virtualAddress, physicalAddress, mode, true);
		return;
	}

	panic("mmu: no free page table entry for %p\n", (void *)virtualAddress);
}


static void
map_range(addr_t virtualAddress, addr_t physicalAddress, size_t size,
	uint8 mode)
{
	for (size_t offset = 0; offset < size; offset += B_PAGE_SIZE)
		map_page(virtualAddress + offset, physicalAddress + offset, mode);
}


//! Kept identical to the OpenFirmware loader's: RAM/32, so a busy desktop
//! does not overflow an individual PTEG, which the kernel panics on.
static size_t
suggested_page_table_size(size_t total)
{
	uint32 max = 23;
		// 2^23 == 8 MB

	while (max < 32) {
		if (total <= (1UL << max))
			break;

		max++;
	}

	return 1UL << (max - 5);
}


extern "C" status_t
boot_arch_mmu_init(void)
{
	insert_physical_memory_range(MEM1_BASE, MEM1_SIZE);
	insert_physical_memory_range(MEM2_BASE, MEM2_SIZE);

	// take the kernel's memory off the top of the arenas, libogc keeps the rest
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

	// everything outside the pools is libogc's, the loader's or IOS'
	insert_physical_allocated_range(MEM1_BASE, sMem1Pool.next - MEM1_BASE);
	insert_physical_allocated_range(sMem1Pool.end,
		MEM1_BASE + MEM1_SIZE - sMem1Pool.end);
	insert_physical_allocated_range(MEM2_BASE, sMem2Pool.next - MEM2_BASE);
	insert_physical_allocated_range(sMem2Pool.end,
		MEM2_BASE + MEM2_SIZE - sMem2Pool.end);

	dprintf("mmu: MEM1 pool %p - %p, MEM2 pool %p - %p\n",
		(void *)sMem1Pool.next, (void *)sMem1Pool.end,
		(void *)sMem2Pool.next, (void *)sMem2Pool.end);

	// the kernel gets its own stack; nothing else sets cpu_kstack up for us
	size_t stackSize = KERNEL_STACK_SIZE
		+ KERNEL_STACK_GUARD_PAGES * B_PAGE_SIZE;
	void *stack = arch_mmu_allocate(NULL, stackSize,
		B_READ_AREA | B_WRITE_AREA, false);
	if (stack == NULL)
		return B_NO_MEMORY;

	gKernelArgs.cpu_kstack[0].start = (addr_t)stack;
	gKernelArgs.cpu_kstack[0].size = stackSize;

	dprintf("mmu: kernel stack %p - %p\n", stack,
		(void *)((addr_t)stack + stackSize));

	return B_OK;
}


/*!	Allocates \a size bytes and gives them the same kernel address the loader
	sees them at, which is the invariant the rest of the loader is built on -
	arch_elf.cpp writes relocations straight through kernel addresses, so an
	image placed anywhere else has its PLT and GOT patched into thin air.

	\a virtualAddress is therefore only read as "this is the kernel image",
	which earns it MEM1; its value is deliberately ignored, and the image is
	relocatable (ET_DYN) precisely so it can be placed elsewhere.
*/
extern "C" void*
arch_mmu_allocate(void* virtualAddress, size_t size, uint8 protection,
	bool exactAddress)
{
	if (exactAddress) {
		dprintf("mmu: cannot place anything at a fixed address (%p)\n",
			virtualAddress);
		return NULL;
	}

	size = ROUNDUP(size, B_PAGE_SIZE);

	addr_t physical = wii_allocate_physical(size, B_PAGE_SIZE,
		virtualAddress != NULL);
	if (physical == 0) {
		dprintf("mmu: out of memory allocating %" B_PRIuSIZE " bytes\n", size);
		return NULL;
	}

	addr_t address = wii_physical_to_loader(physical);

	if (add_mapping(address, physical, size, protection) != B_OK)
		return NULL;

	if (insert_virtual_allocated_range(address, size) != B_OK) {
		dprintf("mmu: out of virtual allocated ranges\n");
		return NULL;
	}

	TRACE("mmu: alloc %" B_PRIuSIZE " bytes at %p (physical %p)\n", size,
		(void *)address, (void *)physical);

	return (void *)address;
}


/*!	Builds the page table the kernel will inherit, maps everything the loader
	promised it, and copies \a gKernelArgs somewhere that survives the switch.

	Returns the kernel's address for the copy in \a _kernelArgs and the SDR1
	value \a arch_start_kernel has to install in \a _sdr1.
*/
status_t
wii_mmu_prepare_handoff(kernel_args** _kernelArgs, uint32* _sdr1)
{
	// the loader's own BSS is gone once the BATs are, so kernel_args moves
	kernel_args* args = (kernel_args*)kernel_args_malloc(sizeof(kernel_args),
		sizeof(void*));
	if (args == NULL) {
		dprintf("mmu: no room for the kernel_args copy\n");
		return B_NO_MEMORY;
	}

	size_t tableSize = suggested_page_table_size(MEM1_SIZE + MEM2_SIZE);
	addr_t tablePhysical = wii_allocate_physical(tableSize, tableSize, false);
	if (tablePhysical == 0) {
		dprintf("mmu: could not allocate a %" B_PRIuSIZE " byte page table\n",
			tableSize);
		return B_NO_MEMORY;
	}

	addr_t tableAddress = wii_physical_to_loader(tablePhysical);
	sPageTable = (page_table_entry_group*)tableAddress;
	sPageTableHashMask = tableSize / sizeof(page_table_entry_group) - 1;
	memset(sPageTable, 0, tableSize);

	// the kernel reaches its own page table through the address space, so the
	// table has to be mapped like any other allocation
	if (add_mapping(tableAddress, tablePhysical, tableSize,
			B_READ_AREA | B_WRITE_AREA) != B_OK
		|| insert_virtual_allocated_range(tableAddress, tableSize) != B_OK) {
		return B_NO_MEMORY;
	}

	gKernelArgs.arch_args.page_table.start = tableAddress;
	gKernelArgs.arch_args.page_table.size = tableSize;

	// libogc and the loader keep their addresses across the switch, the way
	// the OpenFirmware loader keeps the firmware's: the kernel takes
	// exceptions long before it installs its own vectors, and the vectors at
	// physical zero branch straight back into libogc's handlers. This has to
	// go in before the alias below, so address lookups find it first.
	if (add_mapping(wii_physical_to_loader(MEM1_BASE), MEM1_BASE,
			sMem1Pool.next - MEM1_BASE, B_READ_AREA | B_WRITE_AREA) != B_OK
		|| insert_virtual_allocated_range(wii_physical_to_loader(MEM1_BASE),
			sMem1Pool.next - MEM1_BASE) != B_OK
		|| add_mapping(wii_physical_to_loader(MEM2_BASE), MEM2_BASE,
			sMem2Pool.next - MEM2_BASE, B_READ_AREA | B_WRITE_AREA) != B_OK
		|| insert_virtual_allocated_range(wii_physical_to_loader(MEM2_BASE),
			sMem2Pool.next - MEM2_BASE) != B_OK) {
		return B_NO_MEMORY;
	}

	// a second window onto the hardware vectors at physical zero, which the
	// kernel overwrites with its own once the VM is up
	if (add_mapping(EXCEPTION_HANDLERS_BASE, 0, EXCEPTION_HANDLERS_SIZE,
			B_READ_AREA | B_WRITE_AREA) != B_OK
		|| insert_virtual_allocated_range(EXCEPTION_HANDLERS_BASE,
			EXCEPTION_HANDLERS_SIZE) != B_OK) {
		return B_NO_MEMORY;
	}

	gKernelArgs.arch_args.exception_handlers.start = EXCEPTION_HANDLERS_BASE;
	gKernelArgs.arch_args.exception_handlers.size = EXCEPTION_HANDLERS_SIZE;

	dprintf("mmu: page table %p (physical %p), %" B_PRIuSIZE " bytes\n",
		(void *)tableAddress, (void *)tablePhysical, tableSize);

	// everything arch_mmu_allocate() handed out, plus the two entries above
	uint32 mappingCount = sMappingCount;
	for (uint32 i = 0; i < mappingCount; i++) {
		// protections are left wide open; the kernel's VM sets the real ones
		map_range(sMappings[i].kernel, sMappings[i].physical,
			sMappings[i].size, PAGE_READ_WRITE);

		TRACE("mmu: mapped %p -> %p, %" B_PRIuSIZE " bytes\n",
			(void *)sMappings[i].kernel, (void *)sMappings[i].physical,
			sMappings[i].size);
	}

	// the uncached aliases and the register block arch_mmu_map_device() hands
	// out, which were BAT windows until a moment ago
	map_range(0xc0000000, MEM1_BASE, MEM1_SIZE, PAGE_DEVICE);
	map_range(0xc0000000 + DEVICE_BASE, DEVICE_BASE, DEVICE_SIZE, PAGE_DEVICE);
	map_range(0xd0000000, MEM2_BASE, MEM2_SIZE, PAGE_DEVICE);

	if (insert_virtual_allocated_range(0xc0000000, MEM1_SIZE) != B_OK
		|| insert_virtual_allocated_range(0xc0000000 + DEVICE_BASE,
			DEVICE_SIZE) != B_OK
		|| insert_virtual_allocated_range(0xd0000000, MEM2_SIZE) != B_OK) {
		return B_NO_MEMORY;
	}

	memcpy((void *)args, (const void *)&gKernelArgs, sizeof(kernel_args));

	// the kernel starts executing out of pages the loader only ever wrote to
	for (uint32 i = 0; i < mappingCount; i++) {
		void* address = (void *)wii_physical_to_loader(sMappings[i].physical);
		DCFlushRange(address, sMappings[i].size);
		ICInvalidateRange(address, sMappings[i].size);
	}

	addr_t argsAddress;
	platform_bootloader_address_to_kernel_address(args, &argsAddress);

	*_kernelArgs = (kernel_args*)argsAddress;
	*_sdr1 = (tablePhysical & 0xffff0000) | (((tableSize - 1) >> 16) & 0x1ff);

	return B_OK;
}


//! Called from arch_start_kernel() with the kernel's page table already live.
extern "C" void
wii_mmu_translation_on(void)
{
	dprintf("mmu: translation is on, the loader is still here\n");
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
