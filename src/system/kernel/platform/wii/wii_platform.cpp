/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <platform/wii/wii.h>

#include <KernelExport.h>
#include <arch/cpu.h>
#include <boot/kernel_args.h>
#include <vm/vm.h>


static area_id sHollywoodArea = -1;
static addr_t sHollywoodBase;
static addr_t sStarletBase;


addr_t
wii_hollywood_registers(void)
{
	return sHollywoodBase;
}


status_t
wii_platform_init(struct kernel_args *args)
{
	return B_OK;
}


status_t
wii_platform_init_post_vm(struct kernel_args *args)
{
	if (sHollywoodArea >= 0)
		return B_OK;

	void *base;
	sHollywoodArea = map_physical_memory("wii hollywood registers",
		WII_HOLLYWOOD_PHYS_BASE, WII_HOLLYWOOD_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &base);
	if (sHollywoodArea < 0)
		return sHollywoodArea;

	sHollywoodBase = (addr_t)base;

	// Optional: only reachable with AHB access, and only needed to reset.
	if (map_physical_memory("wii starlet registers", WII_STARLET_PHYS_BASE,
			WII_STARLET_SIZE, B_ANY_KERNEL_ADDRESS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &base) >= 0) {
		sStarletBase = (addr_t)base;
	}

	return B_OK;
}


void
wii_platform_shutdown(bool reboot)
{
	if (sStarletBase == 0) {
		dprintf("wii_platform_shutdown(): no access to the Starlet registers, "
			"cannot reset\n");
		return;
	}

	// Deasserting SYSRSTB in the resets register drives the console through a
	// cold reset; there is no software poweroff without IOS.
	volatile uint32 *resets = (volatile uint32 *)(sStarletBase
		+ WII_HW_RESETS);
	*resets = *resets & ~WII_RESETS_SYS;
	eieio();

	for (;;)
		;
}
