/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include <KernelExport.h>
#include <boot/platform.h>
#include <boot/heap.h>
#include <boot/stage2.h>
#include <arch/cpu.h>
#include <string.h>

#include <gccore.h>
#include <ogcsys.h>
#include <ogc/machine/processor.h>
#include <wiiuse/wpad.h>
#include <wiikeyboard/keyboard.h>

#include "console.h"
#include "debug.h"


// the part of libogc's crt0 that has to run before any other libogc call;
// none of these are declared in a public header
extern "C" void PPCExcptInit(void);
extern "C" void KThreadInit(void);
extern "C" void KIrqInit(void);
extern "C" void SYS_PreMain(void);

extern "C" void wii_start(void);
extern "C" int main(stage2_args *args);
extern "C" status_t arch_start_kernel(struct kernel_args *kernelArgs,
		addr_t kernelEntry, addr_t kernelStackTop);
extern "C" status_t boot_arch_mmu_init(void);

extern void (*__ctor_list)(void);
extern void (*__ctor_end)(void);


static void
call_ctors(void)
{
	void (**f)(void);

	for (f = &__ctor_list; f < &__ctor_end; f++)
		(**f)();
}


static addr_t
get_kernel_entry(void)
{
	if (gKernelArgs.kernel_image->elf_class == ELFCLASS64) {
		preloaded_elf64_image *image = static_cast<preloaded_elf64_image *>(
			gKernelArgs.kernel_image.Pointer());
		return image->elf_header.e_entry;
	} else if (gKernelArgs.kernel_image->elf_class == ELFCLASS32) {
		preloaded_elf32_image *image = static_cast<preloaded_elf32_image *>(
			gKernelArgs.kernel_image.Pointer());
		return image->elf_header.e_entry;
	}
	panic("Unknown kernel format! Not 32-bit or 64-bit!");
	return 0;
}


extern "C" void
platform_start_kernel(void)
{
	addr_t kernelEntry = get_kernel_entry();
	addr_t stackTop = gKernelArgs.cpu_kstack[0].start
		+ gKernelArgs.cpu_kstack[0].size;

	dprintf("kernel entry at %p\n", (void*)kernelEntry);
	dprintf("kernel stack top: %p\n", (void*)stackTop);

	// libogc runs with interrupts enabled and a decrementer alarm ticking; the
	// kernel takes over the exception vectors, so both have to be off first.
	uint32_t cookie;
	_CPU_ISR_Disable(cookie);
	(void)cookie;

	status_t error = arch_start_kernel(&gKernelArgs, kernelEntry, stackTop);

	panic("Kernel returned! Return value: %" B_PRId32 "\n", error);
}


extern "C" void
platform_exit(void)
{
	SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);

	while (true)
		;
}


extern "C" uint32
platform_boot_options(void)
{
	return 0;
}


/*!	Broadway runs at 729 MHz off a 243 MHz bus, and the time base counts at a
	quarter of the bus clock. The kernel divides by the time base frequency in
	arch_rtc_init(), so it must not be left at zero.
*/
extern "C" status_t
boot_arch_cpu_init(void)
{
	gKernelArgs.arch_args.cpu_frequency = TB_CORE_CLOCK;
	gKernelArgs.arch_args.bus_frequency = TB_BUS_CLOCK;
	gKernelArgs.arch_args.time_base_frequency = TB_TIMER_CLOCK * 1000;

	return B_OK;
}


extern "C" void
wii_start(void)
{
	ctype_init();

	PPCExcptInit();
	KThreadInit();
	KIrqInit();
	SYS_Init();
	SYS_PreMain();

	call_ctors();

	video_init();
	debug_init();

	dprintf("\nHaiku boot loader for the Nintendo Wii\n");
	dprintf("MEM1 arena: %p - %p, MEM2 arena: %p - %p\n", SYS_GetArena1Lo(),
		SYS_GetArena1Hi(), SYS_GetArena2Lo(), SYS_GetArena2Hi());

	WPAD_Init();
	if (KEYBOARD_Init(NULL) != 0)
		dprintf("no USB keyboard found\n");

	if (boot_arch_mmu_init() != B_OK)
		panic("could not set up the loader's memory map\n");

	stage2_args args;
	memset(&args, 0, sizeof(stage2_args));

	main(&args);
		// only returns if the user asked to leave the loader

	platform_exit();
}
