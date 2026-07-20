/* Linker script for ppc kernel add-ons. Based on kernel.ld, but relocatable
   (no fixed base address) and with no _start entry point. Its purpose,
   beyond matching kernel.ld's proven section layout, is twofold:
     - an explicit PHDRS block that splits text (RX) and data (RW) into two
       separate PT_LOAD segments. Modern binutils otherwise merges everything
       into a single RWE segment for -shared ppc add-ons, which the boot
       loader's ELF loader and the kernel's own add-on loader then have to
       special-case (see the merged-segment handling in
       src/system/boot/loader/elf.cpp and src/system/kernel/vm/vm_init.cpp).
     - the PLT trampoline pool reservation (see .plt below). Add-ons load far
       from the kernel (~256 MB away), so their R_PPC_JMP_SLOT relocations to
       kernel functions are out of 24-bit branch range; without a trampoline
       pool arch_elf_relocate_rela() can only skip them, and calling such a
       slot executes garbage. */
OUTPUT_FORMAT("elf32-powerpc", "elf32-powerpc", "elf32-powerpc")
OUTPUT_ARCH(powerpc)

SEARCH_DIR("libgcc");

PHDRS
{
	headers PT_PHDR PHDRS ;
	text PT_LOAD FILEHDR PHDRS FLAGS(5) ;
	data PT_LOAD ;
	dynamic PT_DYNAMIC ;
}

SECTIONS
{
	. = SIZEOF_HEADERS;

	.interp : { *(.interp) } : text
	.hash : { *(.hash) }
	.dynsym : { *(.dynsym) }
	.dynstr : { *(.dynstr) }
	.gnu.version : { *(.gnu.version) }
	.gnu.version_d : { *(.gnu.version_d) }
	.gnu.version_r : { *(.gnu.version_r) }

	.rel.dyn : {
		*(.rel.init)
		*(.rel.text .rel.text.* .rel.gnu.linkonce.t*)
		*(.rel.fini)
		*(.rel.rodata .rel.rodata.* .rel.gnu.linkonce.r*)
		*(.rel.data.rel.ro*)
		*(.rel.data .rel.data.* .rel.gnu.linkonce.d.*)
		*(.rel.tdata .rel.tdata.* .rel.gnu.linkonce.td.*)
		*(.rel.ctors)
		*(.rel.dtors)
		*(.rel.got)
		*(.rel.sdata .rel.sdata.* .rel.gnu.linkonce.s.*)
		*(.rel.sbss .rel.sbss.* .rel.gnu.linkonce.sb.*)
		*(.rel.sdata2 .rel.sdata2.* .rel.gnu.linkonce.s2.*)
		*(.rel.sbss2 .rel.sbss2.* .rel.gnu.linkonce.sb2.*)
		*(.rel.bss .rel.bss.* .rel.gnu.linkonce.b.*)
	}

	.rela.dyn : {
		*(.rela.init)
		*(.rela.text .rela.text.* .rela.gnu.linkonce.t*)
		*(.rela.fini)
		*(.rela.rodata .rela.rodata.* .rela.gnu.linkonce.r*)
		*(.rela.data .rela.data.* .rela.gnu.linkonce.d.*)
		*(.rela.tdata .rela.tdata.* .rela.gnu.linkonce.td.*)
		*(.rela.ctors)
		*(.rela.dtors)
		*(.rela.got)
		*(.rela.got1)
		*(.rela.got2)
		*(.rela.sdata .rela.sdata.* .rela.gnu.linkonce.s.*)
		*(.rela.sbss .rela.sbss.* .rela.gnu.linkonce.sb.*)
		*(.rela.sdata2 .rela.sdata2.* .rela.gnu.linkonce.s2.*)
		*(.rela.sbss2 .rela.sbss2.* .rela.gnu.linkonce.sb2.*)
		*(.rela.bss .rela.bss.* .rela.gnu.linkonce.b.*)
	}

	/* Unlike every other architecture's kernel.ld (arm, arm64, m68k,
	   riscv64, sparc, x86, x86_64 - all of which explicitly place .plt and
	   .rela.plt), this script never mentioned .plt at all. GNU ld still
	   emits it (PowerPC PLT stubs, patched in-place at boot time by
	   arch_elf_relocate_rela()'s R_PPC_JMP_SLOT handling), but as an
	   unmentioned "orphan section" ld appends it wherever it likes - which,
	   in practice, landed it entirely past _end, outside the range the
	   boot loader actually maps for the kernel image. Any call resolved
	   through a PLT slot out there branches into unmapped memory and
	   crashes immediately - non-deterministically, since whether a given
	   call happens to go through the PLT (vs. a direct, statically
	   resolved branch) depends on subtle codegen/linking details that
	   differ by call site and build. This is very likely the actual root
	   cause behind most of the "self-corrupting exception vector at a
	   seemingly random point in early boot" crashes chased throughout this
	   investigation. Placing .rela.plt/.plt explicitly - matching every
	   other arch's script - keeps them inside the mapped text segment.
	*/
	.rela.plt : { *(.rela.plt) }

	.init : { *(.init) } =0x60000000

	/* Reserve space for arch_elf_relocate_rela()'s R_PPC_JMP_SLOT
	   "long jump island" trampoline pool (see the PLT_TRAMPOLINE_* comment
	   in src/system/kernel/arch/ppc/arch_elf.cpp above ha()) immediately
	   before the linker's own auto-generated .plt content. The leading
	   magic word lets that code verify at relocation time that this
	   reservation is genuinely present (as opposed to whatever real
	   content would otherwise precede .plt) before writing trampoline
	   code into it - keep PLT_TRAMPOLINE_POOL_MAGIC/_COUNT/_SIZE in sync
	   with the constants there if either ever changes. */
	.plt : {
		__plt_trampolines_start = .;
		LONG(0x504c5442);
		. = __plt_trampolines_start + 4 + 256 * 16;
		*(.plt)
	}

	/* text/read-only data */
	.text :	{
		*(.text .stub .text.* .gnu.linkonce.t.*)
	} =0x60000000

	.rodata : { *(.rodata .rodata.* .gnu.linkonce.r.*) }
	.rodata1 : { *(.rodata1) }
	.sdata2 : { *(.sdata2 .sdata2.* .gnu.linkonce.s2.*) }
	.sbss2 : { *(.sbss2 .sbss2.* .gnu.linkonce.sb2.*) }

	/* writable data  */
	/* align to the same offset in the next page (for performance reasons
	   (not that it really matters in the kernel)) */
	. = ALIGN(0x1000) + 0x1000 + (. & (0x1000 - 1));

	__ctor_list = .;
	.ctors : { *(.ctors) } : data
	__ctor_end = .;

	.dynamic : { *(.dynamic) } :dynamic :data

	__data_start = .;
	.data : { *(.data .gnu.linkonce.d.*) }

	.got : { *(.got.plt) *(.got) }
	
	.sdata : { *(.sdata .sdata.* .gnu.linkonce.s.*) }

	/* uninitialized data (in same segment as writable data) */
	__bss_start = .;
	.sbss : { *(.sbss .sbss.* .gnu.linkonce.sb.*) }
	.bss : {
		*(.bss .bss.* .gnu.linkonce.b.*)
		. = ALIGN(0x1000);
	}

	_end = . ;

	/* Strip unnecessary stuff. (kernel.ld additionally INCLUDEs debuginfo.ld
	   here; add-ons don't - it needs a -L search path that's awkward to pass
	   through the compiler-driven add-on link, and debug sections are just
	   discarded below / left as orphans anyway.) */
	/DISCARD/ : { *(.comment .note .eh_frame .dtors .debug_*) }
}
