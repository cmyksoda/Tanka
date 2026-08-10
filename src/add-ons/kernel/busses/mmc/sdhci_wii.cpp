/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Antigravity
 */
#include <algorithm>
#include <new>
#include <stdio.h>
#include <string.h>

#include <KernelExport.h>

#include "IOSchedulerSimple.h"
#include "mmc.h"
#include "sdhci.h"

#define SDHCI_WII_MMC_BUS_MODULE_NAME "busses/mmc/sdhci/wii/device/v1"

#define HOLLYWOOD_SDHC_BASE 0xCD070000
#define HOLLYWOOD_SDHC_SIZE 0x200
#define HOLLYWOOD_SDHC_IRQ  7

status_t
init_bus_wii(device_node* node, void** bus_cookie)
{
	area_id	regs_area;
	struct registers* _regs;
	
	// Map the Hollywood SDHC registers
	regs_area = map_physical_memory("sdhc_regs_map",
		HOLLYWOOD_SDHC_BASE, HOLLYWOOD_SDHC_SIZE, B_ANY_KERNEL_BLOCK_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&_regs);

	if (regs_area < B_OK) {
		return B_BAD_VALUE;
	}

	uint8_t irq = HOLLYWOOD_SDHC_IRQ;
	
	// Wii registers are technically byte-swapped, but since SdhciBus accesses them
	// natively from a Big-Endian PowerPC CPU, the writes/reads match the bus alignment!
	SdhciBus* bus = new(std::nothrow) SdhciBus(_regs, irq, false);

	status_t status = B_NO_MEMORY;
	if (bus != NULL)
		status = bus->InitCheck();

	if (status != B_OK) {
		if (bus != NULL)
			delete bus;
		else
			delete_area(regs_area);
		return status;
	}

	*bus_cookie = bus;
	return status;
}

status_t
register_child_devices_wii(void* cookie)
{
	SdhciDevice* context = (SdhciDevice*)cookie;

	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "Wii SDHC Bus" } },
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE, {.string = MMC_BUS_MODULE_NAME} },
		{ B_DEVICE_BUS, B_STRING_TYPE, {.string = "mmc"} },
		
		// DMA properties for Wii MEM1/MEM2
		{ B_DMA_ALIGNMENT, B_UINT32_TYPE, { .ui32 = 511 }},
		{ B_DMA_HIGH_ADDRESS, B_UINT64_TYPE, { .ui64 = 0x100000000LL }},
		{ B_DMA_BOUNDARY, B_UINT32_TYPE, { .ui32 = (1 << 19) - 1 }},
		{ B_DMA_MAX_SEGMENT_COUNT, B_UINT32_TYPE, { .ui32 = 1 }},
		{ B_DMA_MAX_SEGMENT_BLOCKS, B_UINT32_TYPE, { .ui32 = (1 << 10) - 1 }},
		{ NULL }
	};
	
	device_node* node;
	if (gDeviceManager->register_node(context->fNode,
			SDHCI_WII_MMC_BUS_MODULE_NAME, attrs, NULL,
			&node) != B_OK)
		return B_BAD_VALUE;

	return B_OK;
}

float
supports_device_wii(device_node* parent)
{
	const char* bus;

	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
		!= B_OK) {
		return -1;
	}

	if (strcmp(bus, "hollywood") == 0) {
		return 0.8f;
	}

	return 0.0f;
}

mmc_bus_interface gSDHCIWiiDeviceModule = {
	.info = {
		.info = {
			.name = SDHCI_WII_MMC_BUS_MODULE_NAME,
		},

		.init_driver = init_bus_wii,
		.uninit_driver = uninit_bus,
		.device_removed = bus_removed,
	},

	.set_clock = set_clock,
	.execute_command = execute_command,
	.do_io = do_io,
	.set_scan_semaphore = set_scan_semaphore,
	.set_bus_width = set_bus_width,
	.terminate_bus = terminate_bus,
	.set_card_type = set_card_type,
};
