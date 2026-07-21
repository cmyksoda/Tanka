/*
 * Copyright 2003-2006, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "runtime_loader_private.h"

#include <runtime_loader.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>


//#define TRACE_RLD
#ifdef TRACE_RLD
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


// PowerPC I/D caches are not coherent, so a relocation patched into what will
// be executed as code (the fabricated PLT "b <addr>" below) must be flushed
// from the data cache and invalidated in the instruction cache before it is
// fetched. dcbst/icbi are unprivileged, so this works in userland.
#define ELF_RELOC_CACHELINE 32

static inline void
sync_icache_for_relocation(addr_t address, size_t len)
{
	int off = (unsigned int)address & (ELF_RELOC_CACHELINE - 1);
	int remaining = (int)(len + off);

	char* p = (char*)address - off;
	do {
		asm volatile("dcbst 0,%0" :: "r"(p));
		p += ELF_RELOC_CACHELINE;
	} while ((remaining -= ELF_RELOC_CACHELINE) > 0);
	asm volatile("sync");

	remaining = (int)(len + off);
	p = (char*)address - off;
	do {
		asm volatile("icbi 0,%0" :: "r"(p));
		p += ELF_RELOC_CACHELINE;
	} while ((remaining -= ELF_RELOC_CACHELINE) > 0);
	asm volatile("sync");
	asm volatile("isync");
}


static inline void
write_word32(addr_t P, Elf32_Word value)
{
	*(Elf32_Word*)P = value;
}


static inline bool
write_low24_check(addr_t P, Elf32_Word value)
{
	if ((value & 0x3f000000) && (~value & 0x3f800000))
		return false;
	*(Elf32_Word*)P = (*(Elf32_Word*)P & 0xfc000003)
		| ((value & 0x00ffffff) << 2);
	return true;
}


static inline void
write_half16(addr_t P, Elf32_Word value)
{
	*(Elf32_Half*)P = (Elf32_Half)value;
}


static inline bool
write_half16_check(addr_t P, Elf32_Word value)
{
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


static int
relocate_rela(image_t* rootImage, image_t* image, Elf32_Rela* rel,
	size_t relLength, SymbolLookupCache* cache)
{
	for (size_t i = 0; i < relLength / sizeof(Elf32_Rela); i++) {
		int type = ELF32_R_TYPE(rel[i].r_info);
		int symIndex = ELF32_R_SYM(rel[i].r_info);

		addr_t S = 0;				// symbol value
		image_t* symbolImage = NULL;

		// Resolve the symbol, if the relocation needs one.
		switch (type) {
			case R_PPC_ADDR32:
			case R_PPC_ADDR24:
			case R_PPC_ADDR16:
			case R_PPC_ADDR16_LO:
			case R_PPC_ADDR16_HI:
			case R_PPC_ADDR16_HA:
			case R_PPC_ADDR14:
			case R_PPC_REL24:
			case R_PPC_REL14:
			case R_PPC_REL32:
			case R_PPC_GLOB_DAT:
			case R_PPC_UADDR32:
			case R_PPC_UADDR16:
			case R_PPC_JMP_SLOT:
				if (symIndex != 0) {
					elf_sym* sym = SYMBOL(image, symIndex);
					status_t status = resolve_symbol(rootImage, image, sym,
						cache, &S, &symbolImage);
					if (status != B_OK) {
						TRACE(("resolve symbol \"%s\" returned: %" B_PRId32 "\n",
							SYMNAME(image, sym), status));
						printf("resolve symbol \"%s\" returned: %" B_PRId32 "\n",
							SYMNAME(image, sym), status);
						return status;
					}
				}
				break;
		}

		addr_t P = (addr_t)(image->regions[0].delta + rel[i].r_offset);
		addr_t A = (addr_t)rel[i].r_addend;
		addr_t B = (addr_t)image->regions[0].delta;

		switch (type) {
			case R_PPC_NONE:
				break;

			case R_PPC_ADDR32:
			case R_PPC_GLOB_DAT:
			case R_PPC_UADDR32:
				write_word32(P, S + A);
				break;

			case R_PPC_ADDR16:
			case R_PPC_UADDR16:
				if (!write_half16_check(P, S + A)) {
					printf("R_PPC_ADDR16 overflow\n");
					return B_BAD_DATA;
				}
				break;

			case R_PPC_ADDR16_LO:
				write_half16(P, lo(S + A));
				break;

			case R_PPC_ADDR16_HI:
				write_half16(P, hi(S + A));
				break;

			case R_PPC_ADDR16_HA:
				write_half16(P, ha(S + A));
				break;

			case R_PPC_ADDR24:
				if (!write_low24_check(P, (S + A) >> 2)) {
					printf("R_PPC_ADDR24 overflow\n");
					return B_BAD_DATA;
				}
				break;

			case R_PPC_REL24:
				if (!write_low24_check(P, (S + A - P) >> 2)) {
					printf("R_PPC_REL24 overflow\n");
					return B_BAD_DATA;
				}
				break;

			case R_PPC_REL32:
				write_word32(P, S + A - P);
				break;

			case R_PPC_RELATIVE:
				write_word32(P, B + A);
				break;

			case R_PPC_JMP_SLOT:
			{
				// The PLT slot is 8 bytes/two instruction words. If the target
				// is within a signed 26-bit branch displacement we can drop a
				// single "b <target>" into the slot; images are loaded compactly
				// enough that this always holds in practice. (The kernel handles
				// the >24-bit case with a reserved trampoline pool; user images
				// have no such pool - fail loudly rather than silently.)
				addr_t jumpOffset = S - P;
				if ((jumpOffset & 0xfc000000) != 0
					&& (~jumpOffset & 0xfe000000) != 0) {
					printf("R_PPC_JMP_SLOT: target too far for a single branch "
						"(offset %p)\n", (void*)jumpOffset);
					return B_NOT_SUPPORTED;
				}
				*(uint32*)P = 0x48000000 | (jumpOffset & 0x03fffffc);
				sync_icache_for_relocation(P, sizeof(uint32));
				break;
			}

			default:
				printf("arch_relocate_image: unhandled relocation type %d\n",
					type);
				return B_BAD_DATA;
		}
	}

	return B_OK;
}


status_t
arch_relocate_image(image_t* rootImage, image_t* image,
	SymbolLookupCache* cache)
{
	status_t status;

	// PowerPC uses RELA relocations exclusively (no REL).
	if (image->rela) {
		status = relocate_rela(rootImage, image, image->rela, image->rela_len,
			cache);
		if (status != B_OK)
			return status;
	}

	// The PLT relocations are RELA too (.rela.plt).
	if (image->pltrel) {
		status = relocate_rela(rootImage, image, (Elf32_Rela*)image->pltrel,
			image->pltrel_len, cache);
		if (status != B_OK)
			return status;
	}

	return B_OK;
}
