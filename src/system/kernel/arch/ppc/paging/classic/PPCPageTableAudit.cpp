/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Read-only consistency sweep over the classic PPC hashed page table.
	Session 14: the first wrong step of the undertaker NULL-Cache()/wired
	underflow bug is silent and only crashes hours later, so every valid page
	table entry is periodically checked against the VM's own bookkeeping.
	Diagnostic only - it never writes the table, never panics.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <OS.h>

#include <arch/cpu.h>
#include <arch_mmu.h>
#include <debug.h>
#include <kernel.h>
#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/vm_types.h>
#include <vm/VMAddressSpace.h>
#include <vm/VMArea.h>
#include <vm/VMCache.h>
#include <vm/VMTranslationMap.h>

#include "paging/classic/PPCPagingMethodClassic.h"


#define AUDIT_PREFIX			"PPC-PTE-AUDIT: "

// the valid bit is bit 0 of the upper word (big-endian bitfields, arch_mmu.h)
#define AUDIT_PTE_VALID			0x80000000

#define AUDIT_MAX_LINES			32
#define AUDIT_FAST_SWEEPS		10
#define AUDIT_DAEMON_FREQUENCY	100
	// kernel daemon iterations are 100 ms, so 100 == every 10 seconds
#define AUDIT_SLOW_SKIP			5
	// after the fast phase only every 6th wakeup sweeps, i.e. every 60 seconds
#define AUDIT_DEDUP_SLOTS		256
#define AUDIT_MAPPING_LIMIT		64

// Wii physical layout, mirrored from src/system/boot/platform/wii/mmu.cpp
#define AUDIT_MEM1_END			0x01800000
#define AUDIT_MEM2_BASE			0x10000000
#define AUDIT_MEM2_END			0x14000000
#define AUDIT_DEVICE_BASE		0x0c000000
#define AUDIT_DEVICE_END		0x0e000000

#define AUDIT_PRINT(state, x...) \
	do { if ((state).kdlMode) kprintf(x); else dprintf(x); } while (false)


struct audit_range {
	addr_t		base;
	addr_t		end;
};

// loader mappings the kernel never turns into areas (see the loader's mmu.cpp)
static const audit_range kLoaderWindows[] = {
	// the loader maps 16 kB of vectors, arch_int's own area covers only part
	{ 0xa0000000, 0xa0004000 },
	// uncached alias of MEM1
	{ 0xc0000000, 0xc1800000 },
	// Hollywood/Broadway register block (DEVICE_BASE, DEVICE_SIZE)
	{ 0xcc000000, 0xcd800000 },
	// uncached alias of MEM2
	{ 0xd0000000, 0xd4000000 },
};


struct pte_info {
	uint32	high;
	uint32	low;
	uint32	vsid;
	uint32	api;
	uint32	pageIndex;
	uint32	rpn;
	bool	secondary;
	bool	apiMismatch;
};


struct sweep_state {
	uint32	entries;
	uint32	kernelEntries;
	uint32	findings;
	uint32	printed;
	uint32	repeats;
	uint32	dropped;
	uint32	loaderWindow;
	addr_t	loaderWindowFirst;
	uint32	maxLines;
	bool	kdlMode;
};


// VSIDs of the kernel's own segment registers, re-read at every sweep
static uint32 sKernelSegmentVSIDs[8];

static uint32 sDedupKeys[AUDIT_DEDUP_SLOTS];
static uint32 sDedupUsed;
static uint32 sSweepNumber;
static uint32 sLastLoaderWindow = 0xffffffff;
static int32 sSweepsToSkip;
static int32 sInitialized;

static char sLine[512];
static char sExtra[64];


/*!	Reads the VSIDs of segments 8-15, the ones ChangeASID() never reprograms. */
static void
audit_read_kernel_segments()
{
	for (int i = 0; i < 8; i++)
		sKernelSegmentVSIDs[i] = get_sr((void*)((addr_t)(i + 8) << 28)) & 0xffffff;
}


/*!	Returns the kernel segment \a vsid belongs to, or -1 for a user VSID.

	Segments 0-7 hold whatever team is currently scheduled, so entries tagged
	with a user VSID only get the checks that need no effective address.
*/
static int
audit_kernel_segment(uint32 vsid)
{
	for (int i = 0; i < 8; i++) {
		if (sKernelSegmentVSIDs[i] == vsid)
			return i + 8;
	}

	return -1;
}


/*!	Reconstructs everything an entry knows about itself from its group index.

	This is the eviction tlbie math of PPCVMTranslationMapClassic::Map():
	groupIndex == (secondary ? ~PrimaryHash : PrimaryHash) & mask and
	PrimaryHash == (VSID & 0x7ffff) ^ (pageIndex & 0xffff).
*/
static void
audit_decode(uint32 groupIndex, uint32 hashMask, uint32 high, uint32 low,
	pte_info& info)
{
	info.high = high;
	info.low = low;
	info.vsid = (high >> 7) & 0xffffff;
	info.secondary = (high & 0x40) != 0;
	info.api = high & 0x3f;
	info.rpn = low >> 12;

	uint32 primaryMasked = info.secondary
		? (~groupIndex & hashMask) : (groupIndex & hashMask);
	uint32 pageIndexLow = (primaryMasked ^ (info.vsid & 0x7ffff)) & hashMask;

	// the API is page index bits 15..10, the hash supplies the rest
	info.pageIndex = ((info.api << 10) | (pageIndexLow & 0x3ff)) & 0xffff;

	// wherever the hash covers bit 10 and up, both sources must agree
	uint32 overlap = (hashMask >> 10) & 0x3f;
	info.apiMismatch = overlap != 0
		&& ((pageIndexLow >> 10) & overlap) != (info.api & overlap);
}


static bool
audit_rpn_is_sane(uint32 rpn)
{
	phys_addr_t physical = (phys_addr_t)rpn << 12;

	return physical < AUDIT_MEM1_END
		|| (physical >= AUDIT_MEM2_BASE && physical < AUDIT_MEM2_END)
		|| (physical >= AUDIT_DEVICE_BASE && physical < AUDIT_DEVICE_END);
}


static bool
audit_in_loader_window(addr_t ea)
{
	for (size_t i = 0; i < sizeof(kLoaderWindows) / sizeof(audit_range); i++) {
		if (ea >= kLoaderWindows[i].base && ea < kLoaderWindows[i].end)
			return true;
	}

	return false;
}


/*!	Cache() only reads cache_ref->cache, so a sane pointer is enough to call. */
static VMCache*
audit_page_cache(vm_page* page)
{
	VMCacheRef* cacheRef = page->CacheRef();
	if (cacheRef == NULL || !IS_KERNEL_ADDRESS((addr_t)cacheRef))
		return NULL;

	return page->Cache();
}


/*!	Remembers findings already reported, so a standing problem prints once. */
static bool
audit_seen_before(const char* what, addr_t ea, uint32 vsid, uint32 rpn)
{
	uint32 key = (uint32)(addr_t)what * 2654435761U;
	key ^= (uint32)ea * 40503U;
	key ^= vsid * 2246822519U;
	key ^= rpn * 374761393U;
	if (key == 0)
		key = 1;

	// once the table is full everything prints again, rate limiting still applies
	if (sDedupUsed >= AUDIT_DEDUP_SLOTS)
		return false;

	uint32 start = (key >> 8) % AUDIT_DEDUP_SLOTS;
	for (uint32 i = 0; i < AUDIT_DEDUP_SLOTS; i++) {
		uint32 index = (start + i) % AUDIT_DEDUP_SLOTS;
		if (sDedupKeys[index] == key)
			return true;
		if (sDedupKeys[index] == 0) {
			sDedupKeys[index] = key;
			sDedupUsed++;
			return false;
		}
	}

	return false;
}


static size_t
audit_advance(size_t length, int written, size_t limit)
{
	if (written < 0)
		return limit - 1;

	size_t next = length + (size_t)written;
	return next < limit ? next : limit - 1;
}


/*!	Prints one dense line per finding, built in one call so it can't interleave. */
static void
audit_report(sweep_state& state, const char* what, int segment, addr_t ea,
	uint32 groupIndex, int slot, const pte_info& info, VMArea* area,
	vm_page* page, const char* extra)
{
	state.findings++;

	if (audit_seen_before(what, ea, info.vsid, info.rpn)) {
		state.repeats++;
		return;
	}

	if (state.printed >= state.maxLines) {
		state.dropped++;
		return;
	}
	state.printed++;

	size_t length = audit_advance(0, snprintf(sLine, sizeof(sLine),
		AUDIT_PREFIX "%s ea %#010lx seg %d vsid %#lx rpn %#lx pte %08lx:%08lx "
		"grp %#lx.%d sec %d api %#lx", what, (unsigned long)ea, segment,
		(unsigned long)info.vsid, (unsigned long)info.rpn,
		(unsigned long)info.high, (unsigned long)info.low,
		(unsigned long)groupIndex, slot, info.secondary ? 1 : 0,
		(unsigned long)info.api), sizeof(sLine));

	if (area != NULL) {
		length = audit_advance(length, snprintf(sLine + length,
			sizeof(sLine) - length, " | area %d \"%s\" wiring %u ctype %u "
			"base %#lx size %#lx acache %p", (int)area->id, area->name,
			(unsigned)area->wiring, (unsigned)area->cache_type,
			(unsigned long)area->Base(), (unsigned long)area->Size(),
			(void*)area->cache), sizeof(sLine));
	}

	if (page != NULL) {
		length = audit_advance(length, snprintf(sLine + length,
			sizeof(sLine) - length, " | page %p state %u wired %u cache %p "
			"coff %lu", (void*)page, (unsigned)page->State(),
			(unsigned)page->WiredCount(), (void*)audit_page_cache(page),
			(unsigned long)page->cache_offset), sizeof(sLine));
	}

	if (extra != NULL) {
		length = audit_advance(length, snprintf(sLine + length,
			sizeof(sLine) - length, " | %s", extra), sizeof(sLine));
	}

	AUDIT_PRINT(state, "%s\n", sLine);
}


/*!	Finds a second valid entry for the same (VSID, effective address).

	A duplicate can only sit in the primary or the secondary group of that
	address, so no side table is needed. Only the earlier of the two positions
	reports, which makes every pair show up exactly once.
*/
static bool
audit_find_duplicate(page_table_entry_group* table, uint32 hashMask,
	uint32 groupIndex, int slot, const pte_info& info, uint32* _group,
	int* _slot)
{
	uint32 groups[2] = { groupIndex, ~groupIndex & hashMask };
	uint32 ownKey = groupIndex * 8 + (uint32)slot;

	for (int g = 0; g < 2; g++) {
		for (int s = 0; s < 8; s++) {
			if (groups[g] * 8 + (uint32)s <= ownKey)
				continue;

			const volatile uint32* raw
				= (const volatile uint32*)&table[groups[g]].entry[s];
			uint32 high = raw[0];
			if ((high & AUDIT_PTE_VALID) == 0)
				continue;

			pte_info other;
			audit_decode(groups[g], hashMask, high, raw[1], other);
			if (other.vsid == info.vsid && other.pageIndex == info.pageIndex) {
				*_group = groups[g];
				*_slot = s;
				return true;
			}
		}
	}

	return false;
}


/*!	Checks whether \a page still records a mapping for \a area. */
static bool
audit_has_mapping(vm_page* page, VMArea* area, bool kdlMode)
{
	VMTranslationMap* map = area->address_space != NULL
		? area->address_space->TranslationMap() : NULL;
	if (!kdlMode && map != NULL)
		map->Lock();

	bool found = false;
	uint32 seen = 0;
	for (vm_page_mappings::Iterator it = page->mappings.GetIterator();
			vm_page_mapping* mapping = it.Next();) {
		if (mapping->area == area) {
			found = true;
			break;
		}

		// never chase a long (or concurrently changing) list, assume it's fine
		if (++seen >= AUDIT_MAPPING_LIMIT) {
			found = true;
			break;
		}
	}

	if (!kdlMode && map != NULL)
		map->Unlock();

	return found;
}


static void
audit_check_kernel_entry(sweep_state& state, VMAddressSpace* kernelSpace,
	VMArea** _lastArea, int segment, addr_t ea, uint32 groupIndex, int slot,
	const pte_info& info)
{
	if (audit_in_loader_window(ea)) {
		state.loaderWindow++;
		if (state.loaderWindowFirst == 0)
			state.loaderWindowFirst = ea;
		return;
	}

	VMArea* area = *_lastArea;
	if (area == NULL || !area->ContainsAddress(ea))
		area = kernelSpace->LookupArea(ea);
	if (area == NULL) {
		audit_report(state, "STALE-NO-AREA", segment, ea, groupIndex, slot,
			info, NULL, NULL, NULL);
		return;
	}
	*_lastArea = area;

	// device and null caches keep no vm_page ledger to compare against
	if (area->cache_type != CACHE_TYPE_RAM
		&& area->cache_type != CACHE_TYPE_VNODE) {
		return;
	}

	vm_page* page = vm_lookup_page(info.rpn);
	if (page == NULL) {
		audit_report(state, "NO-PAGE", segment, ea, groupIndex, slot, info,
			area, NULL, NULL);
		return;
	}

	VMCacheRef* cacheRef = page->CacheRef();
	if (cacheRef != NULL && !IS_KERNEL_ADDRESS((addr_t)cacheRef)) {
		audit_report(state, "BAD-CACHEREF", segment, ea, groupIndex, slot,
			info, area, NULL, NULL);
		return;
	}

	VMCache* cache = page->Cache();
	if (cache == NULL) {
		audit_report(state, "NO-CACHE", segment, ea, groupIndex, slot, info,
			area, page, NULL);
	}

	if (area->wiring != B_NO_LOCK) {
		// map_page() counts every wired-area mapping, so zero means lost
		if (page->WiredCount() == 0) {
			audit_report(state, "WIRED0", segment, ea, groupIndex, slot, info,
				area, page, NULL);
		}
	} else if (!audit_has_mapping(page, area, state.kdlMode)) {
		audit_report(state, "NO-MAPPING", segment, ea, groupIndex, slot, info,
			area, page, NULL);
	}

	if (cache == NULL || area->cache == NULL)
		return;

	if (cache != area->cache) {
		// a stacked cache is normal for copy-on-write, not for a wired area
		if (area->wiring != B_NO_LOCK) {
			audit_report(state, "CACHE-MISMATCH", segment, ea, groupIndex,
				slot, info, area, page, NULL);
		}
		return;
	}

	off_t expected = (area->cache_offset + (off_t)(ea - area->Base())) >> 12;
	if ((off_t)page->cache_offset != expected) {
		snprintf(sExtra, sizeof(sExtra), "expected coff %lu",
			(unsigned long)expected);
		audit_report(state, "OFFSET-MISMATCH", segment, ea, groupIndex, slot,
			info, area, page, sExtra);
	}
}


static void
audit_sweep(bool kdlMode, uint32 maxLines)
{
	PPCPagingMethodClassic* method = PPCPagingMethodClassic::Method();
	if (method == NULL)
		return;

	page_table_entry_group* table = method->PageTable();
	uint32 hashMask = method->PageTableHashMask();
	VMAddressSpace* kernelSpace = VMAddressSpace::Kernel();
	if (table == NULL || hashMask == 0 || kernelSpace == NULL)
		return;

	sweep_state state;
	memset(&state, 0, sizeof(state));
	state.maxLines = maxLines;
	state.kdlMode = kdlMode;

	audit_read_kernel_segments();

	bigtime_t start = system_time();
	sSweepNumber++;

	// a diagnostic sweep, so no locks in KDL and torn reads are tolerated
	if (!kdlMode)
		kernelSpace->ReadLock();

	VMArea* lastArea = NULL;
	uint32 groupCount = hashMask + 1;

	for (uint32 group = 0; group < groupCount; group++) {
		for (int slot = 0; slot < 8; slot++) {
			const volatile uint32* raw
				= (const volatile uint32*)&table[group].entry[slot];

			// the cheapest possible test first: almost every entry is invalid
			uint32 high = raw[0];
			if ((high & AUDIT_PTE_VALID) == 0)
				continue;

			uint32 low = raw[1];
			if (raw[0] != high)
				continue;

			state.entries++;

			pte_info info;
			audit_decode(group, hashMask, high, low, info);

			// a user VSID has no segment here, so its address stays segment-relative
			int segment = audit_kernel_segment(info.vsid);
			addr_t ea = ((addr_t)(segment > 0 ? segment : 0) << 28)
				| ((addr_t)info.pageIndex << 12);

			if (info.apiMismatch) {
				audit_report(state, "BAD-API", segment, ea, group, slot, info,
					NULL, NULL, NULL);
			}

			if (!audit_rpn_is_sane(info.rpn)) {
				audit_report(state, "BAD-RPN", segment, ea, group, slot, info,
					NULL, NULL, NULL);
			}

			uint32 dupGroup;
			int dupSlot;
			if (audit_find_duplicate(table, hashMask, group, slot, info,
					&dupGroup, &dupSlot)) {
				snprintf(sExtra, sizeof(sExtra), "duplicate at grp %#lx.%d",
					(unsigned long)dupGroup, dupSlot);
				audit_report(state, "DUP-PTE", segment, ea, group, slot, info,
					NULL, NULL, sExtra);
			}

			if (segment < 0)
				continue;

			state.kernelEntries++;
			audit_check_kernel_entry(state, kernelSpace, &lastArea, segment,
				ea, group, slot, info);
		}
	}

	if (!kdlMode)
		kernelSpace->ReadUnlock();

	bigtime_t duration = system_time() - start;

	AUDIT_PRINT(state, AUDIT_PREFIX "sweep %lu: %lu entries, %lu kernel, "
		"%lu findings (%lu printed, %lu repeats, +%lu more), %lu in loader "
		"windows, %lld us\n", (unsigned long)sSweepNumber,
		(unsigned long)state.entries, (unsigned long)state.kernelEntries,
		(unsigned long)state.findings, (unsigned long)state.printed,
		(unsigned long)state.repeats, (unsigned long)state.dropped,
		(unsigned long)state.loaderWindow, (long long)duration);

	if (state.loaderWindow != sLastLoaderWindow) {
		sLastLoaderWindow = state.loaderWindow;
		AUDIT_PRINT(state, AUDIT_PREFIX "loader-window entries changed: %lu "
			"(first ea %#lx)\n", (unsigned long)state.loaderWindow,
			(unsigned long)state.loaderWindowFirst);
	}
}


static void
audit_daemon(void* /*arg*/, int /*iteration*/)
{
	if (sSweepsToSkip > 0) {
		sSweepsToSkip--;
		return;
	}

	audit_sweep(false, AUDIT_MAX_LINES);

	if (sSweepNumber >= AUDIT_FAST_SWEEPS)
		sSweepsToSkip = AUDIT_SLOW_SKIP;
}


static int
audit_debugger_command(int argc, char** argv)
{
	uint32 maxLines = AUDIT_MAX_LINES;
	if (argc > 1)
		maxLines = strtoul(argv[1], NULL, 0);

	audit_sweep(true, maxLines);
	return 0;
}


/*!	Registers the sweep, called repeatedly until the kernel has finished booting.

	Neither the kernel daemon nor the debugger command list exists while
	gKernelStartup is set, so registration waits for the first address space
	created after that - which is the first team, i.e. the start of userland.
*/
void
ppc_pte_audit_init()
{
	// two teams can be created at once, so claim the registration atomically
	if (gKernelStartup || atomic_test_and_set(&sInitialized, 1, 0) != 0)
		return;

	add_debugger_command("pteaudit", &audit_debugger_command,
		"Audit the classic PPC page table for stale/duplicate/unaccounted "
		"entries");

	if (register_kernel_daemon(&audit_daemon, NULL, AUDIT_DAEMON_FREQUENCY)
			!= B_OK) {
		dprintf(AUDIT_PREFIX "could not register the sweep daemon\n");
	}
}
