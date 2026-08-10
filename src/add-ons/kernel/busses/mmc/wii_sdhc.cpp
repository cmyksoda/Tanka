/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Antigravity
 */

#include <KernelExport.h>
#include <device_manager.h>
#include <bus/mmc/mmc.h>

#include <string.h>
#include <new>

#define TRACE_WII_SDHC
#ifdef TRACE_WII_SDHC
#	define TRACE(x...) dprintf("wii_sdhc: " x)
#else
#	define TRACE(x...) ;
#endif


struct wii_sdhc_cookie {
	device_node* node;
	// Hardware registers and state
	// void* registers;
};


//	#pragma mark - MMC Bus Interface Implementation


static status_t
wii_sdhc_set_clock(void* _cookie, uint32_t kilohertz)
{
	wii_sdhc_cookie* cookie = (wii_sdhc_cookie*)_cookie;
	TRACE("set_clock(%lu kHz)\n", kilohertz);
	// TODO: Configure Wii RVL-SD hardware clock
	return B_OK;
}


static status_t
wii_sdhc_execute_command(void* _cookie, uint8_t command, uint32_t argument,
	uint32_t* result)
{
	wii_sdhc_cookie* cookie = (wii_sdhc_cookie*)_cookie;
	TRACE("execute_command(cmd: %d, arg: 0x%08" B_PRIx32 ")\n", command, argument);
	// TODO: Send command via RVL-SD registers
	return B_ERROR; // Not implemented yet
}


static status_t
wii_sdhc_do_io(void* _cookie, uint8_t command, IOOperation* operation,
	bool offsetAsSectors)
{
	wii_sdhc_cookie* cookie = (wii_sdhc_cookie*)_cookie;
	TRACE("do_io(cmd: %d, offsetAsSectors: %d)\n", command, offsetAsSectors);
	// TODO: DMA / PIO data transfer
	return B_ERROR;
}


static void
wii_sdhc_set_scan_semaphore(void* _cookie, sem_id sem)
{
	wii_sdhc_cookie* cookie = (wii_sdhc_cookie*)_cookie;
	TRACE("set_scan_semaphore()\n");
	// Store semaphore for media change interrupts
}


static void
wii_sdhc_set_bus_width(void* _cookie, int width)
{
	wii_sdhc_cookie* cookie = (wii_sdhc_cookie*)_cookie;
	TRACE("set_bus_width(%d)\n", width);
	// TODO: Wii SD controller supports 4-bit mode, configure it here.
}


static void
wii_sdhc_terminate_bus(void* _cookie)
{
	wii_sdhc_cookie* cookie = (wii_sdhc_cookie*)_cookie;
	TRACE("terminate_bus()\n");
}


static void
wii_sdhc_set_card_type(void* _cookie, card_type type)
{
	wii_sdhc_cookie* cookie = (wii_sdhc_cookie*)_cookie;
	TRACE("set_card_type(%d)\n", type);
}


//	#pragma mark - Device Module Interface


static status_t
wii_sdhc_init_device(void* _cookie, void** cookie)
{
	device_node* node = (device_node*)_cookie;
	TRACE("init_device()\n");

	wii_sdhc_cookie* device = new(std::nothrow) wii_sdhc_cookie;
	if (device == NULL)
		return B_NO_MEMORY;

	device->node = node;
	// TODO: Map RVL-SD hardware registers (0x0D006000) here

	*cookie = device;
	return B_OK;
}


static void
wii_sdhc_uninit_device(void* _cookie)
{
	wii_sdhc_cookie* cookie = (wii_sdhc_cookie*)_cookie;
	TRACE("uninit_device()\n");

	// TODO: Unmap hardware registers
	delete cookie;
}


static status_t
wii_sdhc_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {string: "Nintendo Wii SDHC Controller"}},
		{NULL}
	};

	return gDeviceManager->register_node(parent, MMC_BUS_MODULE_NAME, attrs,
		NULL, NULL);
}


static status_t
wii_sdhc_probe(device_node* parent)
{
	// Ensure we only bind on the Nintendo Wii architecture.
	// Since we are compiling specifically for Wii/PowerPC, and this is a custom
	// internal bus, we look for a specific parent bus (e.g. "wii_bus" or "hollywood").
	
	const char* bus;
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false) != B_OK)
		return B_ERROR;

	if (strcmp(bus, "fdt") != 0 && strcmp(bus, "hollywood") != 0)
		return B_ERROR;

	// TODO: check FDT node name / compatible string for Wii SDHC

	return 0.8; // High likelihood if it matches our FDT/bus check
}


//	#pragma mark - Module Structures


static mmc_bus_interface sWiiSdhcModule = {
	{
		{
			"busses/mmc/wii_sdhc/v1",
			0,
			NULL
		},
		NULL, // supports_device
		wii_sdhc_register_device,
		wii_sdhc_init_device,
		wii_sdhc_uninit_device,
		NULL, // register_child_devices
		NULL, // rescan
		NULL, // device_removed
	},
	wii_sdhc_set_clock,
	wii_sdhc_execute_command,
	wii_sdhc_do_io,
	wii_sdhc_set_scan_semaphore,
	wii_sdhc_set_bus_width,
	wii_sdhc_terminate_bus,
	wii_sdhc_set_card_type
};

static driver_module_info sWiiSdhcDeviceModule = {
	{
		"busses/mmc/wii_sdhc/driver_v1",
		0,
		NULL
	},
	NULL,
	wii_sdhc_probe,
	NULL, // init_device (handled by mmc_bus_interface)
	NULL, // uninit_device
	NULL, // register_child_devices
	NULL, // rescan
	NULL  // device_removed
};


device_manager_info* gDeviceManager;


module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager},
	{}
};

module_info* modules[] = {
	(module_info*)&sWiiSdhcModule,
	(module_info*)&sWiiSdhcDeviceModule,
	NULL
};
