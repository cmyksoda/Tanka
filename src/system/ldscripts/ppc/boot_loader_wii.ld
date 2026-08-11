OUTPUT_FORMAT("elf32-powerpc", "elf32-powerpc", "elf32-powerpc")
OUTPUT_ARCH(powerpc)

ENTRY(_start)

SECTIONS
{
	/* Reserve room for the ELF headers inside the first segment. elf2dol emits
	   a DOL section starting at the segment address, so without this the
	   headers would be written below BOOT_LOADER_BASE, on top of the low MEM1
	   system globals and libogc's exception vectors. */
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

/* libogc is built for the EABI and addresses .sdata/.sdata2 through r13/r2.
   _start loads these two bases; nothing may be linked before them. */
PROVIDE(_SDA_BASE_ = __sdata_start + 0x8000);
PROVIDE(_SDA2_BASE_ = __sdata2_start + 0x8000);

/* libogc reads these as absolute addresses, not as variables - devkitPPC's
   rvl.ld/libogc_common.ld define them the same way. Arena1 is what is left of
   MEM1 above the loader; the loader itself sits in MEM1 low with the kernel's
   own image, so arena1 is capped well below the top of MEM1 and the rest is
   handed to the kernel by mmu.cpp. */
PROVIDE(__isIPL = 0);
PROVIDE(__ppc_excpt_buf = 0x800000d0);
PROVIDE(__ppc_excpt_table = 0x80003000);
PROVIDE(__Arena1Lo = (_end + 0x1f) & ~ 0x1f);
PROVIDE(__Arena1Hi = 0x81800000);
PROVIDE(__Arena2Lo = 0x90002000);
PROVIDE(__Arena2Hi = 0x933e0000);
PROVIDE(__ipcbufferLo = 0x933e0000);
PROVIDE(__ipcbufferHi = 0x93400000);
