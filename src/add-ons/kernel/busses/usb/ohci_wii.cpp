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

#include "ohci.h"

#define OHCI_WII_USB_BUS_MODULE_NAME "busses/usb/ohci/wii/device/v1"
#define OHCI_WII_DEVICE_MODULE_NAME "busses/usb/ohci/wii/v1"

#define HOLLYWOOD_OHCI_0_BASE 0xCD050000
#define HOLLYWOOD_OHCI_1_BASE 0xCD060000
#define HOLLYWOOD_OHCI_SIZE   0x1000

// In Haiku PowerPC, we usually just pass IRQ vectors straight through.
#define HOLLYWOOD_OHCI_0_IRQ  5
#define HOLLYWOOD_OHCI_1_IRQ  6

struct ohci_wii_sim_info {
	device_node* driver_node;
	phys_addr_t base;
	uint32 irq;
};

static status_t
init_bus_wii(device_node* node, void** bus_cookie)
{
	driver_module_info* driver;
	ohci_wii_sim_info* bus;
	device_node* parent = gDeviceManager->get_parent_node(node);
	gDeviceManager->get_driver(parent, &driver, (void**)&bus);
	gDeviceManager->put_node(parent);

	Stack *stack;
	if (gUSB->get_stack((void**)&stack) != B_OK)
		return B_ERROR;

	OHCI *ohci = new(std::nothrow) OHCI(bus->base, HOLLYWOOD_OHCI_SIZE, bus->irq, NULL, NULL, NULL, stack, node);
	if (ohci == NULL) {
		return B_NO_MEMORY;
	}

	if (ohci->InitCheck() < B_OK) {
		delete ohci;
		return B_ERROR;
	}

	if (ohci->Start() != B_OK) {
		delete ohci;
		return B_ERROR;
	}

	*bus_cookie = ohci;

	return B_OK;
}

static void
uninit_bus_wii(void* bus_cookie)
{
	OHCI *ohci = (OHCI *)bus_cookie;
	delete ohci;
}

static status_t
register_child_devices_wii(void* cookie)
{
	ohci_wii_sim_info* bus = (ohci_wii_sim_info*)cookie;
	device_node* node = bus->driver_node;

	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "OHCI Wii Controller" }},
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE, { .string = USB_FOR_CONTROLLER_MODULE_NAME }},
		{ NULL }
	};

	return gDeviceManager->register_node(node, OHCI_WII_USB_BUS_MODULE_NAME,
		attrs, NULL, NULL);
}

static status_t
init_device_wii(device_node* node, void** device_cookie)
{
	ohci_wii_sim_info* bus = (ohci_wii_sim_info*)calloc(1,
		sizeof(ohci_wii_sim_info));
	if (bus == NULL)
		return B_NO_MEMORY;

	bus->driver_node = node;
	
	// Check which OHCI controller this is based on some attribute (e.g., FDT reg)
	// For now, assume it's OHCI 0
	bus->base = HOLLYWOOD_OHCI_0_BASE;
	bus->irq = HOLLYWOOD_OHCI_0_IRQ;

	*device_cookie = bus;
	return B_OK;
}

static void
uninit_device_wii(void* device_cookie)
{
	ohci_wii_sim_info* bus = (ohci_wii_sim_info*)device_cookie;
	free(bus);
}

static status_t
register_device_wii(device_node* parent)
{
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "OHCI Wii"}},
		{}
	};

	return gDeviceManager->register_node(parent,
		OHCI_WII_DEVICE_MODULE_NAME, attrs, NULL, NULL);
}

static float
supports_device_wii(device_node* parent)
{
	const char* bus;
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false) < B_OK)
		return -1;

	if (strcmp(bus, "hollywood") == 0)
		return 0.8f;

	return 0.0f;
}

usb_bus_interface gOHCIWiiDeviceModule = {
	.info = {
		.info = {
			.name = OHCI_WII_USB_BUS_MODULE_NAME,
		},
		.init_driver = init_bus_wii,
		.uninit_driver = uninit_bus_wii,
		.device_removed = NULL, // bus_removed
	},
};

driver_module_info sOHCIWiiDevice = {
	.info = {
		.name = OHCI_WII_DEVICE_MODULE_NAME,
	},
	.supports_device = supports_device_wii,
	.register_device = register_device_wii,
	.init_device = init_device_wii,
	.uninit_device = uninit_device_wii,
	.register_child_devices = register_child_devices_wii,
};
