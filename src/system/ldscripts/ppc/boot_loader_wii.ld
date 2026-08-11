OUTPUT_FORMAT("elf32-powerpc", "elf32-powerpc", "elf32-powerpc")
OUTPUT_ARCH(powerpc)

ENTRY(_start)

SECTIONS
{
	/* room for the ELF headers, so elf2dol does not emit below BOOT_LOADER_BASE */
	. = BOOT_LOADER_BASE + SIZEOF_HEADERS;

	__text_begin = .;

	/* text/read-only data */
	.text :	{ *(.text .text.* .gnu.linkonce.t.*)
	          *(.crt0.* )
	          *(.rodata .rodata.* .gnu.linkonce.r.*) }

	__sdata2_start = .;
	.sdata2 : {
		*(.sdata2 .sdata2.* .gnu.linkonce.s2.*)
	}

	.sbss2 : {
		*(.sbss2 .sbss2.* .gnu.linkonce.sb2.*)
	}

	/* writable data  */
	/* align to the same offset in the next page (for performance reasons
	   (not that it really matters in the boot loader)) */
	. = ALIGN(0x1000) + 0x1000 + (. & (0x1000 - 1));

	.data : {
		__ctor_list = .;
		*(.ctors)
		__ctor_end = .;

		__data_start = .;
		*(.data .gnu.linkonce.d.*)
		*(.data.rel.ro.local .data.rel.ro*)
		*(.got .got2)
	}

	__sdata_start = .;
	.sdata : {
		*(.sdata .sdata.* .gnu.linkonce.s.* .fixup)
	}

	/* uninitialized data (in same segment as writable data) */
	__bss_start = .;

	__sbss_start = .;
	.sbss : {
		*(.sbss .sbss.* .gnu.linkonce.sb.*)
	}

	.bss : {
		*(.bss .bss.* .gnu.linkonce.b.*)
		. = ALIGN(0x1000);
	}
	__bss_end = .;

	_end = . ;

	/* Strip unnecessary stuff */
	/DISCARD/ : { *(.comment .note .eh_frame .dtors .debug_* .gnu.attributes) }
}

/* libogc addresses .sdata/.sdata2 off r13/r2; entry.S loads both bases */
PROVIDE(_SDA_BASE_ = __sdata_start + 0x8000);
PROVIDE(_SDA2_BASE_ = __sdata2_start + 0x8000);

/* libogc reads these as absolute addresses, like devkitPPC's own rvl.ld */
PROVIDE(__isIPL = 0);
PROVIDE(__ppc_excpt_buf = 0x800000d0);
PROVIDE(__ppc_excpt_table = 0x80003000);
PROVIDE(__Arena1Lo = (_end + 0x1f) & ~ 0x1f);
PROVIDE(__Arena1Hi = 0x81800000);
PROVIDE(__Arena2Lo = 0x90002000);
PROVIDE(__Arena2Hi = 0x933e0000);
PROVIDE(__ipcbufferLo = 0x933e0000);
PROVIDE(__ipcbufferHi = 0x93400000);
