/*
 * Copyright 2008-2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Copyright 2002-2007, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001-2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */

/*	(bonefish) Some explanatory words on how address translation is implemented
	for the 32 bit PPC architecture.

	I use the address type nomenclature as used in the PPC architecture
	specs, i.e.
	- effective address: An address as used by program instructions, i.e.
	  that's what elsewhere (e.g. in the VM implementation) is called
	  virtual address.
	- virtual address: An intermediate address computed from the effective
	  address via the segment registers.
	- physical address: An address referring to physical storage.

	The hardware translates an effective address to a physical address using
	either of two mechanisms: 1) Block Address Translation (BAT) or
	2) segment + page translation. The first mechanism does this directly
	using two sets (for data/instructions) of special purpose registers.
	The latter mechanism is of more relevance here, though:

	effective address (32 bit):	     [ 0 ESID  3 | 4  PIX 19 | 20 Byte 31 ]
								           |           |            |
							     (segment registers)   |            |
									       |           |            |
	virtual address (52 bit):   [ 0      VSID 23 | 24 PIX 39 | 40 Byte 51 ]
	                            [ 0             VPN       39 | 40 Byte 51 ]
								                 |                  |
										   (page table)             |
											     |                  |
	physical address (32 bit):       [ 0        PPN       19 | 20 Byte 31 ]


	ESID: Effective Segment ID
	VSID: Virtual Segment ID
	PIX:  Page Index
	VPN:  Virtual Page Number
	PPN:  Physical Page Number


	Unlike on x86 we can't just switch the context to another team by just
	setting a register to another page directory, since we only have one
	page table containing both kernel and user address mappings. Instead we
	map the effective address space of kernel and *all* teams
	non-intersectingly into the virtual address space (which fortunately is
	20 bits wider), and use the segment registers to select the section of
	the virtual address space for the current team. Half of the 16 segment
	registers (8 - 15) map the kernel addresses, so they remain unchanged.

	The range of the virtual address space a team's effective address space
	is mapped to is defined by its PPCVMTranslationMap::fVSIDBase,
	which is the first of the 8 successive VSID values used for the team.

	Which fVSIDBase values are already taken is defined by the set bits in
	the bitmap sVSIDBaseBitmap.


	TODO:
	* If we want to continue to use the OF services, we would need to add
	  its address mappings to the kernel space. Unfortunately some stuff
	  (especially RAM) is mapped in an address range without the kernel
	  address space. We probably need to map those into each team's address
	  space as kernel read/write areas.
	* The current locking scheme is insufficient. The page table is a resource
	  shared by all teams. We need to synchronize access to it. Probably via a
	  spinlock.
 */

#include "paging/classic/PPCVMTranslationMapClassic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arch/cpu.h>
#include <arch_mmu.h>
#include <interrupts.h>
#include <thread.h>
#include <slab/Slab.h>
#include <smp.h>
#include <util/AutoLock.h>
#include <util/ThreadAutoLock.h>
#include <util/queue.h>
#include <vm/vm_page.h>
#include <vm/vm_priv.h>
#include <vm/VMAddressSpace.h>
#include <vm/VMCache.h>

#include "paging/classic/PPCPagingMethodClassic.h"
#include "paging/classic/PPCPagingStructuresClassic.h"
#include "generic_vm_physical_page_mapper.h"
#include "generic_vm_physical_page_ops.h"
#include "GenericVMPhysicalPageMapper.h"


//#define TRACE_PPC_VM_TRANSLATION_MAP_CLASSIC
#ifdef TRACE_PPC_VM_TRANSLATION_MAP_CLASSIC
#	define TRACE(x...) dprintf(x)
#else
#	define TRACE(x...) ;
#endif


// The VSID is a 24 bit number. The lower three bits are defined by the
// (effective) segment number, which leaves us with a 21 bit space of
// VSID bases (= 2 * 1024 * 1024).
#define MAX_VSID_BASES (B_PAGE_SIZE * 8)
static uint32 sVSIDBaseBitmap[MAX_VSID_BASES / (sizeof(uint32) * 8)];
static spinlock sVSIDBaseBitmapLock;
// The VSIDs the kernel's 16 segment registers actually hold, captured for the
// eviction code below. These are *not* a contiguous range starting at the
// kernel map's fVSIDBase: the boot loader preserves whatever arbitrary,
// non-linear VSID Open Firmware assigned to each segment (see
// vsid_for_address() below), so recognising a kernel page table entry means
// testing membership in this set, not a range check against segment 0's value.
static uint32 sKernelVSIDs[16];
static bool sKernelVSIDsKnown = false;


/*!	Returns whether \a vsid belongs to the kernel address space.

	Only meaningful once the kernel translation map has been initialised;
	before that everything mapped is the kernel's, so the conservative answer
	is also the correct one.
*/
static inline bool
is_kernel_vsid(uint32 vsid)
{
	if (!sKernelVSIDsKnown)
		return true;

	for (int i = 0; i < 16; i++) {
		if (sKernelVSIDs[i] == vsid)
			return true;
	}

	return false;
}

#define VSID_BASE_SHIFT 3
#define VADDR_TO_VSID(vsidBase, vaddr) (vsidBase + ((vaddr) >> 28))


/*!	Returns the virtual segment ID (VSID) the hardware actually uses for
	\a virtualAddress in this translation map.

	The VADDR_TO_VSID(fVSIDBase, ...) formula assumes VSID == fVSIDBase +
	segment index, i.e. that VSIDs are linear in the segment number. That
	holds for user address spaces (ChangeASID() programs segment registers
	0-7 to exactly fVSIDBase + i), but NOT for the kernel: the boot loader
	preserves whatever arbitrary, non-linear VSIDs Open Firmware assigned to
	each segment (see the loader's mmu.cpp), and every kernel PTE is tagged
	with those real values - both the loader's own and MapEarly()'s, which
	read the live segment register (get_sr()). Using the linear formula for a
	kernel address therefore computes a VSID the hardware never uses, so the
	hash lookup misses (LookupPageTableEntry() returns NULL, Query() reports
	physical 0) and any PTE written via Map() is tagged so the CPU's own MMU
	can never find it. Kernel segment registers (8-15) are fixed and always
	loaded regardless of the current team, so reading the real VSID directly
	is always safe here; only kernel addresses need this - user addresses
	keep the self-consistent formula.
*/
static inline uint32
vsid_for_address(uint32 vsidBase, addr_t virtualAddress)
{
	if (IS_KERNEL_ADDRESS(virtualAddress))
		return get_sr((void*)virtualAddress) & 0xffffff;
	return VADDR_TO_VSID(vsidBase, virtualAddress);
}


// #pragma mark - wired-area page table entry forensics


// Why a wired area's page table entry could not be trusted.
enum {
	PPC_PTE_TRUSTED = 0,
	PPC_PTE_NULL_PAGE,
	PPC_PTE_NULL_CACHE,
	PPC_PTE_FOREIGN_CACHE,
	PPC_PTE_WIRED_ZERO
};

static const char* const kPPCPteTrustNames[] = {
	"OK", "NULLPAGE", "NULLCACHE", "FOREIGN", "WIRED0"
};

// Keep the gecko console usable: dump in full only this many times per boot.
#define PPC_PTE_FORENSIC_LIMIT 16
static int32 sPPCPteForensicCount = 0;


/*!	Returns the depth of \a pageCache in \a area's cache chain, or -1.

	B_FULL_LOCK/B_CONTIGUOUS/B_ALREADY_WIRED insert their pages into the area's
	own cache, so those wire pages at depth 0; a B_FULL_LOCK clone made by
	vm_copy_area() and every B_LAZY_LOCK soft fault can instead wire a page
	owned by a source cache, so the whole chain counts as "ours" and the depth
	is what tells the two apart in the dump.
*/
static int
ppc_area_cache_depth(VMArea* area, VMCache* pageCache)
{
	VMCache* cache = area->cache;
	for (int depth = 0; cache != NULL && depth < 16;
			depth++, cache = cache->source) {
		if (cache == pageCache)
			return depth;
	}

	return -1;
}


/*!	Vets the page a page table entry names before a wired area accounts it.

	Wired areas carry no vm_page_mapping objects, so UnmapPage()/UnmapPages()
	have nothing to check a hardware entry against and used to hand every page
	they found straight to PageUnmapped() -> DecrementWiredCount(). If the entry
	is stale, or the page was already released, that silently corrupts a
	stranger's wired count - the first wrong step behind the undertaker panic.
	Returns true when the page really belongs here; otherwise dumps one dense
	line and lets the caller drop the entry without touching the page at all.

	One-shot diagnostic: page/cache fields are read without their own locks (we
	only hold the translation map lock), which is acceptable because nothing
	acts on the values beyond printing them.
*/
static bool
ppc_check_wired_pte(VMArea* area, addr_t address, page_table_entry* entry,
	page_num_t pageNumber)
{
	vm_page* page = vm_lookup_page(pageNumber);
	VMCache* pageCache = page != NULL ? page->Cache() : NULL;
	int depth = pageCache != NULL ? ppc_area_cache_depth(area, pageCache) : -1;

	int reason = PPC_PTE_TRUSTED;
	if (page == NULL)
		reason = PPC_PTE_NULL_PAGE;
	else if (pageCache == NULL)
		reason = PPC_PTE_NULL_CACHE;
	else if (depth < 0)
		reason = PPC_PTE_FOREIGN_CACHE;
	else if (page->WiredCount() == 0)
		reason = PPC_PTE_WIRED_ZERO;

	if (reason == PPC_PTE_TRUSTED)
		return true;

	int32 count = atomic_add(&sPPCPteForensicCount, 1);
	if (count >= PPC_PTE_FORENSIC_LIMIT) {
		if (count == PPC_PTE_FORENSIC_LIMIT) {
			dprintf("PPC-PTE-FORENSIC: limit reached, further events are "
				"counted silently\n");
		}
		return false;
	}

	// The cache's first area names who really owns the page (e.g. a stack).
	const char* cacheOwner = "-";
	unsigned int cacheType = 0;
	if (pageCache != NULL) {
		cacheType = (unsigned int)pageCache->type;
		VMArea* ownerArea = pageCache->areas.Head();
		if (ownerArea != NULL)
			cacheOwner = ownerArea->name;
	}

	// List the areas the page still thinks it is mapped into (B_NO_LOCK ones).
	char mappedAreas[96];
	size_t pos = 0;
	int mappings = 0;
	if (page != NULL) {
		vm_page_mappings::Iterator it = page->mappings.GetIterator();
		while (vm_page_mapping* mapping = it.Next()) {
			mappings++;
			if (mappings > 4 || pos + 1 >= sizeof(mappedAreas))
				continue;
			int written = snprintf(mappedAreas + pos, sizeof(mappedAreas) - pos,
				"%s%" B_PRId32 ":%s", mappings > 1 ? "," : "",
				mapping->area->id, mapping->area->name);
			if (written < 0 || (size_t)written >= sizeof(mappedAreas) - pos) {
				pos = sizeof(mappedAreas) - 1;
				continue;
			}
			pos += written;
		}
	}
	mappedAreas[pos] = '\0';

	uint32* raw = (uint32*)entry;
	VMAddressSpace* space = area->address_space;

	dprintf("PPC-PTE-FORENSIC: %s n=%" B_PRId32 " va=%#" B_PRIxADDR " area=%"
		B_PRId32 " \"%s\" base=%#" B_PRIxADDR " size=%#" B_PRIxSIZE
		" wiring=%u %s team=%" B_PRId32 " pte=%08" B_PRIx32 ":%08" B_PRIx32
		" vsid=%#x H=%u API=%#x rpn=%#x R=%u C=%u PP=%u pfn=%#" B_PRIxPHYSADDR
		" page=%p state=%u wired=%u busy=%u cache=%p ctype=%u cdepth=%d"
		" cowner=\"%s\" nmaps=%d maps=[%s]\n",
		kPPCPteTrustNames[reason], count + 1, address, area->id, area->name,
		area->Base(), area->Size(), (unsigned int)area->wiring,
		space == VMAddressSpace::Kernel() ? "kernel" : "user",
		space != NULL ? space->ID() : (team_id)-1, raw[0], raw[1],
		(unsigned int)entry->virtual_segment_id,
		(unsigned int)entry->secondary_hash,
		(unsigned int)entry->abbr_page_index,
		(unsigned int)entry->physical_page_number,
		(unsigned int)entry->referenced, (unsigned int)entry->changed,
		(unsigned int)entry->page_protection, pageNumber, page,
		page != NULL ? (unsigned int)page->State() : 0,
		page != NULL ? (unsigned int)page->WiredCount() : 0,
		page != NULL ? (unsigned int)page->busy : 0, pageCache, cacheType,
		depth, cacheOwner, mappings, mappedAreas);

	return false;
}


// #pragma mark -


PPCVMTranslationMapClassic::PPCVMTranslationMapClassic()
	:
	fPagingStructures(NULL)
{
}


PPCVMTranslationMapClassic::~PPCVMTranslationMapClassic()
{
	if (fPagingStructures == NULL)
		return;

#if 0//X86
	if (fPageMapper != NULL)
		fPageMapper->Delete();
#endif

	// Note: fMapCount is no longer an exact per-map page-table-entry count once
	// Map() may evict entries belonging to other address spaces (see Map()), so
	// a non-zero value here is not an error. What matters for correctness is
	// that no live entry tagged with this map's VSID survives - the forced
	// per-page UnmapArea() teardown removes every such entry - so it is safe to
	// tear the map down regardless of the counter.
	if (fMapCount > 0) {
		TRACE("destroy_tmap: map %p torn down with residual map count %d "
			"(expected under page-table-entry eviction)\n", this, fMapCount);
	}

	// mark the vsid base not in use
	int baseBit = fVSIDBase >> VSID_BASE_SHIFT;
	atomic_and((int32 *)&sVSIDBaseBitmap[baseBit / 32],
			~(1 << (baseBit % 32)));

#if 0//X86
	if (fPagingStructures->pgdir_virt != NULL) {
		// cycle through and free all of the user space pgtables
		for (uint32 i = VADDR_TO_PDENT(USER_BASE);
				i <= VADDR_TO_PDENT(USER_BASE + (USER_SIZE - 1)); i++) {
			if ((fPagingStructures->pgdir_virt[i] & PPC_PDE_PRESENT) != 0) {
				addr_t address = fPagingStructures->pgdir_virt[i]
					& PPC_PDE_ADDRESS_MASK;
				vm_page* page = vm_lookup_page(address / B_PAGE_SIZE);
				if (!page)
					panic("destroy_tmap: didn't find pgtable page\n");
				DEBUG_PAGE_ACCESS_START(page);
				vm_page_free_etc(NULL, page, &reservation);
			}
		}
	}
#endif

	fPagingStructures->RemoveReference();
}


status_t
PPCVMTranslationMapClassic::Init(bool kernel)
{
	TRACE("PPCVMTranslationMapClassic::Init()\n");

	PPCVMTranslationMap::Init(kernel);

	cpu_status state = disable_interrupts();
	acquire_spinlock(&sVSIDBaseBitmapLock);

	// allocate a VSID base for this one
	if (kernel) {
		// The boot loader has set up the segment registers for identical
		// mapping, preserving whatever VSID Open Firmware actually had
		// active per segment (necessarily so - it's an arbitrary,
		// firmware-assigned value with no fixed relationship to the segment
		// index; see the loader's own mmu.cpp for the original fix this
		// mirrors). This used to be assumed to always be 0 - i.e. VSID ==
		// segment index - which held on whatever firmware this code was
		// last verified against, but does not hold in general: confirmed
		// directly on QEMU/OpenBIOS, where the real VSID for segment 0 is
		// 1024, not 0. A hardcoded fVSIDBase caused every kernel-space
		// Map()/Query() to tag entries with a VSID the hardware's segment
		// registers don't actually have active, making the software-side
		// hash table lookups self-consistent but functionally disconnected
		// from what the CPU's own MMU resolves for a real memory access -
		// most visibly, a translation map remap (used to relocate the page
		// table itself out of low memory during vm_translation_map_init_
		// post_area()) would successfully write a "new" mapping that real
		// hardware translation could never actually reach, silently
		// resolving reads through it to whatever unrelated, pre-existing
		// mapping happened to already cover that virtual range instead.
		// Query the real, current value instead of assuming it.
		fVSIDBase = get_sr((void*)0) & 0xffffff;

		// Record every kernel segment's real VSID, so the page table
		// eviction code can tell a kernel entry from a user one. They are
		// firmware-assigned and need not be contiguous, so all 16 are read
		// rather than derived from segment 0's.
		for (int i = 0; i < 16; i++)
			sKernelVSIDs[i] = get_sr((void*)((addr_t)i << 28)) & 0xffffff;
		sKernelVSIDsKnown = true;

		// Two VSID bases are reserved for the kernel, spanning it: the
		// kernel's fVSIDBase covers all 16 segments (this translation map
		// is used for both the low addresses required by Open Firmware
		// services and the kernel address space at 0x80000000+), which in
		// the 8-segments-per-base allocation scheme below spans exactly two
		// base slots.
		uint32 kernelBaseBit = fVSIDBase >> VSID_BASE_SHIFT;
		sVSIDBaseBitmap[kernelBaseBit / 32] |= 1 << (kernelBaseBit % 32);
		sVSIDBaseBitmap[(kernelBaseBit + 1) / 32]
			|= 1 << ((kernelBaseBit + 1) % 32);
	} else {
		int i = 0;

		while (i < MAX_VSID_BASES) {
			if (sVSIDBaseBitmap[i / 32] == 0xffffffff) {
				i += 32;
				continue;
			}
			if ((sVSIDBaseBitmap[i / 32] & (1 << (i % 32))) == 0) {
				// we found it
				sVSIDBaseBitmap[i / 32] |= 1 << (i % 32);
				break;
			}
			i++;
		}
		if (i >= MAX_VSID_BASES)
			panic("vm_translation_map_create: out of VSID bases\n");
		fVSIDBase = i << VSID_BASE_SHIFT;
	}

	release_spinlock(&sVSIDBaseBitmapLock);
	restore_interrupts(state);

	fPagingStructures = new(std::nothrow) PPCPagingStructuresClassic;
	if (fPagingStructures == NULL)
		return B_NO_MEMORY;

	PPCPagingMethodClassic* method = PPCPagingMethodClassic::Method();

	if (!kernel) {
		// user
#if 0//X86
		// allocate a physical page mapper
		status_t error = method->PhysicalPageMapper()
			->CreateTranslationMapPhysicalPageMapper(&fPageMapper);
		if (error != B_OK)
			return error;
#endif
#if 0//X86
		// allocate the page directory
		page_directory_entry* virtualPageDir = (page_directory_entry*)memalign(
			B_PAGE_SIZE, B_PAGE_SIZE);
		if (virtualPageDir == NULL)
			return B_NO_MEMORY;

		// look up the page directory's physical address
		phys_addr_t physicalPageDir;
		vm_get_page_mapping(VMAddressSpace::KernelID(),
			(addr_t)virtualPageDir, &physicalPageDir);
#endif

		fPagingStructures->Init(/*NULL, 0,
			method->KernelVirtualPageDirectory()*/method->PageTable());
	} else {
		// kernel
#if 0//X86
		// get the physical page mapper
		fPageMapper = method->KernelPhysicalPageMapper();
#endif

		// we already know the kernel pgdir mapping
		fPagingStructures->Init(/*method->KernelVirtualPageDirectory(),
			method->KernelPhysicalPageDirectory(), NULL*/method->PageTable());
	}

	return B_OK;
}


void
PPCVMTranslationMapClassic::ChangeASID()
{
// this code depends on the kernel being at 0x80000000, fix if we change that
#if KERNEL_BASE != 0x80000000
#error fix me
#endif
	int vsidBase = VSIDBase();

	isync();	// synchronize context
	asm("mtsr	0,%0" : : "g"(vsidBase));
	asm("mtsr	1,%0" : : "g"(vsidBase + 1));
	asm("mtsr	2,%0" : : "g"(vsidBase + 2));
	asm("mtsr	3,%0" : : "g"(vsidBase + 3));
	asm("mtsr	4,%0" : : "g"(vsidBase + 4));
	asm("mtsr	5,%0" : : "g"(vsidBase + 5));
	asm("mtsr	6,%0" : : "g"(vsidBase + 6));
	asm("mtsr	7,%0" : : "g"(vsidBase + 7));
	isync();	// synchronize context
}


page_table_entry *
PPCVMTranslationMapClassic::LookupPageTableEntry(addr_t virtualAddress)
{
	// lookup the vsid based off the va
	uint32 virtualSegmentID = vsid_for_address(fVSIDBase, virtualAddress);

//	dprintf("vm_translation_map.lookup_page_table_entry: vsid %ld, va 0x%lx\n", virtualSegmentID, virtualAddress);

	PPCPagingMethodClassic* m = PPCPagingMethodClassic::Method();

	// Search for the page table entry using the primary hash value

	uint32 hash = page_table_entry::PrimaryHash(virtualSegmentID, virtualAddress);
	page_table_entry_group *group = &(m->PageTable())[hash & m->PageTableHashMask()];
	page_table_entry_group *primaryGroup = group;

	// Only a VALID entry may match: an invalidated slot keeps its tag fields,
	// and returning such a twin makes every consumer act on the wrong slot -
	// most fatally Map()'s stale-translation defense, which would clear the
	// dead twin and leave the live PTE (and its translation) untouched.
	for (int i = 0; i < 8; i++) {
		page_table_entry *entry = &group->entry[i];

		if (entry->valid
			&& entry->virtual_segment_id == virtualSegmentID
			&& entry->secondary_hash == false
			&& entry->abbr_page_index == ((virtualAddress >> 22) & 0x3f))
			return entry;
	}

	// didn't find it, try the secondary hash value

	hash = page_table_entry::SecondaryHash(hash);
	group = &(m->PageTable())[hash & m->PageTableHashMask()];

	for (int i = 0; i < 8; i++) {
		page_table_entry *entry = &group->entry[i];

		if (entry->valid
			&& entry->virtual_segment_id == virtualSegmentID
			&& entry->secondary_hash == true
			&& entry->abbr_page_index == ((virtualAddress >> 22) & 0x3f))
			return entry;
	}

	return NULL;
}


bool
PPCVMTranslationMapClassic::RemovePageTableEntry(addr_t virtualAddress)
{
	page_table_entry *entry = LookupPageTableEntry(virtualAddress);
	bool found = (entry != NULL);
	if (found)
		entry->valid = 0;

	// Always invalidate the TLB for this address, even when the hash-table
	// entry was already gone. Map()'s eviction drops a PTE from the table
	// WITHOUT a tlbie, so a stale TLB entry can still be resolving this address
	// to its old physical page. If we skipped the tlbie here (the entry ==
	// NULL / evicted case), that stale translation would survive an unmap or a
	// COW re-map, and the address would alias a freed/reused physical page --
	// the real-hardware-only 'wrong physical page' corruption behind the ppc
	// fork/teardown/window-open crashes. A tlbie for an address with no live
	// TLB entry is a harmless no-op.
	ppc_sync();
	tlbie(virtualAddress);
	eieio();
	tlbsync();
	ppc_sync();

	return found;
}


size_t
PPCVMTranslationMapClassic::MaxPagesNeededToMap(addr_t start, addr_t end) const
{
	return 0;
}


// PPC keeps the instruction and data caches incoherent, and the classic page
// table has no per-page execute bit, so a page that is demand-paged in as data
// and then executed (all user code: runtime_loader and everything it loads)
// would run whatever stale bytes happen to be in the I-cache -- the executable
// loads fine on the cache-coherent emulator but crashes on real PPC hardware.
// Sync the page's caches when we insert an executable mapping. icbi/dcbst take
// effective addresses, so this is only valid while the freshly inserted
// translation is reachable in the active MMU context -- i.e. this map is active
// on the current CPU, which is exactly the case for a user demand-paging fault
// (the faulting thread runs in this very address space). Mappings made into a
// non-current address space are skipped; those pages are synced when they first
// fault in the context that actually runs them.
static void
ppc_sync_executable_mapping(PPCVMTranslationMapClassic* map,
	addr_t virtualAddress, uint32 attributes)
{
	if ((attributes & B_EXECUTE_AREA) == 0)
		return;
	if (!map->PagingStructures()->active_on_cpus.GetBit(smp_get_current_cpu()))
		return;
	arch_cpu_sync_icache((void*)virtualAddress, B_PAGE_SIZE);
}


status_t
PPCVMTranslationMapClassic::Map(addr_t virtualAddress,
	phys_addr_t physicalAddress, uint32 attributes,
	uint32 memoryType, vm_page_reservation* reservation)
{
	TRACE("map_tmap: entry pa 0x%lx va 0x%lx\n", pa, va);

	// lookup the vsid based off the va
	uint32 virtualSegmentID = vsid_for_address(fVSIDBase, virtualAddress);
	uint32 protection = 0;

	// ToDo: check this
	// all kernel mappings are R/W to supervisor code
	if (attributes & (B_READ_AREA | B_WRITE_AREA))
		protection = (attributes & B_WRITE_AREA) ? PTE_READ_WRITE : PTE_READ_ONLY;

	//dprintf("vm_translation_map.map_tmap: vsid %d, pa 0x%lx, va 0x%lx\n", vsid, pa, va);

	PPCPagingMethodClassic* m = PPCPagingMethodClassic::Method();

	// The generic physical page mapper reuses a fixed set of iospace virtual
	// addresses, remapping them to new physical pages without unmapping first
	// - it relies on Map() overwriting the existing translation, the way x86
	// does. Our slot search below only ever fills an *invalid* PTE, so without
	// removing a pre-existing mapping for this address we would leave a stale
	// duplicate PTE (and its cached TLB entry) still pointing at the old
	// physical page. The hardware hash walk could then satisfy the translation
	// from the stale entry, so reads/writes through the window would hit the
	// wrong physical page - manifesting as random kernel memory corruption once
	// demand paging churns enough physical memory to force chunk eviction. Drop
	// any existing translation for this address first. (For a genuinely fresh
	// mapping this is a cheap lookup that finds nothing.)
	RemovePageTableEntry(virtualAddress);

	// Search for a free page table slot using the primary hash value
	uint32 hash = page_table_entry::PrimaryHash(virtualSegmentID, virtualAddress);
	page_table_entry_group *group = &(m->PageTable())[hash & m->PageTableHashMask()];

	for (int i = 0; i < 8; i++) {
		page_table_entry *entry = &group->entry[i];

		if (entry->valid)
			continue;

		m->FillPageTableEntry(entry, virtualSegmentID, virtualAddress,
			physicalAddress, protection, memoryType, false);
		fMapCount++;
		ppc_sync_executable_mapping(this, virtualAddress, attributes);
		return B_OK;
	}

	// Didn't found one, try the secondary hash value

	hash = page_table_entry::SecondaryHash(hash);
	group = &(m->PageTable())[hash & m->PageTableHashMask()];

	for (int i = 0; i < 8; i++) {
		page_table_entry *entry = &group->entry[i];

		if (entry->valid)
			continue;

		// This slot lives in the SECONDARY PTEG, so the entry must be tagged
		// secondary_hash = true (H=1). The hardware page-table walker checks the
		// primary PTEG for H=0 entries and the secondary PTEG for H=1 entries;
		// an H=0 entry placed in the secondary group matches neither walk, so
		// the CPU can never translate the address (DSI fault), and our own
		// LookupPageTableEntry() misses it too. This only triggers once a
		// primary PTEG fills all 8 slots (heavy memory pressure), which is why
		// it manifested only on real hardware under load; dingusppc's MMU model
		// resolves the entry regardless of the H bit and so never faulted.
		m->FillPageTableEntry(entry, virtualSegmentID, virtualAddress,
			physicalAddress, protection, memoryType, true);
		fMapCount++;
		ppc_sync_executable_mapping(this, virtualAddress, attributes);
		return B_OK;
	}

	// Both the primary and secondary hash buckets are full. The classic PPC
	// page table is a hardware *cache* of the software page mappings, not the
	// authoritative record (that is the VMArea/vm_page mapping list), so rather
	// than give up we evict an existing entry to make room. The displaced
	// virtual address simply takes a page fault the next time it is touched and
	// is re-inserted then. A rotating victim keeps any single slot from being
	// starved. No TLB invalidation is required: eviction only drops the
	// hash-table copy of a translation whose underlying page mapping is
	// unchanged, so a stale TLB entry still resolves to the correct physical
	// page until it is naturally cast out, whereupon the address refaults.
	// fMapCount is intentionally left alone: a slot is reused, not added, and
	// because a full PTEG routinely holds entries from several address spaces
	// the victim may belong to a different map, so a per-map count cannot be
	// kept exact here anyway - the destructor no longer relies on it.
	// (Known limitation: the victim's accessed/dirty bits are dropped rather
	// than written back to its vm_page; acceptable for this bring-up.)
	//
	// "It simply refaults" is only true for some entries, though, and the two
	// exceptions below must never be chosen as a victim:
	//
	// - A kernel mapping. It may be touched again with interrupts disabled
	//   (e.g. inside a page-queue critical section), where a fault cannot be
	//   serviced and panics instead ("page fault, but interrupts were
	//   disabled").
	// - A mapping of a wired page. The refault goes through map_page(), which
	//   for a wired area has no way to tell a refault from a first mapping and
	//   so increments the page's wired count a *second* time. The count then
	//   never falls back to zero, the page looks mapped forever, and tearing
	//   its cache down panics with "page still has mappings".
	//
	// Anything else may be dropped: a B_NO_LOCK mapping refaults harmlessly,
	// because map_page() recognises the still-live software mapping and only
	// re-inserts the page table entry (see the __POWERPC__ branch there).
	uint32 primaryHash = page_table_entry::PrimaryHash(virtualSegmentID,
		virtualAddress);
	page_table_entry_group *victimGroups[2];
	victimGroups[0] = &(m->PageTable())[primaryHash & m->PageTableHashMask()];
	victimGroups[1] = &(m->PageTable())[page_table_entry::SecondaryHash(
		primaryHash) & m->PageTableHashMask()];

	static uint32 sEvictionRotor = 0;
	int victimGroup = -1;
	int victim = -1;
	for (int g = 0; g < 2 && victim < 0; g++) {
		for (int i = 0; i < 8; i++) {
			int slot = (sEvictionRotor + i) & 7;
			page_table_entry* candidate = &victimGroups[g]->entry[slot];

			if (is_kernel_vsid(candidate->virtual_segment_id))
				continue;

			vm_page* page = vm_lookup_page(candidate->physical_page_number);
			if (page != NULL && page->WiredCount() > 0)
				continue;

			victimGroup = g;
			victim = slot;
			break;
		}
	}
	if (victim < 0) {
		// Every slot in both buckets is kernel-owned or wired. Dropping one is
		// wrong whichever we pick, but refusing to map is worse (Map()'s
		// callers do not handle failure), so fall back to the rotor and make
		// the situation visible instead of silent.
		static bool sEvictionFallbackWarned = false;
		if (!sEvictionFallbackWarned) {
			sEvictionFallbackWarned = true;
			dprintf("PPCVMTranslationMapClassic::Map(): page table group for "
				"va %#" B_PRIxADDR " holds only kernel/wired entries - "
				"evicting one anyway\n", virtualAddress);
		}
		victimGroup = 0;
		victim = sEvictionRotor & 7;
	}
	sEvictionRotor++;

	// Invalidate the victim's hardware TLB entry before we overwrite its
	// page-table slot. The classic page table is only a *cache* of the
	// software mappings, but the CPU's TLB independently caches translations;
	// dropping the victim's PTE here does NOT drop its TLB entry. If we leave
	// it, the victim's address keeps resolving (via the stale TLB) to its old
	// physical page. Neither our unmap nor our remap path invalidates it: both
	// tlbie only when they find a live PTE, and an evicted address has none. A
	// later remap of the same address would then be shadowed by the stale
	// translation, and a freed-then-recycled page would be silently aliased.
	// (Real-hardware only; dingusppc has no TLB model.)
	// tlbie selects the TLB congruence class by effective-address bits and is
	// independent of the VSID, so it suffices to reconstruct the victim's low
	// page-index bits from its PTE and the group it occupies:
	//   groupIndex == (victim->secondary_hash ? ~PrimaryHash : PrimaryHash) & mask
	//   PrimaryHash == (VSID & 0x7ffff) ^ (pageIndex & 0xffff)
	// so  pageIndex&mask == ((victim->secondary_hash ? ~groupIndex : groupIndex)
	//                          ^ (VSID & 0x7ffff)) & mask
	// and the effective address is pageIndex << 12 (VA[12:27]).
	{
		page_table_entry* victimEntry
			= &victimGroups[victimGroup]->entry[victim];
		if (victimEntry->valid) {
			uint32 mask = m->PageTableHashMask();
			uint32 groupIndex = (victimGroup == 0
				? primaryHash
				: page_table_entry::SecondaryHash(primaryHash)) & mask;
			uint32 victimPrimaryMasked = victimEntry->secondary_hash != 0
				? (~groupIndex & mask) : (groupIndex & mask);
			uint32 victimPageIndex = (victimPrimaryMasked
				^ (victimEntry->virtual_segment_id & 0x7ffff)) & mask;
			addr_t victimEA = (addr_t)victimPageIndex << 12;
			ppc_sync();
			tlbie(victimEA);
			eieio();
			tlbsync();
			ppc_sync();
		}
	}

	m->FillPageTableEntry(&victimGroups[victimGroup]->entry[victim],
		virtualSegmentID, virtualAddress, physicalAddress, protection,
		memoryType, victimGroup == 1);
	ppc_sync_executable_mapping(this, virtualAddress, attributes);
	return B_OK;

#if 0//X86
/*
	dprintf("pgdir at 0x%x\n", pgdir);
	dprintf("index is %d\n", va / B_PAGE_SIZE / 1024);
	dprintf("final at 0x%x\n", &pgdir[va / B_PAGE_SIZE / 1024]);
	dprintf("value is 0x%x\n", *(int *)&pgdir[va / B_PAGE_SIZE / 1024]);
	dprintf("present bit is %d\n", pgdir[va / B_PAGE_SIZE / 1024].present);
	dprintf("addr is %d\n", pgdir[va / B_PAGE_SIZE / 1024].addr);
*/
	page_directory_entry* pd = fPagingStructures->pgdir_virt;

	// check to see if a page table exists for this range
	uint32 index = VADDR_TO_PDENT(va);
	if ((pd[index] & PPC_PDE_PRESENT) == 0) {
		phys_addr_t pgtable;
		vm_page *page;

		// we need to allocate a pgtable
		page = vm_page_allocate_page(reservation,
			PAGE_STATE_WIRED | VM_PAGE_ALLOC_CLEAR);

		DEBUG_PAGE_ACCESS_END(page);

		pgtable = (phys_addr_t)page->physical_page_number * B_PAGE_SIZE;

		TRACE("map_tmap: asked for free page for pgtable. 0x%lx\n", pgtable);

		// put it in the pgdir
		PPCPagingMethodClassic::PutPageTableInPageDir(&pd[index], pgtable,
			attributes
				| ((attributes & B_USER_PROTECTION) != 0
						? B_WRITE_AREA : B_KERNEL_WRITE_AREA));

		// update any other page directories, if it maps kernel space
		if (index >= FIRST_KERNEL_PGDIR_ENT
			&& index < (FIRST_KERNEL_PGDIR_ENT + NUM_KERNEL_PGDIR_ENTS)) {
			PPCPagingStructuresClassic::UpdateAllPageDirs(index, pd[index]);
		}

		fMapCount++;
	}

	// now, fill in the pentry
	Thread* thread = thread_get_current_thread();
	ThreadCPUPinner pinner(thread);

	page_table_entry* pt = (page_table_entry*)fPageMapper->GetPageTableAt(
		pd[index] & PPC_PDE_ADDRESS_MASK);
	index = VADDR_TO_PTENT(va);

	ASSERT_PRINT((pt[index] & PPC_PTE_PRESENT) == 0,
		"virtual address: %#" B_PRIxADDR ", existing pte: %#" B_PRIx32, va,
		pt[index]);

	PPCPagingMethodClassic::PutPageTableEntryInTable(&pt[index], pa, attributes,
		memoryType, fIsKernelMap);

	pinner.Unlock();

	// Note: We don't need to invalidate the TLB for this address, as previously
	// the entry was not present and the TLB doesn't cache those entries.

	fMapCount++;

	return 0;
#endif
}


status_t
PPCVMTranslationMapClassic::Unmap(addr_t start, addr_t end)
{
	page_table_entry *entry;

	start = ROUNDDOWN(start, B_PAGE_SIZE);
	end = ROUNDUP(end, B_PAGE_SIZE);

	if (start >= end)
		return B_OK;

	TRACE("unmap_tmap: asked to free pages 0x%lx to 0x%lx\n", start, end);

//	dprintf("vm_translation_map.unmap_tmap: start 0x%lx, end 0x%lx\n", start, end);

	while (start < end) {
		if (RemovePageTableEntry(start))
			fMapCount--;

		start += B_PAGE_SIZE;
	}

	return B_OK;

#if 0//X86

	start = ROUNDDOWN(start, B_PAGE_SIZE);
	if (start >= end)
		return B_OK;

	TRACE("unmap_tmap: asked to free pages 0x%lx to 0x%lx\n", start, end);

	page_directory_entry *pd = fPagingStructures->pgdir_virt;

	do {
		int index = VADDR_TO_PDENT(start);
		if ((pd[index] & PPC_PDE_PRESENT) == 0) {
			// no page table here, move the start up to access the next page
			// table
			start = ROUNDUP(start + 1, kPageTableAlignment);
			continue;
		}

		Thread* thread = thread_get_current_thread();
		ThreadCPUPinner pinner(thread);

		page_table_entry* pt = (page_table_entry*)fPageMapper->GetPageTableAt(
			pd[index] & PPC_PDE_ADDRESS_MASK);

		for (index = VADDR_TO_PTENT(start); (index < 1024) && (start < end);
				index++, start += B_PAGE_SIZE) {
			if ((pt[index] & PPC_PTE_PRESENT) == 0) {
				// page mapping not valid
				continue;
			}

			TRACE("unmap_tmap: removing page 0x%lx\n", start);

			page_table_entry oldEntry
				= PPCPagingMethodClassic::ClearPageTableEntryFlags(&pt[index],
					PPC_PTE_PRESENT);
			fMapCount--;

			if ((oldEntry & PPC_PTE_ACCESSED) != 0) {
				// Note, that we only need to invalidate the address, if the
				// accessed flags was set, since only then the entry could have
				// been in any TLB.
				InvalidatePage(start);
			}
		}
	} while (start != 0 && start < end);

	return B_OK;
#endif
}


status_t
PPCVMTranslationMapClassic::RemapAddressRange(addr_t *_virtualAddress,
	size_t size, bool unmap)
{
	addr_t virtualAddress = ROUNDDOWN(*_virtualAddress, B_PAGE_SIZE);
	size = ROUNDUP(*_virtualAddress + size - virtualAddress, B_PAGE_SIZE);

	VMAddressSpace *addressSpace = VMAddressSpace::Kernel();

	// reserve space in the address space
	void *newAddress = NULL;
	status_t error = vm_reserve_address_range(addressSpace->ID(), &newAddress,
		B_ANY_KERNEL_ADDRESS, size, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (error != B_OK) {
		dprintf("RemapAddressRange: vm_reserve_address_range(size %" B_PRIuSIZE
			") failed: %#" B_PRIx32 "\n", size, (uint32)error);
		return error;
	}

	// Get the area's first physical page. This is expected to fail for the
	// low-memory ranges Open Firmware handed the loader (the page table
	// itself among them) - the loader preserves whatever VSID scheme OF
	// actually has active per segment (necessarily so; it's arbitrary,
	// firmware-assigned, and not knowable in advance), but that per-segment
	// VSID is never passed on to the kernel, so LookupPageTableEntry()'s
	// hardcoded VADDR_TO_VSID(fVSIDBase, ...) formula can't find entries
	// tagged with it. Every such preserved OF range observed in practice is
	// identity-mapped (virtual == physical) by construction - the loader
	// only ever preserves OF's own existing translations verbatim, it
	// doesn't relocate them - so fall back to that instead of failing
	// outright.
	page_table_entry *entry = LookupPageTableEntry(virtualAddress);
	phys_addr_t physicalBase;
	if (entry != NULL) {
		physicalBase = (phys_addr_t)entry->physical_page_number << 12;
	} else {
		dprintf("RemapAddressRange: LookupPageTableEntry(%p) found nothing, "
			"assuming an identity-mapped OF-preserved range\n",
			(void*)virtualAddress);
		physicalBase = (phys_addr_t)virtualAddress;
	}

	// map the pages
	error = ppc_map_address_range((addr_t)newAddress, physicalBase, size);
	if (error != B_OK) {
		dprintf("RemapAddressRange: ppc_map_address_range(%p, %p, %"
			B_PRIuSIZE ") failed: %#" B_PRIx32 "\n", newAddress,
			(void*)physicalBase, size, (uint32)error);
		return error;
	}

	*_virtualAddress = (addr_t)newAddress;

	// unmap the old pages
	if (unmap)
		ppc_unmap_address_range(virtualAddress, size);

	return B_OK;
}


status_t
PPCVMTranslationMapClassic::UnmapPage(VMArea* area, addr_t address,
	bool updatePageQueue, bool deletingAddressSpace, uint32* _flags)
{
	ASSERT(address % B_PAGE_SIZE == 0);
	ASSERT(_flags == NULL || !updatePageQueue);

	RecursiveLocker locker(fLock);

	if (area->cache_type == CACHE_TYPE_DEVICE) {
		if (!RemovePageTableEntry(address))
			return B_ENTRY_NOT_FOUND;

		fMapCount--;
		return B_OK;
	}

	page_table_entry* entry = LookupPageTableEntry(address);
	if (entry == NULL) {
		// With eviction (see Map()) a still-live mapping may have had its
		// page-table entry cast out. There is nothing to remove from hardware,
		// so report success (the caller removes the software mapping itself);
		// returning B_ENTRY_NOT_FOUND here would trip the generic UnmapArea's
		// "mapping without translation map entry" panic during teardown.
		if (_flags != NULL)
			*_flags = 0;
		return B_OK;
	}

	page_num_t pageNumber = entry->physical_page_number;
	bool accessed = entry->referenced;
	bool modified = entry->changed;

	// The hardware entry can be a stale cache copy that names a page whose
	// software mapping in this area was already removed (see UnmapPages() and
	// Map()). Only account it to PageUnmapped() when it is genuine, or its
	// "mapping != NULL" assert fires. A wired area has no mapping objects
	// (PageUnmapped() uses DecrementWiredCount()), so vet its page separately.
	bool genuine = area->wiring != B_NO_LOCK;
	if (!genuine) {
		vm_page* ptePage = vm_lookup_page(pageNumber);
		if (ptePage != NULL) {
			vm_page_mappings::Iterator it = ptePage->mappings.GetIterator();
			while (vm_page_mapping* m = it.Next()) {
				if (m->area == area) { genuine = true; break; }
			}
		}
	} else if (!ppc_check_wired_pte(area, address, entry, pageNumber)) {
		// Wired area, but the page isn't ours: drop the entry, account nothing.
		genuine = false;
	}

	RemovePageTableEntry(address);

	fMapCount--;

	if (_flags == NULL) {
		if (genuine) {
			locker.Detach();
				// PageUnmapped() will unlock for us

			PageUnmapped(area, pageNumber, accessed, modified, updatePageQueue);
		}
		// else: stale entry cleared above; nothing for PageUnmapped() to
		// remove. The RecursiveLocker unlocks normally on return.
	} else {
		uint32 flags = PAGE_PRESENT;
		if (accessed)
			flags |= PAGE_ACCESSED;
		if (modified)
			flags |= PAGE_MODIFIED;
		*_flags = flags;
	}

	return B_OK;

#if 0//X86

	ASSERT(address % B_PAGE_SIZE == 0);

	page_directory_entry* pd = fPagingStructures->pgdir_virt;

	TRACE("PPCVMTranslationMapClassic::UnmapPage(%#" B_PRIxADDR ")\n", address);

	RecursiveLocker locker(fLock);

	int index = VADDR_TO_PDENT(address);
	if ((pd[index] & PPC_PDE_PRESENT) == 0)
		return B_ENTRY_NOT_FOUND;

	ThreadCPUPinner pinner(thread_get_current_thread());

	page_table_entry* pt = (page_table_entry*)fPageMapper->GetPageTableAt(
		pd[index] & PPC_PDE_ADDRESS_MASK);

	index = VADDR_TO_PTENT(address);
	page_table_entry oldEntry = PPCPagingMethodClassic::ClearPageTableEntry(
		&pt[index]);

	pinner.Unlock();

	if ((oldEntry & PPC_PTE_PRESENT) == 0) {
		// page mapping not valid
		return B_ENTRY_NOT_FOUND;
	}

	fMapCount--;

	if ((oldEntry & PPC_PTE_ACCESSED) != 0) {
		// Note, that we only need to invalidate the address, if the
		// accessed flags was set, since only then the entry could have been
		// in any TLB.
		InvalidatePage(address);
		Flush();

		// NOTE: Between clearing the page table entry and Flush() other
		// processors (actually even this processor with another thread of the
		// same team) could still access the page in question via their cached
		// entry. We can obviously lose a modified flag in this case, with the
		// effect that the page looks unmodified (and might thus be recycled),
		// but is actually modified.
		// In most cases this is harmless, but for vm_remove_all_page_mappings()
		// this is actually a problem.
		// Interestingly FreeBSD seems to ignore this problem as well
		// (cf. pmap_remove_all()), unless I've missed something.
	}

	locker.Detach();
		// PageUnmapped() will unlock for us

	PageUnmapped(area, (oldEntry & PPC_PTE_ADDRESS_MASK) / B_PAGE_SIZE,
		(oldEntry & PPC_PTE_ACCESSED) != 0, (oldEntry & PPC_PTE_DIRTY) != 0,
		updatePageQueue);

	return B_OK;
#endif
}


void
PPCVMTranslationMapClassic::UnmapArea(VMArea* area, bool deletingAddressSpace,
	bool ignoreTopCachePageFlags)
{
	// The generic implementation leaves the top cache's page-table entries in
	// place when the whole address space is going away, relying on the arch
	// translation map's destructor to reclaim them wholesale - the way the x86
	// pmap simply frees its page directory. The classic ppc page table is a
	// single global hash shared by every address space and tagged only by
	// VSID, so entries left behind for a retired (and later recycled) VSID
	// base would silently alias into a different team, and our destructor has
	// no cheap page-directory to free that would drop them. Rather than walk
	// the entire hash at teardown, force the honest per-page unmap here by
	// clearing ignoreTopCachePageFlags: every entry is then removed (with a
	// correct tlbie for its real effective address) and fMapCount is driven
	// to zero, so the map is genuinely empty by the time it is destroyed.
	// NOTE: Map() DOES evict - when both hash buckets are full it drops an
	// existing user entry to make room (it only refuses to evict kernel
	// mappings). So a still-live user mapping can have no page-table entry
	// here, and UnmapPage() reports B_OK for that case rather than
	// B_ENTRY_NOT_FOUND. Beware: the address-walking UnmapPages() path, which
	// the generic UnmapArea uses for wired areas, simply skips such an address
	// and therefore leaks the software vm_page_mapping - the cause of the
	// "page still has mappings" panic in VMCache::Delete. Fixing that by
	// walking the mapping list here was tried and did NOT resolve the panic
	// (it also occurs for kernel areas, which are never evicted), so a second
	// cause remains unidentified.
	VMTranslationMap::UnmapArea(area, deletingAddressSpace, false);
}


void
PPCVMTranslationMapClassic::UnmapPages(VMArea* area, addr_t base, size_t size,
	bool updatePageQueue, bool deletingAddressSpace)
{
	// NOTE: unlike the dead x86 code this replaces, the classic ppc page
	// table is a hashed table (see LookupPageTableEntry()), not a two-level
	// directory/table structure - there is no page-directory-entry check to
	// skip whole unmapped regions with, so this simply walks every page in
	// [base, base + size) individually, the same way UnmapPage() handles a
	// single page. The per-page work is batched under one lock acquisition
	// and the resulting page-area mappings are queued and freed after
	// unlocking, exactly as UnmapPage() and the (still-live) x86 reference
	// implementation this was adapted from both do.
	if (size == 0)
		return;

	addr_t start = base;
	addr_t end = base + size - 1;

	TRACE("PPCVMTranslationMapClassic::UnmapPages(%p, %#" B_PRIxADDR ", %#"
		B_PRIxADDR ")\n", area, start, end);

	RecursiveLocker locker(fLock);

	if (area->cache_type == CACHE_TYPE_DEVICE) {
		for (addr_t address = start; address <= end; address += B_PAGE_SIZE) {
			if (RemovePageTableEntry(address))
				fMapCount--;
		}
		return;
	}

	VMAreaMappings queue;

	for (addr_t address = start; address <= end; address += B_PAGE_SIZE) {
		page_table_entry* entry = LookupPageTableEntry(address);
		if (entry == NULL)
			continue;

		page_num_t pageNumber = entry->physical_page_number;
		bool accessed = entry->referenced;
		bool modified = entry->changed;

		// The classic PPC page table is a *cache* of the software page
		// mappings (see Map()), not the authoritative record, so a present
		// hardware entry can be stale: it may name a page whose software
		// mapping in this area was already removed. Handing such a page to
		// PageUnmapped() would trip its "mapping != NULL" assert. Account the
		// page only when it is genuinely mapped here:
		//  - A wired area carries no vm_page_mapping objects (it uses
		//    wired_count); PageUnmapped() handles it via DecrementWiredCount(),
		//    so ppc_check_wired_pte() vets the page instead (see there).
		//  - Otherwise the page must have a mapping for this area.
		// A stale entry (verified over many boots to always name a page with
		// an empty mapping list) is simply cleared below; nothing is leaked,
		// because a live mapping at this address would have kept the entry
		// pointing at its own page rather than this stale one.
		bool genuine = area->wiring != B_NO_LOCK;
		if (!genuine) {
			vm_page* ptePage = vm_lookup_page(pageNumber);
			if (ptePage != NULL) {
				vm_page_mappings::Iterator it = ptePage->mappings.GetIterator();
				while (vm_page_mapping* m = it.Next()) {
					if (m->area == area) { genuine = true; break; }
				}
			}
		} else if (!ppc_check_wired_pte(area, address, entry, pageNumber)) {
			// Wired area, but the page isn't ours: drop the entry, account
			// nothing - never take a wired count we did not put there.
			genuine = false;
		}

		RemovePageTableEntry(address);
		fMapCount--;

		if (genuine) {
			PageUnmapped(area, pageNumber, accessed, modified, updatePageQueue,
				&queue);
		}
	}

	locker.Unlock();

	// free removed mappings
	bool isKernelSpace = area->address_space == VMAddressSpace::Kernel();
	uint32 freeFlags = CACHE_DONT_WAIT_FOR_MEMORY
		| (isKernelSpace ? CACHE_DONT_LOCK_KERNEL_SPACE : 0);
	while (vm_page_mapping* mapping = queue.RemoveHead())
		vm_free_page_mapping(mapping->page->physical_page_number, mapping, freeFlags);
}


status_t
PPCVMTranslationMapClassic::Query(addr_t va, phys_addr_t *_outPhysical,
	uint32 *_outFlags)
{
	page_table_entry *entry;

	// default the flags to not present
	*_outFlags = 0;
	*_outPhysical = 0;

	entry = LookupPageTableEntry(va);
	if (entry == NULL)
		return B_NO_ERROR;

	// ToDo: check this!
	if (IS_KERNEL_ADDRESS(va))
		*_outFlags |= B_KERNEL_READ_AREA | (entry->page_protection == PTE_READ_ONLY ? 0 : B_KERNEL_WRITE_AREA);
	else
		*_outFlags |= B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_READ_AREA | (entry->page_protection == PTE_READ_ONLY ? 0 : B_WRITE_AREA);

	*_outFlags |= entry->changed ? PAGE_MODIFIED : 0;
	*_outFlags |= entry->referenced ? PAGE_ACCESSED : 0;
	*_outFlags |= entry->valid ? PAGE_PRESENT : 0;

	*_outPhysical = entry->physical_page_number * B_PAGE_SIZE;

	return B_OK;

#if 0//X86
	// default the flags to not present
	*_flags = 0;
	*_physical = 0;

	int index = VADDR_TO_PDENT(va);
	page_directory_entry *pd = fPagingStructures->pgdir_virt;
	if ((pd[index] & PPC_PDE_PRESENT) == 0) {
		// no pagetable here
		return B_OK;
	}

	Thread* thread = thread_get_current_thread();
	ThreadCPUPinner pinner(thread);

	page_table_entry* pt = (page_table_entry*)fPageMapper->GetPageTableAt(
		pd[index] & PPC_PDE_ADDRESS_MASK);
	page_table_entry entry = pt[VADDR_TO_PTENT(va)];

	*_physical = entry & PPC_PDE_ADDRESS_MASK;

	// read in the page state flags
	if ((entry & PPC_PTE_USER) != 0) {
		*_flags |= ((entry & PPC_PTE_WRITABLE) != 0 ? B_WRITE_AREA : 0)
			| B_READ_AREA;
	}

	*_flags |= ((entry & PPC_PTE_WRITABLE) != 0 ? B_KERNEL_WRITE_AREA : 0)
		| B_KERNEL_READ_AREA
		| ((entry & PPC_PTE_DIRTY) != 0 ? PAGE_MODIFIED : 0)
		| ((entry & PPC_PTE_ACCESSED) != 0 ? PAGE_ACCESSED : 0)
		| ((entry & PPC_PTE_PRESENT) != 0 ? PAGE_PRESENT : 0);

	pinner.Unlock();

	TRACE("query_tmap: returning pa 0x%lx for va 0x%lx\n", *_physical, va);

	return B_OK;
#endif
}


status_t
PPCVMTranslationMapClassic::QueryInterrupt(addr_t virtualAddress,
	phys_addr_t *_physicalAddress, uint32 *_flags)
{
	return PPCVMTranslationMapClassic::Query(virtualAddress, _physicalAddress, _flags);

#if 0//X86
	*_flags = 0;
	*_physical = 0;

	int index = VADDR_TO_PDENT(va);
	page_directory_entry* pd = fPagingStructures->pgdir_virt;
	if ((pd[index] & PPC_PDE_PRESENT) == 0) {
		// no pagetable here
		return B_OK;
	}

	// map page table entry
	page_table_entry* pt = (page_table_entry*)PPCPagingMethodClassic::Method()
		->PhysicalPageMapper()->InterruptGetPageTableAt(
			pd[index] & PPC_PDE_ADDRESS_MASK);
	page_table_entry entry = pt[VADDR_TO_PTENT(va)];

	*_physical = entry & PPC_PDE_ADDRESS_MASK;

	// read in the page state flags
	if ((entry & PPC_PTE_USER) != 0) {
		*_flags |= ((entry & PPC_PTE_WRITABLE) != 0 ? B_WRITE_AREA : 0)
			| B_READ_AREA;
	}

	*_flags |= ((entry & PPC_PTE_WRITABLE) != 0 ? B_KERNEL_WRITE_AREA : 0)
		| B_KERNEL_READ_AREA
		| ((entry & PPC_PTE_DIRTY) != 0 ? PAGE_MODIFIED : 0)
		| ((entry & PPC_PTE_ACCESSED) != 0 ? PAGE_ACCESSED : 0)
		| ((entry & PPC_PTE_PRESENT) != 0 ? PAGE_PRESENT : 0);

	return B_OK;
#endif
}


status_t
PPCVMTranslationMapClassic::Protect(addr_t start, addr_t end, uint32 attributes,
	uint32 memoryType)
{
	(void)memoryType;
	start = ROUNDDOWN(start, B_PAGE_SIZE);
	if (start >= end)
		return B_OK;

	// Only enforce protection changes on USER maps. Kernel protection changes
	// happen during very early VM init; editing kernel PTEs there is not yet
	// validated on this port, and kernel protection is rare, so leave it
	// unenforced (as when this hook was a stub). The copy-on-write correctness
	// that fork() needs lives entirely in user maps.
	if (fIsKernelMap)
		return B_OK;

	uint32 protection = 0;
	if (attributes & (B_READ_AREA | B_WRITE_AREA))
		protection = (attributes & B_WRITE_AREA) ? PTE_READ_WRITE : PTE_READ_ONLY;
	protection &= 0x3;

	RecursiveLocker locker(fLock);

	for (addr_t va = start; va < end; va += B_PAGE_SIZE) {
		page_table_entry* entry = LookupPageTableEntry(va);
		if (entry == NULL || !entry->valid
			|| entry->page_protection == protection) {
			continue;
		}

		// Change PP in place, valid stays set (atomic to the hash walker, never
		// unmapped -> no fault). Flush stale TLB copies.
		entry->page_protection = protection;
		eieio();
		tlbie(va);
		eieio();
		tlbsync();
		ppc_sync();
	}

	return B_OK;
}


status_t
PPCVMTranslationMapClassic::ClearFlags(addr_t virtualAddress, uint32 flags)
{
	page_table_entry *entry = LookupPageTableEntry(virtualAddress);
	if (entry == NULL)
		return B_NO_ERROR;

	bool modified = false;

	// clear the bits
	if (flags & PAGE_MODIFIED && entry->changed) {
		entry->changed = false;
		modified = true;
	}
	if (flags & PAGE_ACCESSED && entry->referenced) {
		entry->referenced = false;
		modified = true;
	}

	// synchronize
	if (modified) {
		tlbie(virtualAddress);
		eieio();
		tlbsync();
		ppc_sync();
	}

	return B_OK;

#if 0//X86
	int index = VADDR_TO_PDENT(va);
	page_directory_entry* pd = fPagingStructures->pgdir_virt;
	if ((pd[index] & PPC_PDE_PRESENT) == 0) {
		// no pagetable here
		return B_OK;
	}

	uint32 flagsToClear = ((flags & PAGE_MODIFIED) ? PPC_PTE_DIRTY : 0)
		| ((flags & PAGE_ACCESSED) ? PPC_PTE_ACCESSED : 0);

	Thread* thread = thread_get_current_thread();
	ThreadCPUPinner pinner(thread);

	page_table_entry* pt = (page_table_entry*)fPageMapper->GetPageTableAt(
		pd[index] & PPC_PDE_ADDRESS_MASK);
	index = VADDR_TO_PTENT(va);

	// clear out the flags we've been requested to clear
	page_table_entry oldEntry
		= PPCPagingMethodClassic::ClearPageTableEntryFlags(&pt[index],
			flagsToClear);

	pinner.Unlock();

	if ((oldEntry & flagsToClear) != 0)
		InvalidatePage(va);

	return B_OK;
#endif
}


bool
PPCVMTranslationMapClassic::ClearAccessedAndModified(VMArea* area,
	addr_t address, bool unmapIfUnaccessed, bool& _modified)
{
	ASSERT(address % B_PAGE_SIZE == 0);

	// TODO: Implement for real! ATM this is just an approximation using
	// Query(), ClearFlags(), and UnmapPage(). See below!

	RecursiveLocker locker(fLock);

	uint32 flags;
	phys_addr_t physicalAddress;
	if (Query(address, &physicalAddress, &flags) != B_OK
		|| (flags & PAGE_PRESENT) == 0) {
		return false;
	}

	_modified = (flags & PAGE_MODIFIED) != 0;

	if ((flags & (PAGE_ACCESSED | PAGE_MODIFIED)) != 0)
		ClearFlags(address, flags & (PAGE_ACCESSED | PAGE_MODIFIED));

	if ((flags & PAGE_ACCESSED) != 0)
		return true;

	if (!unmapIfUnaccessed)
		return false;

	locker.Unlock();

	UnmapPage(area, address, false, false, NULL);
		// TODO: Obvious race condition: Between querying and unmapping the
		// page could have been accessed. We try to compensate by considering
		// vm_page::{accessed,modified} (which would have been updated by
		// UnmapPage()) below, but that doesn't quite match the required
		// semantics of the method.

	vm_page* page = vm_lookup_page(physicalAddress / B_PAGE_SIZE);
	if (page == NULL)
		return false;

	_modified |= page->modified;

	return page->accessed;

#if 0//X86
	page_directory_entry* pd = fPagingStructures->pgdir_virt;

	TRACE("PPCVMTranslationMapClassic::ClearAccessedAndModified(%#" B_PRIxADDR
		")\n", address);

	RecursiveLocker locker(fLock);

	int index = VADDR_TO_PDENT(address);
	if ((pd[index] & PPC_PDE_PRESENT) == 0)
		return false;

	ThreadCPUPinner pinner(thread_get_current_thread());

	page_table_entry* pt = (page_table_entry*)fPageMapper->GetPageTableAt(
		pd[index] & PPC_PDE_ADDRESS_MASK);

	index = VADDR_TO_PTENT(address);

	// perform the deed
	page_table_entry oldEntry;

	if (unmapIfUnaccessed) {
		while (true) {
			oldEntry = pt[index];
			if ((oldEntry & PPC_PTE_PRESENT) == 0) {
				// page mapping not valid
				return false;
			}

			if (oldEntry & PPC_PTE_ACCESSED) {
				// page was accessed -- just clear the flags
				oldEntry = PPCPagingMethodClassic::ClearPageTableEntryFlags(
					&pt[index], PPC_PTE_ACCESSED | PPC_PTE_DIRTY);
				break;
			}

			// page hasn't been accessed -- unmap it
			if (PPCPagingMethodClassic::TestAndSetPageTableEntry(&pt[index], 0,
					oldEntry) == oldEntry) {
				break;
			}

			// something changed -- check again
		}
	} else {
		oldEntry = PPCPagingMethodClassic::ClearPageTableEntryFlags(&pt[index],
			PPC_PTE_ACCESSED | PPC_PTE_DIRTY);
	}

	pinner.Unlock();

	_modified = (oldEntry & PPC_PTE_DIRTY) != 0;

	if ((oldEntry & PPC_PTE_ACCESSED) != 0) {
		// Note, that we only need to invalidate the address, if the
		// accessed flags was set, since only then the entry could have been
		// in any TLB.
		InvalidatePage(address);

		Flush();

		return true;
	}

	if (!unmapIfUnaccessed)
		return false;

	// We have unmapped the address. Do the "high level" stuff.

	fMapCount--;

	locker.Detach();
		// UnaccessedPageUnmapped() will unlock for us

	UnaccessedPageUnmapped(area,
		(oldEntry & PPC_PTE_ADDRESS_MASK) / B_PAGE_SIZE);

	return false;
#endif
}


PPCPagingStructures*
PPCVMTranslationMapClassic::PagingStructures() const
{
	return fPagingStructures;
}
