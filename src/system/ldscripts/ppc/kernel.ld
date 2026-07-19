OUTPUT_FORMAT("elf32-powerpc", "elf32-powerpc", "elf32-powerpc")
OUTPUT_ARCH(powerpc)

ENTRY(_start)
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
	. = 0x80000000 + SIZEOF_HEADERS;

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
	.plt : { *(.plt) }

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

	/* Debug  */
	INCLUDE debuginfo.ld

	/* Strip unnecessary stuff */
	/DISCARD/ : { *(.comment .note .eh_frame .dtors .debug_*) }
}
