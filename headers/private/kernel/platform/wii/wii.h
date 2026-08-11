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

#define WII_HOLLYWOOD_PHYS_BASE		0x0d000000
#define WII_HOLLYWOOD_SIZE			0x00008000
#define WII_HW_PPCIRQFLAG			0x030
#define WII_HW_PPCIRQMASK			0x034
#define WII_HW_EXI					0x6800		// GC-compatible EXI block
#define WII_HW_RESETS				0x194
#define WII_HW_TIMER				0x010

#define WII_RESETS_SYS				(1 << 0)	// deasserted = full reset

// Broadway's memory map: MEM1 is 24 MiB of 1T-SRAM inside Hollywood, MEM2 is
// 64 MiB of GDDR3. The upper 3 MiB of MEM2 belong to IOS and must not be
// handed to the VM.
#define WII_MEM1_BASE				0x00000000
#define WII_MEM1_SIZE				0x01800000
#define WII_MEM2_BASE				0x10000000
#define WII_MEM2_SIZE				0x04000000
#define WII_MEM2_IOS_RESERVED		0x00300000

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
