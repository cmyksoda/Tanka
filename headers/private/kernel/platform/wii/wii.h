/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_PLATFORM_WII_H
#define _KERNEL_PLATFORM_WII_H

#include <SupportDefs.h>

struct kernel_args;
struct interrupt_controller_module_info;
struct real_time_data;


// Physical MMIO layout. Broadway sees these uncached; the kernel maps them
// through map_physical_memory(), which defaults to B_UNCACHED_MEMORY.
#define WII_PI_PHYS_BASE			0x0c003000	// Flipper processor interface
#define WII_PI_INTSR				0x00
#define WII_PI_INTMR				0x04

// Hollywood publishes two register banks. Broadway always reaches the first;
// the second belongs to Starlet and only answers when the loader was granted
// AHB access (the Homebrew Channel does so for an <ahb_access/> app).
#define WII_HOLLYWOOD_PHYS_BASE		0x0d000000
#define WII_HOLLYWOOD_SIZE			0x00008000
#define WII_HW_PPCIRQFLAG			0x030
#define WII_HW_PPCIRQMASK			0x034
#define WII_HW_EXI					0x6800		// GC-compatible EXI block

#define WII_STARLET_PHYS_BASE		0x0d800000
#define WII_STARLET_SIZE			0x00000400
#define WII_HW_RESETS				0x194

#define WII_RESETS_SYS				(1 << 0)	// deasserted = full reset

// IRQ numbering handed to the kernel. Hollywood sources keep their hardware
// bit number, since the Wii drivers hardcode them (5/6 OHCI, 7 SDHC); the
// Flipper processor interface sources follow above them.
#define WII_IRQ_HOLLYWOOD_BASE		0
#define WII_IRQ_HOLLYWOOD_COUNT		32
#define WII_IRQ_PI_BASE				32
#define WII_IRQ_PI_COUNT			15
#define WII_IRQ_COUNT				(WII_IRQ_PI_BASE + WII_IRQ_PI_COUNT)

// Processor interface cause bit aggregating every Hollywood interrupt.
#define WII_PI_INT_HOLLYWOOD		14


#ifdef __cplusplus
extern "C" {
#endif

status_t wii_platform_init(struct kernel_args *args);
status_t wii_platform_init_post_vm(struct kernel_args *args);
void wii_platform_shutdown(bool reboot);

// Kernel virtual address of the mapped Hollywood register block, valid after
// wii_platform_init_post_vm().
addr_t wii_hollywood_registers(void);

status_t wii_pic_init(void);
struct interrupt_controller_module_info *wii_pic_module(void);

status_t wii_rtc_init(void);
uint32 wii_rtc_get(void);
void wii_rtc_set(uint32 seconds);

#ifdef __cplusplus
}
#endif

#endif	// _KERNEL_PLATFORM_WII_H
