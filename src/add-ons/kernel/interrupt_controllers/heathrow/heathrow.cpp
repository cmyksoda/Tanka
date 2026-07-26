/*
 * Copyright 2026, Sean Malseed.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Sean Malseed, actionretro@pm.me
 *		Claude (Anthropic), paired via Claude Code
 *
 * Interrupt controller driver for the Apple "mac-io" Grand-Central-style
 * interrupt controller found in the Grand Central, Heathrow and Paddington
 * mac-io chips (Old World and early New World Power Macs / iMac G3). These
 * predate the OpenPIC/MPIC used by later mac-io chips (KeyLargo / Intrepid,
 * handled by the openpic driver).
 *
 * The controller lives at a fixed offset within the mac-io PCI device's
 * register space. It has one bank of 32 interrupt sources (Grand Central) or
 * two banks of 32 (Heathrow / Paddington, 64 sources total). Each bank has
 * four little-endian 32-bit registers: pending events, an enable mask, a
 * write-1-to-clear register, and the current line levels.
 */

#include <stdio.h>
#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>

#include <AutoDeleter.h>
#include <bus/PCI.h>
#include <interrupt_controller.h>
#include <util/kernel_cpp.h>


#define HEATHROW_MODULE_NAME	"interrupt_controllers/heathrow/device_v1"

// mac-io interrupt controller register offsets (relative to the mac-io
// register base). All little-endian.
enum {
	MIO_INT_EVENTS2	= 0x10,	// second bank (IRQs 32-63); Heathrow/Paddington
	MIO_INT_MASK2	= 0x14,
	MIO_INT_CLEAR2	= 0x18,
	MIO_INT_LEVELS2	= 0x1c,
	MIO_INT_EVENTS1	= 0x20,	// first bank (IRQs 0-31)
	MIO_INT_MASK1	= 0x24,
	MIO_INT_CLEAR1	= 0x28,
	MIO_INT_LEVELS1	= 0x2c,
};

// The top bit of the first bank's mask register selects the interrupt mode
// (0 = native/level, 1 = 68k emulated) rather than enabling an interrupt.
// We keep it clear (native mode) and never expose IRQ 31 of bank 1.
#define MACIO_INT_MODE	0x80000000


struct heathrow_supported_device {
	const char	*name;
	uint16		vendor_id;
	uint16		device_id;
	int			bank_count;	// 1 = Grand Central, 2 = Heathrow/Paddington
};

static heathrow_supported_device sSupportedDevices[] = {
	{ "Grand Central",	0x106b, 0x0002, 1 },
	{ "Heathrow",		0x106b, 0x0010, 2 },
	{ "Paddington",		0x106b, 0x0017, 2 },
	// Note: KeyLargo (0x0022) / Pangea (0x0025) / Intrepid (0x003e) do NOT use
	// this old-style 2-bank PIC. Their mac-io interrupt controller is a CHRP
	// OpenPIC (MPIC) at mac-io offset 0x40000, handled by the openpic driver.
	{}
};

static device_manager_info *sDeviceManager;


struct heathrow_info : interrupt_controller_info {
	heathrow_info()
	{
		memset((interrupt_controller_info*)this, 0,
			sizeof(interrupt_controller_info));
		register_area = -1;
	}

	~heathrow_info()
	{
		if (register_area >= 0)
			delete_area(register_area);
	}

	heathrow_supported_device	*supported_device;
	device_node					*node;
	pci_device_module_info		*pci;
	pci_device					*device;

	addr_t						virtual_registers;
	area_id						register_area;
};


static heathrow_supported_device *
heathrow_check_supported_device(uint16 vendorID, uint16 deviceID)
{
	for (heathrow_supported_device *device = sSupportedDevices;
			device->name; device++) {
		if (device->vendor_id == vendorID && device->device_id == deviceID)
			return device;
	}

	return NULL;
}


// The mac-io interrupt registers are little-endian, so byte-swap on the
// big-endian PPC. They are device registers, so use ordered accesses.
static inline uint32
heathrow_read(heathrow_info *info, int reg)
{
	uint32 value
		= *(volatile uint32*)(info->virtual_registers + reg);
	return B_LENDIAN_TO_HOST_INT32(value);
}


static inline void
heathrow_write(heathrow_info *info, int reg, uint32 value)
{
	*(volatile uint32*)(info->virtual_registers + reg)
		= B_HOST_TO_LENDIAN_INT32(value);
	asm volatile("eieio" ::: "memory");
}


static status_t
heathrow_init_controller(heathrow_info *info)
{
	info->cpu_count = 1;
	info->irq_count = info->supported_device->bank_count * 32;

	// Mask (disable) every interrupt and clear any pending events. Leaving
	// MASK1's top bit clear also selects native (level) mode.
	heathrow_write(info, MIO_INT_MASK1, 0);
	heathrow_write(info, MIO_INT_CLEAR1, 0x7fffffff);
	if (info->supported_device->bank_count > 1) {
		heathrow_write(info, MIO_INT_MASK2, 0);
		heathrow_write(info, MIO_INT_CLEAR2, 0xffffffff);
	}

	return B_OK;
}


// #pragma mark - driver interface


static status_t
heathrow_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		case B_MODULE_UNINIT:
			return B_OK;

		default:
			return B_ERROR;
	}
}


static float
heathrow_supports_device(device_node *parent)
{
	const char *bus;
	uint16 vendorID;
	uint16 deviceID;

	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK) {
		return B_ERROR;
	}

	if (sDeviceManager->get_attr_uint16(parent, B_DEVICE_VENDOR_ID, &vendorID,
			false) != B_OK
		|| sDeviceManager->get_attr_uint16(parent, B_DEVICE_ID, &deviceID,
			false) != B_OK) {
		return B_ERROR;
	}

	if (strcmp(bus, "pci") != 0
		|| heathrow_check_supported_device(vendorID, deviceID) == NULL) {
		return 0.0;
	}

	return 0.6;
}


static status_t
heathrow_register_device(device_node *parent)
{
	device_node *newNode;
	device_attr attrs[] = {
		{ B_DEVICE_TYPE, B_UINT16_TYPE, { .ui16 = PCI_base_peripheral }},
		{ B_DEVICE_SUB_TYPE, B_UINT16_TYPE, { .ui16 = PCI_pic }},
		{}
	};
	io_resource resources[] = {
		{}
	};

	return sDeviceManager->register_node(parent, HEATHROW_MODULE_NAME, attrs,
		resources, &newNode);
}


static status_t
heathrow_init_driver(device_node *node, void **cookie)
{
	heathrow_info *info = new(std::nothrow) heathrow_info;
	if (info == NULL)
		return B_NO_MEMORY;
	ObjectDeleter<heathrow_info> infoDeleter(info);

	info->node = node;

	// get the interface to the parent PCI device. get_driver() hands back the
	// parent node's driver cookie directly - for a PCI device node that cookie
	// is the pci_device we need. (An earlier version called the PCI module's
	// init_driver() again on our own node, which has no PCI address attributes,
	// and left info->device uninitialized -> a fault dereferencing 0xcccccccc.)
	void *pciCookie;
	status_t status = sDeviceManager->get_driver(
		sDeviceManager->get_parent_node(node),
		(driver_module_info**)&info->pci, &pciCookie);
	if (status != B_OK)
		return status;
	info->device = (pci_device*)pciCookie;

	pci_info pciInfo;
	info->pci->get_pci_info(info->device, &pciInfo);

	info->supported_device = heathrow_check_supported_device(pciInfo.vendor_id,
		pciInfo.device_id);
	if (info->supported_device == NULL) {
		dprintf("heathrow: device (0x%04hx:0x%04hx) not supported\n",
			pciInfo.vendor_id, pciInfo.device_id);
		return B_ERROR;
	}

	// map the mac-io register space (the interrupt registers live in the
	// low 0x30 bytes; map a whole page)
	addr_t physicalBase = pciInfo.u.h0.base_registers[0];
	size_t size = pciInfo.u.h0.base_register_sizes[0];
	if (size < B_PAGE_SIZE)
		size = B_PAGE_SIZE;

	void *virtualBase = NULL;
	area_id registerArea = map_physical_memory("heathrow pic registers",
		physicalBase, size, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &virtualBase);
	if (registerArea < 0)
		return registerArea;

	info->register_area = registerArea;
	info->virtual_registers = (addr_t)virtualBase;

	status = heathrow_init_controller(info);
	if (status != B_OK)
		return status;

	dprintf("heathrow: %s interrupt controller at %p, %d IRQs\n",
		info->supported_device->name, (void*)physicalBase, info->irq_count);

	infoDeleter.Detach();
	*cookie = info;

	return B_OK;
}


static void
heathrow_uninit_driver(void *cookie)
{
	heathrow_info *info = (heathrow_info*)cookie;
	delete info;
}


static status_t
heathrow_register_child_devices(void *cookie)
{
	return B_OK;
}


static status_t
heathrow_rescan_child_devices(void *cookie)
{
	return B_OK;
}


static void
heathrow_device_removed(void *cookie)
{
}


// #pragma mark - interrupt_controller interface


static status_t
heathrow_get_controller_info(void *cookie, interrupt_controller_info *_info)
{
	if (_info == NULL)
		return B_BAD_VALUE;

	heathrow_info *info = (heathrow_info*)cookie;
	*_info = *info;

	return B_OK;
}


static status_t
heathrow_enable_io_interrupt(void *cookie, int irq, int type)
{
	heathrow_info *info = (heathrow_info*)cookie;
	if (irq < 0 || irq >= info->irq_count)
		return B_BAD_VALUE;

	int maskReg = (irq < 32) ? MIO_INT_MASK1 : MIO_INT_MASK2;
	uint32 mask = heathrow_read(info, maskReg);
	mask |= 1u << (irq & 31);
	if (maskReg == MIO_INT_MASK1)
		mask &= ~(uint32)MACIO_INT_MODE;
	heathrow_write(info, maskReg, mask);

	return B_OK;
}


static status_t
heathrow_disable_io_interrupt(void *cookie, int irq)
{
	heathrow_info *info = (heathrow_info*)cookie;
	if (irq < 0 || irq >= info->irq_count)
		return B_BAD_VALUE;

	int maskReg = (irq < 32) ? MIO_INT_MASK1 : MIO_INT_MASK2;
	uint32 mask = heathrow_read(info, maskReg);
	mask &= ~(1u << (irq & 31));
	if (maskReg == MIO_INT_MASK1)
		mask &= ~(uint32)MACIO_INT_MODE;
	heathrow_write(info, maskReg, mask);

	return B_OK;
}


static int
heathrow_acknowledge_io_interrupt(void *cookie)
{
	heathrow_info *info = (heathrow_info*)cookie;

	for (int bank = 0; bank < info->supported_device->bank_count; bank++) {
		int eventsReg = (bank == 0) ? MIO_INT_EVENTS1 : MIO_INT_EVENTS2;
		int maskReg = (bank == 0) ? MIO_INT_MASK1 : MIO_INT_MASK2;
		int clearReg = (bank == 0) ? MIO_INT_CLEAR1 : MIO_INT_CLEAR2;

		uint32 pending = heathrow_read(info, eventsReg)
			& heathrow_read(info, maskReg);
		if (bank == 0)
			pending &= ~(uint32)MACIO_INT_MODE;
		if (pending == 0)
			continue;

		int bit = __builtin_ctz(pending);

		// acknowledge by clearing the event bit
		heathrow_write(info, clearReg, 1u << bit);

		return bank * 32 + bit;
	}

	return -1;	// no (more) pending interrupts / spurious
}


static interrupt_controller_module_info sControllerModuleInfo = {
	{
		{
			HEATHROW_MODULE_NAME,
			0,
			heathrow_std_ops
		},

		heathrow_supports_device,
		heathrow_register_device,
		heathrow_init_driver,
		heathrow_uninit_driver,
		heathrow_register_child_devices,
		heathrow_rescan_child_devices,
		heathrow_device_removed,
		NULL,	// suspend
		NULL	// resume
	},

	heathrow_get_controller_info,
	heathrow_enable_io_interrupt,
	heathrow_disable_io_interrupt,
	heathrow_acknowledge_io_interrupt,
};

module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};

module_info *modules[] = {
	(module_info*)&sControllerModuleInfo,
	NULL
};
