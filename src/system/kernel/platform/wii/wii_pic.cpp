/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <platform/wii/wii.h>

#include <KernelExport.h>
#include <arch/cpu.h>
#include <interrupt_controller.h>
#include <vm/vm.h>


// The Wii has two stacked interrupt controllers. Flipper's processor interface
// drives the CPU's external interrupt line and reports its own legacy sources
// (VI, EXI, AI, DSP, ...) plus one aggregate bit for everything Hollywood
// raises. Hollywood's controller behind that bit owns the interesting devices:
// USB, SDHC, IPC and the on-chip timer.
//
// PI_INTSR bits are cleared by the originating device, so we never poll a
// source unless its handler asked for it - masked sources simply never appear
// in the cause. Hollywood's flag register is write-1-to-clear and acknowledged
// here.

static area_id sPIArea = -1;
static addr_t sPIBase;
static addr_t sHollywoodBase;


static inline volatile uint32 *
pi_reg(uint32 offset)
{
	return (volatile uint32 *)(sPIBase + offset);
}


static inline volatile uint32 *
hw_reg(uint32 offset)
{
	return (volatile uint32 *)(sHollywoodBase + offset);
}


status_t
wii_pic_init(void)
{
	if (sPIArea >= 0)
		return B_OK;

	sHollywoodBase = wii_hollywood_registers();
	if (sHollywoodBase == 0)
		return B_NO_INIT;

	// The processor interface block is a single page in Flipper's range.
	void *base;
	sPIArea = map_physical_memory("wii processor interface",
		WII_PI_PHYS_BASE & ~(addr_t)(B_PAGE_SIZE - 1), B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &base);
	if (sPIArea < 0)
		return sPIArea;

	sPIBase = (addr_t)base + (WII_PI_PHYS_BASE & (B_PAGE_SIZE - 1));

	// Mask everything, then open the aggregate bit so Hollywood sources only
	// depend on their own mask.
	*hw_reg(WII_HW_PPCIRQMASK) = 0;
	eieio();
	*hw_reg(WII_HW_PPCIRQFLAG) = 0xffffffff;
	eieio();
	*pi_reg(WII_PI_INTMR) = 1 << WII_PI_INT_HOLLYWOOD;
	eieio();

	return B_OK;
}


static status_t
wii_pic_get_controller_info(void *cookie, interrupt_controller_info *info)
{
	info->cpu_count = 1;
	info->irq_count = WII_IRQ_COUNT;
	return B_OK;
}


static status_t
wii_pic_enable_io_interrupt(void *cookie, int irq, int type)
{
	if (irq < 0 || irq >= WII_IRQ_COUNT)
		return B_BAD_VALUE;

	if (irq < WII_IRQ_PI_BASE) {
		*hw_reg(WII_HW_PPCIRQMASK) = *hw_reg(WII_HW_PPCIRQMASK) | (1 << irq);
	} else {
		uint32 bit = 1 << (irq - WII_IRQ_PI_BASE);
		*pi_reg(WII_PI_INTMR) = *pi_reg(WII_PI_INTMR) | bit;
	}
	eieio();

	return B_OK;
}


static status_t
wii_pic_disable_io_interrupt(void *cookie, int irq)
{
	if (irq < 0 || irq >= WII_IRQ_COUNT)
		return B_BAD_VALUE;

	if (irq < WII_IRQ_PI_BASE) {
		*hw_reg(WII_HW_PPCIRQMASK) = *hw_reg(WII_HW_PPCIRQMASK) & ~(1 << irq);
	} else {
		uint32 bit = 1 << (irq - WII_IRQ_PI_BASE);
		*pi_reg(WII_PI_INTMR) = *pi_reg(WII_PI_INTMR) & ~bit;
	}
	eieio();

	return B_OK;
}


static int
wii_pic_acknowledge_io_interrupt(void *cookie)
{
	uint32 cause = *pi_reg(WII_PI_INTSR) & *pi_reg(WII_PI_INTMR);
	if (cause == 0)
		return -1;

	if ((cause & (1 << WII_PI_INT_HOLLYWOOD)) != 0) {
		uint32 hollywood = *hw_reg(WII_HW_PPCIRQFLAG)
			& *hw_reg(WII_HW_PPCIRQMASK);
		if (hollywood != 0) {
			int irq = 31 - __builtin_clz(hollywood);
			*hw_reg(WII_HW_PPCIRQFLAG) = 1 << irq;
			eieio();
			return WII_IRQ_HOLLYWOOD_BASE + irq;
		}

		// Aggregate bit set with no unmasked Hollywood source left: the
		// remaining cause bits, if any, are Flipper's own.
		cause &= ~(1 << WII_PI_INT_HOLLYWOOD);
		if (cause == 0)
			return -1;
	}

	// Flipper sources are cleared by their device's handler, not here.
	return WII_IRQ_PI_BASE + (31 - __builtin_clz(cause));
}


static interrupt_controller_module_info sWiiPICModule = {
	{
		{
			"interrupt_controllers/wii/device_v1",
			0,
			NULL
		},
		NULL,	// supports_device
		NULL,	// register_device
		NULL,	// init_driver
		NULL,	// uninit_driver
		NULL,	// register_child_devices
		NULL,	// rescan_child_devices
		NULL,	// device_removed
		NULL,	// suspend
		NULL	// resume
	},
	wii_pic_get_controller_info,
	wii_pic_enable_io_interrupt,
	wii_pic_disable_io_interrupt,
	wii_pic_acknowledge_io_interrupt
};


interrupt_controller_module_info *
wii_pic_module(void)
{
	return &sWiiPICModule;
}
