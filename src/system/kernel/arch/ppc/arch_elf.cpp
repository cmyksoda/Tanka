/*
 * Copyright 2005, Ingo Weinhold <bonefish@cs.tu-berlin.de>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 *
 * Copyright 2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */

#ifdef _BOOT_MODE
#include <boot/arch.h>
#endif

#include <KernelExport.h>

#include <elf_priv.h>
#include <arch/elf.h>


#define CHATTY 0

#ifdef _BOOT_MODE
status_t
boot_arch_elf_relocate_rel(struct preloaded_elf32_image *image, Elf32_Rel *rel,
	int rel_len)
#else
int
arch_elf_relocate_rel(struct elf_image_info *image,
	struct elf_image_info *resolve_image, Elf32_Rel *rel, int rel_len)
#endif
{
	// there are no rel entries in PPC elf
	return B_NO_ERROR;
}


static inline void
write_word32(addr_t P, Elf32_Word value)
{
	*(Elf32_Word*)P = value;
}


// PowerPC I/D caches are not coherent: a plain store through the data path
// (as used to patch relocations into already-loaded code, e.g. the PLT
// "b <addr>" stubs written for R_PPC_JMP_SLOT below) is not guaranteed to
// be visible to subsequent instruction fetches until the modified line is
// flushed from the data cache and the corresponding instruction cache line
// is explicitly invalidated. See the PowerPC Programming Environments
// Manual, "Ensuring the Coherency of Memory and I-Cache" / self-modifying
// code sequence. Mirrors arch_cpu_sync_icache() in arch_cpu.cpp, but is
// self-contained since this file is also compiled into the boot loader
// under _BOOT_MODE, where arch_cpu.cpp is not available.
#define ELF_RELOC_CACHELINE 32

static inline void
sync_icache_for_relocation(addr_t address, size_t len)
{
	int off = (unsigned int)address & (ELF_RELOC_CACHELINE - 1);
	int remaining = (int)(len + off);

	char* p = (char*)address - off;
	do {
		asm volatile ("dcbst 0,%0" :: "r"(p));
		p += ELF_RELOC_CACHELINE;
	} while ((remaining -= ELF_RELOC_CACHELINE) > 0);
	asm volatile ("sync");

	remaining = (int)(len + off);
	p = (char*)address - off;
	do {
		asm volatile ("icbi 0,%0" :: "r"(p));
		p += ELF_RELOC_CACHELINE;
	} while ((remaining -= ELF_RELOC_CACHELINE) > 0);
	asm volatile ("sync");
	asm volatile ("isync");
}


static inline void
write_word30(addr_t P, Elf32_Word value)
{
	// bits 0:29
	*(Elf32_Word*)P = (*(Elf32_Word*)P & 0x3) | (value << 2);
}


static inline bool
write_low24_check(addr_t P, Elf32_Word value)
{
	// bits 6:29
	if ((value & 0x3f000000) && (~value & 0x3f800000))
		return false;
	*(Elf32_Word*)P = (*(Elf32_Word*)P & 0xfc000003)
		| ((value & 0x00ffffff) << 2);
	return true;
}


static inline bool
write_low14_check(addr_t P, Elf32_Word value)
{
	// bits 16:29
	if ((value & 0x3fffc000) && (~value & 0x3fffe000))
		return false;
	*(Elf32_Word*)P = (*(Elf32_Word*)P & 0xffff0003)
		| ((value & 0x00003fff) << 2);
	return true;
}


static inline void
write_half16(addr_t P, Elf32_Word value)
{
	// bits 16:29
	*(Elf32_Half*)P = (Elf32_Half)value;
}


static inline bool
write_half16_check(addr_t P, Elf32_Word value)
{
	// bits 16:29
	if ((value & 0xffff0000) && (~value & 0xffff8000))
		return false;
	*(Elf32_Half*)P = (Elf32_Half)value;
	return true;
}


static inline Elf32_Word
lo(Elf32_Word value)
{
	return (value & 0xffff);
}


static inline Elf32_Word
hi(Elf32_Word value)
{
	return ((value >> 16) & 0xffff);
}


static inline Elf32_Word
ha(Elf32_Word value)
{
	return (((value >> 16) + (value & 0x8000 ? 1 : 0)) & 0xffff);
}


// R_PPC_JMP_SLOT relocations whose target is more than 24 bits away from
// the PLT slot can't be reached with a single fabricated "b <addr>"
// instruction (the only thing that fits in a slot's 8 bytes/2 instruction
// words - confirmed via readelf: consecutive .rela.plt r_offset values are
// exactly 8 bytes apart). Route those through a small pool of "long jump
// island" trampolines instead: a linker script that wants to opt into this
// (currently just src/system/ldscripts/ppc/kernel.ld) reserves
// PLT_TRAMPOLINE_POOL_SIZE bytes immediately before the linker's own
// auto-generated .plt content, with a magic sentinel word at the very
// start so this code can verify the reservation is actually present
// before trusting the space - any image whose linker script doesn't
// reserve it (the sentinel simply won't match, since that memory is
// whatever real content preceded .plt in that image) safely falls back to
// the old skip-only behavior instead of corrupting unrelated code. Each
// trampoline is a full 4-instruction absolute jump ("lis/ori/mtctr/bctr",
// the standard way to reach an arbitrary 32-bit address on ppc32 - there's
// no single instruction that loads a full 32-bit immediate, and no branch
// form, relative or absolute, that encodes one either) - the PLT slot
// itself only ever needs a plain "b" to its assigned trampoline, which is
// guaranteed in-range since the pool lives inside the same image, well
// under the 24-bit branch range away.
#define PLT_TRAMPOLINE_POOL_MAGIC	0x504c5442	// "PLTB"
#define PLT_TRAMPOLINE_COUNT		256
#define PLT_TRAMPOLINE_SIZE		16	// 4 instructions
#define PLT_TRAMPOLINE_POOL_HEADER_SIZE	4	// the magic sentinel word
#define PLT_TRAMPOLINE_POOL_SIZE \
	(PLT_TRAMPOLINE_POOL_HEADER_SIZE + PLT_TRAMPOLINE_COUNT * PLT_TRAMPOLINE_SIZE)


static addr_t
find_plt_trampoline_pool(addr_t nearAddress)
{
	// The linker's own auto-generated .plt content is preceded both by
	// kernel.ld's explicit trampoline-pool reservation and by ld's own
	// internal PPC PLT0-style header (observed to add an extra 72-byte
	// gap with the toolchain this was built against, but that's an
	// implementation detail of the binutils PPC backend, not guaranteed
	// stable across versions, so it's not worth hardcoding). Search
	// backward for the magic sentinel instead of assuming a fixed gap.
	// nearAddress can be any PLT slot's own address P, not just the first
	// one - R_PPC_JMP_SLOT relocations turn up in both .rela.plt and the
	// general .rela table (confirmed at runtime: a handful of JMP_SLOT
	// entries appear mixed into the much larger .rela table alongside
	// R_PPC_RELATIVE and friends) - so the search needs to cover the
	// entire .plt section's size (~22 KB observed for this kernel, mostly
	// driven by relocation count), not just the small gap right before
	// the trampoline pool itself. 128 KB is a generous upper bound.
	addr_t address = nearAddress & ~(addr_t)3;
	for (int32 i = 0; i < 0x20000 / 4; i++) {
		if (*(uint32*)address == PLT_TRAMPOLINE_POOL_MAGIC)
			return address;
		address -= 4;
	}
	return 0;
}


#ifdef _BOOT_MODE
status_t
boot_arch_elf_relocate_rela(struct preloaded_elf32_image *image,
	Elf32_Rela *rel, int rel_len)
#else
int
arch_elf_relocate_rela(struct elf_image_info *image,
	struct elf_image_info *resolve_image, Elf32_Rela *rel, int rel_len)
#endif
{
	int i;
	Elf32_Sym *sym;
	int vlErr;

	Elf32_Addr S = 0;	// symbol address
	addr_t R = 0;		// section relative symbol address

	addr_t G = 0;		// GOT address
	addr_t L = 0;		// PLT address

	int32 trampolinesUsed = 0;
		// see the PLT_TRAMPOLINE_* comment above ha()

	#define P	((addr_t)(image->text_region.delta + rel[i].r_offset))
	#define A	((addr_t)rel[i].r_addend)
	#define B	(image->text_region.delta)

	// TODO: Get the GOT address!
	#define REQUIRE_GOT	\
		if (G == 0) {	\
			dprintf("arch_elf_relocate_rela(): Failed to get GOT address!\n"); \
			return B_ERROR;	\
		}

	// TODO: Get the PLT address!
	#define REQUIRE_PLT	\
		if (L == 0) {	\
			dprintf("arch_elf_relocate_rela(): Failed to get PLT address!\n"); \
			return B_ERROR;	\
		}

	for (i = 0; i * (int)sizeof(Elf32_Rela) < rel_len; i++) {
#if CHATTY
		dprintf("looking at rel type %d, offset 0x%lx, sym 0x%lx, addend 0x%lx\n",
			ELF32_R_TYPE(rel[i].r_info), rel[i].r_offset, ELF32_R_SYM(rel[i].r_info), rel[i].r_addend);
#endif
		switch (ELF32_R_TYPE(rel[i].r_info)) {
			case R_PPC_SECTOFF:
			case R_PPC_SECTOFF_LO:
			case R_PPC_SECTOFF_HI:
			case R_PPC_SECTOFF_HA:
				dprintf("arch_elf_relocate_rela(): Getting section relative "
					"symbol addresses not yet supported!\n");
				return B_ERROR;

			case R_PPC_ADDR32:
			case R_PPC_ADDR24:
			case R_PPC_ADDR16:
			case R_PPC_ADDR16_LO:
			case R_PPC_ADDR16_HI:
			case R_PPC_ADDR16_HA:
			case R_PPC_ADDR14:
			case R_PPC_ADDR14_BRTAKEN:
			case R_PPC_ADDR14_BRNTAKEN:
			case R_PPC_REL24:
			case R_PPC_REL14:
			case R_PPC_REL14_BRTAKEN:
			case R_PPC_REL14_BRNTAKEN:
			case R_PPC_GLOB_DAT:
			case R_PPC_UADDR32:
			case R_PPC_UADDR16:
			case R_PPC_REL32:
			case R_PPC_SDAREL16:
			case R_PPC_ADDR30:
			case R_PPC_JMP_SLOT:
				sym = SYMBOL(image, ELF32_R_SYM(rel[i].r_info));

#ifdef _BOOT_MODE
				vlErr = boot_elf_resolve_symbol(image, sym, &S);
#else
				vlErr = elf_resolve_symbol(image, sym, resolve_image, &S);
#endif
				if (vlErr < 0) {
					dprintf("%s(): Failed to relocate "
						"entry index %d, rel type %d, offset 0x%lx, sym 0x%lx, "
						"addend 0x%lx\n", __FUNCTION__, i, ELF32_R_TYPE(rel[i].r_info),
						rel[i].r_offset, ELF32_R_SYM(rel[i].r_info),
						rel[i].r_addend);
					return vlErr;
				}
				break;
		}

		switch (ELF32_R_TYPE(rel[i].r_info)) {
			case R_PPC_NONE:
				break;

			case R_PPC_COPY:
				// TODO: Implement!
				dprintf("arch_elf_relocate_rela(): R_PPC_COPY not yet "
					"supported!\n");
				return B_ERROR;

			case R_PPC_ADDR32:
			case R_PPC_GLOB_DAT:
			case R_PPC_UADDR32:
				write_word32(P, S + A);
				break;

			case R_PPC_ADDR24:
				if (write_low24_check(P, (S + A) >> 2))
					break;
dprintf("R_PPC_ADDR24 overflow\n");
				return B_BAD_DATA;

			case R_PPC_ADDR16:
			case R_PPC_UADDR16:
				if (write_half16_check(P, S + A))
					break;
dprintf("R_PPC_ADDR16 overflow\n");
				return B_BAD_DATA;

			case R_PPC_ADDR16_LO:
				write_half16(P, lo(S + A));
				break;

			case R_PPC_ADDR16_HI:
				write_half16(P, hi(S + A));
				break;

			case R_PPC_ADDR16_HA:
				write_half16(P, ha(S + A));
				break;

			case R_PPC_ADDR14:
			case R_PPC_ADDR14_BRTAKEN:
			case R_PPC_ADDR14_BRNTAKEN:
				if (write_low14_check(P, (S + A) >> 2))
					break;
dprintf("R_PPC_ADDR14 overflow\n");
				return B_BAD_DATA;

			case R_PPC_REL24:
				if (write_low24_check(P, (S + A - P) >> 2))
					break;
dprintf("R_PPC_REL24 overflow: 0x%lx\n", (S + A - P) >> 2);
				return B_BAD_DATA;

			case R_PPC_REL14:
			case R_PPC_REL14_BRTAKEN:
			case R_PPC_REL14_BRNTAKEN:
				if (write_low14_check(P, (S + A - P) >> 2))
					break;
dprintf("R_PPC_REL14 overflow\n");
				return B_BAD_DATA;

			case R_PPC_GOT16:
				REQUIRE_GOT;
				if (write_half16_check(P, G + A))
					break;
dprintf("R_PPC_GOT16 overflow\n");
				return B_BAD_DATA;

			case R_PPC_GOT16_LO:
				REQUIRE_GOT;
				write_half16(P, lo(G + A));
				break;

			case R_PPC_GOT16_HI:
				REQUIRE_GOT;
				write_half16(P, hi(G + A));
				break;

			case R_PPC_GOT16_HA:
				REQUIRE_GOT;
				write_half16(P, ha(G + A));
				break;

			case R_PPC_JMP_SLOT:
			{
				// If the relative offset is small enough, we fabricate a
				// relative branch instruction ("b <addr>").
				addr_t jumpOffset = S - P;
				if ((jumpOffset & 0xfc000000) != 0
					&& (~jumpOffset & 0xfe000000) != 0) {
					// Offset > 24 bit - see the PLT_TRAMPOLINE_* comment
					// above ha() for the full explanation. This also
					// replaces what used to be a "return B_ERROR" here,
					// which propagated straight out of
					// ELFLoader<Class>::Relocate() / elf_relocate() and
					// skipped ALL remaining relocations for this image -
					// including the entire subsequent image->rela table
					// (R_PPC_RELATIVE and friends) - producing exactly the
					// "garbage function pointer / self-corrupting
					// exception vector" crash signature chased for most of
					// this debugging session.
					addr_t pltTrampolinePool = find_plt_trampoline_pool(P);
					if (pltTrampolinePool != 0
						&& trampolinesUsed < PLT_TRAMPOLINE_COUNT) {
						addr_t island = pltTrampolinePool
							+ PLT_TRAMPOLINE_POOL_HEADER_SIZE
							+ trampolinesUsed * PLT_TRAMPOLINE_SIZE;
						trampolinesUsed++;

						uint32* code = (uint32*)island;
						// Use hi() (plain high 16 bits), NOT ha(): the second
						// instruction is ori (zero-extending), not a
						// sign-extending addi. ha() adds 1 to the high half
						// whenever lo(S) has bit 15 set, to compensate for
						// addi's sign extension - but ori doesn't sign-extend,
						// so pairing ha() with ori overshoots the target by
						// 0x10000 for every symbol whose low half is >= 0x8000
						// (roughly half of them). That sent trampolined calls
						// 64 KB past their target (e.g. a module's get_module
						// call landed in generate_topology_array).
						code[0] = 0x3c000000 | hi(S);	// lis   r0, S@hi
						code[1] = 0x60000000 | lo(S);	// ori   r0, r0, S@l
						code[2] = 0x7c0903a6;			// mtctr r0
						code[3] = 0x4e800420;			// bctr
						sync_icache_for_relocation(island, 4 * sizeof(uint32));

						addr_t stubOffset = island - P;
						*(uint32*)P = 0x48000000 | (stubOffset & 0x03fffffc);
						sync_icache_for_relocation(P, sizeof(uint32));
					} else {
						dprintf("arch_elf_relocate_rela(): R_PPC_JMP_SLOT: "
							"Offsets > 24 bit and no trampoline pool "
							"available for this image! Skipping this slot "
							"only.\n");
dprintf("jumpOffset: %p\n", (void*)jumpOffset);
					}
					break;
				} else {
					// Offset <= 24 bit
					// 0:5 opcode (= 18), 6:29 address, 30 AA, 31 LK
					// "b" instruction: opcode = 18, AA = 0, LK = 0
					// address: 24 high-order bits of 26 bit offset
					*(uint32*)P = 0x48000000 | ((jumpOffset) & 0x03fffffc);
					sync_icache_for_relocation(P, sizeof(uint32));
				}
				break;
			}

			case R_PPC_RELATIVE:
				write_word32(P, B + A);
				break;

			case R_PPC_LOCAL24PC:
// TODO: Implement!
// low24*
// 				if (write_low24_check(P, ?)
// 					break;
// 				return B_BAD_DATA;
				dprintf("arch_elf_relocate_rela(): R_PPC_LOCAL24PC not yet "
					"supported!\n");
				return B_ERROR;

			case R_PPC_REL32:
				write_word32(P, S + A - P);
				break;

			case R_PPC_PLTREL24:
				REQUIRE_PLT;
				if (write_low24_check(P, (L + A - P) >> 2))
					break;
dprintf("R_PPC_PLTREL24 overflow\n");
				return B_BAD_DATA;

			case R_PPC_PLT32:
				REQUIRE_PLT;
				write_word32(P, L + A);
				break;

			case R_PPC_PLTREL32:
				REQUIRE_PLT;
				write_word32(P, L + A - P);
				break;

			case R_PPC_PLT16_LO:
				REQUIRE_PLT;
				write_half16(P, lo(L + A));
				break;

			case R_PPC_PLT16_HI:
				REQUIRE_PLT;
				write_half16(P, hi(L + A));
				break;

			case R_PPC_PLT16_HA:
				write_half16(P, ha(L + A));
				break;

			case R_PPC_SDAREL16:
// TODO: Implement!
// 				if (write_half16_check(P, S + A - _SDA_BASE_))
// 					break;
// 				return B_BAD_DATA;
				dprintf("arch_elf_relocate_rela(): R_PPC_SDAREL16 not yet "
					"supported!\n");
				return B_ERROR;

			case R_PPC_SECTOFF:
				if (write_half16_check(P, R + A))
					break;
dprintf("R_PPC_SECTOFF overflow\n");
				return B_BAD_DATA;

			case R_PPC_SECTOFF_LO:
				write_half16(P, lo(R + A));
				break;

			case R_PPC_SECTOFF_HI:
				write_half16(P, hi(R + A));
				break;

			case R_PPC_SECTOFF_HA:
				write_half16(P, ha(R + A));
				break;

			case R_PPC_ADDR30:
				write_word30(P, (S + A - P) >> 2);
				break;

			default:
				dprintf("arch_elf_relocate_rela: unhandled relocation type %d\n", ELF32_R_TYPE(rel[i].r_info));
				return B_ERROR;
		}
	}

	return B_NO_ERROR;
}

