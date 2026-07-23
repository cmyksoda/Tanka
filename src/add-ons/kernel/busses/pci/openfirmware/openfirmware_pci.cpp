/*
 * Copyright 2026, Sean Malseed.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Sean Malseed, actionretro@pm.me
 *		Claude (Anthropic), paired via Claude Code
 *
 * OpenFirmware PCI host-bridge controller for PowerPC Macs.
 *
 * The Haiku PCI bus manager on ppc enumerates the bus itself (fBusEnumeration
 * = true), but that needs a pci_controller host bridge registered via
 * PCI::AddController() to provide config-space access. On x86 the host bridge
 * comes from ACPI; on arm/riscv from FDT. PowerPC Macs describe their host
 * bridge in the OpenFirmware device tree instead, and there was no ppc host
 * bridge driver at all - so device_manager saw an empty device tree and
 * nothing (disk, interrupt controller, ...) could ever be driven.
 *
 * This driver attaches to the device_manager root and provides config-space
 * access. The first target is the MPC106 "Grackle" host bridge used by the
 * iMac G3 and beige Power Mac G3: its config mechanism is the classic indirect
 * CONFIG_ADDR/CONFIG_DATA pair, memory-mapped at architecturally-fixed
 * physical addresses (0xFEC00000 / 0xFEE00000) and accessed little-endian (the
 * Grackle runs in PCI little-endian mode).
 *
 * NOTE: this driver deliberately makes no OpenFirmware client calls. The OF
 * client interface is not reliably callable this late in kernel boot - walking
 * the device tree at runtime jumps into OF ROM code the kernel MMU doesn't map
 * and faults. Everything here therefore relies on the fixed Grackle register
 * layout. Reading the host bridge's address windows from OF (for non-Grackle
 * bridges, or for PCI resource allocation) will require capturing that
 * information in the boot loader, where OF is fully alive, and passing it
 * through kernel_args.
 */

#include <new>

#include <stdio.h>
#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>

#include <AutoDeleterOS.h>
#include <bus/PCI.h>
#include <device_manager.h>


#define OF_PCI_DRIVER_MODULE_NAME	"busses/pci/openfirmware/driver_v1"

#define CHECK_RET(err) { status_t _err = (err); if (_err < B_OK) return _err; }


// Grackle (MPC106) indirect config registers. These physical addresses are
// architecturally fixed for the MPC106 and match the OpenFirmware device tree.
#define GRACKLE_CONFIG_ADDR		0xFEC00000
#define GRACKLE_CONFIG_DATA		0xFEE00000
#define GRACKLE_REGS_BASE		0xFEC00000
#define GRACKLE_REGS_SIZE		0x00300000	// covers ADDR, DATA and int-ack

// Grackle "Address Map B" host-side windows (CPU physical -> PCI). These are
// the standard MPC106 windows; they let the PCI stack understand where the
// already-assigned device BARs live.
#define GRACKLE_MMIO_HOST_BASE	0x80000000
#define GRACKLE_MMIO_PCI_BASE	0x80000000
#define GRACKLE_MMIO_SIZE		0x7D000000	// 0x80000000 .. 0xFCFFFFFF
#define GRACKLE_IO_HOST_BASE	0xFE000000
#define GRACKLE_IO_PCI_BASE		0x00000000
#define GRACKLE_IO_SIZE			0x00400000


device_manager_info* gDeviceManager;
pci_module_info* gPCI;


class OpenFirmwarePCIController {
public:
	static float SupportsDevice(device_node* parent);
	static status_t RegisterDevice(device_node* parent);
	static status_t InitDriver(device_node* node,
		OpenFirmwarePCIController*& outDriver);
	void UninitDriver();

	status_t ReadConfig(uint8 bus, uint8 device, uint8 function,
		uint16 offset, uint8 size, uint32& value);
	status_t WriteConfig(uint8 bus, uint8 device, uint8 function,
		uint16 offset, uint8 size, uint32 value);

	status_t GetMaxBusDevices(int32& count);
	status_t GetRange(uint32 index, pci_resource_range* range);

	status_t ReadIrq(uint8 bus, uint8 device, uint8 function,
		uint8 pin, uint8& irq);
	status_t WriteIrq(uint8 bus, uint8 device, uint8 function,
		uint8 pin, uint8 irq);

	status_t Finalize();

private:
	status_t InitGrackle();

	inline void SetConfigAddress(uint8 bus, uint8 device, uint8 function,
		uint16 offset);

private:
	device_node*	fNode = NULL;

	AreaDeleter		fRegsArea;
	addr_t			fConfigAddr = 0;
	addr_t			fConfigData = 0;

	pci_resource_range	fRanges[2];
	uint32			fRangeCount = 0;
};


// #pragma mark - discovery / driver lifecycle


float
OpenFirmwarePCIController::SupportsDevice(device_node* parent)
{
	const char* bus;
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK) {
		return -1.0f;
	}

	// Attach to the machine root. This driver only builds for ppc Open
	// Firmware machines, which always have a PCI host bridge.
	if (strcmp(bus, "root") != 0)
		return 0.0f;

	return 0.8f;
}


status_t
OpenFirmwarePCIController::RegisterDevice(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "OpenFirmware PCI Host Bridge" } },
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{ .string = "bus_managers/pci/root/driver_v1" } },
		{}
	};

	return gDeviceManager->register_node(parent, OF_PCI_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


status_t
OpenFirmwarePCIController::InitDriver(device_node* node,
	OpenFirmwarePCIController*& outDriver)
{
	ObjectDeleter<OpenFirmwarePCIController> driver(
		new(std::nothrow) OpenFirmwarePCIController);
	if (!driver.IsSet())
		return B_NO_MEMORY;

	driver->fNode = node;

	CHECK_RET(driver->InitGrackle());

	outDriver = driver.Detach();
	return B_OK;
}


void
OpenFirmwarePCIController::UninitDriver()
{
	delete this;
}


status_t
OpenFirmwarePCIController::InitGrackle()
{
	// map the Grackle config register window (uncached device memory)
	void* regs = NULL;
	fRegsArea.SetTo(map_physical_memory("Grackle PCI config",
		GRACKLE_REGS_BASE, GRACKLE_REGS_SIZE,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &regs));
	CHECK_RET(fRegsArea.Get());

	fConfigAddr = (addr_t)regs + (GRACKLE_CONFIG_ADDR - GRACKLE_REGS_BASE);
	fConfigData = (addr_t)regs + (GRACKLE_CONFIG_DATA - GRACKLE_REGS_BASE);

	pci_resource_range& mmio = fRanges[fRangeCount++];
	mmio = {};
	mmio.type = B_IO_MEMORY;
	mmio.address_type = PCI_address_type_32;
	mmio.host_address = GRACKLE_MMIO_HOST_BASE;
	mmio.pci_address = GRACKLE_MMIO_PCI_BASE;
	mmio.size = GRACKLE_MMIO_SIZE;

	pci_resource_range& io = fRanges[fRangeCount++];
	io = {};
	io.type = B_IO_PORT;
	io.host_address = GRACKLE_IO_HOST_BASE;
	io.pci_address = GRACKLE_IO_PCI_BASE;
	io.size = GRACKLE_IO_SIZE;

	dprintf("of_pci: Grackle host bridge ready (config %#lx/%#lx)\n",
		(addr_t)GRACKLE_CONFIG_ADDR, (addr_t)GRACKLE_CONFIG_DATA);
	return B_OK;
}


// #pragma mark - config space access (Grackle indirect)


void
OpenFirmwarePCIController::SetConfigAddress(uint8 bus, uint8 device,
	uint8 function, uint16 offset)
{
	uint32 address = 0x80000000 | ((uint32)bus << 16)
		| ((uint32)device << 11) | ((uint32)function << 8) | (offset & 0xFC);

	// The Grackle runs little-endian: a byte-reversed store latches the
	// natural CONFIG_ADDR value (equivalent to PowerPC out_le32).
	*(volatile uint32*)fConfigAddr = B_HOST_TO_LENDIAN_INT32(address);
	asm volatile("eieio" ::: "memory");
}


status_t
OpenFirmwarePCIController::ReadConfig(uint8 bus, uint8 device, uint8 function,
	uint16 offset, uint8 size, uint32& value)
{
	SetConfigAddress(bus, device, function, offset);

	addr_t data = fConfigData + (offset & 3);
	switch (size) {
		case 1:
			value = *(volatile uint8*)data;
			break;
		case 2:
			value = B_LENDIAN_TO_HOST_INT16(*(volatile uint16*)data);
			break;
		case 4:
			value = B_LENDIAN_TO_HOST_INT32(*(volatile uint32*)data);
			break;
		default:
			return B_BAD_VALUE;
	}
	asm volatile("eieio" ::: "memory");

	return B_OK;
}


status_t
OpenFirmwarePCIController::WriteConfig(uint8 bus, uint8 device, uint8 function,
	uint16 offset, uint8 size, uint32 value)
{
	SetConfigAddress(bus, device, function, offset);

	addr_t data = fConfigData + (offset & 3);
	switch (size) {
		case 1:
			*(volatile uint8*)data = (uint8)value;
			break;
		case 2:
			*(volatile uint16*)data = B_HOST_TO_LENDIAN_INT16((uint16)value);
			break;
		case 4:
			*(volatile uint32*)data = B_HOST_TO_LENDIAN_INT32(value);
			break;
		default:
			return B_BAD_VALUE;
	}
	asm volatile("eieio" ::: "memory");

	return B_OK;
}


// #pragma mark - controller queries


status_t
OpenFirmwarePCIController::GetMaxBusDevices(int32& count)
{
	count = 32;
	return B_OK;
}


status_t
OpenFirmwarePCIController::GetRange(uint32 index, pci_resource_range* range)
{
	if (index >= fRangeCount)
		return B_BAD_INDEX;

	*range = fRanges[index];
	return B_OK;
}


status_t
OpenFirmwarePCIController::ReadIrq(uint8 bus, uint8 device, uint8 function,
	uint8 pin, uint8& irq)
{
	// PCI interrupt routing is resolved through the OpenFirmware
	// "interrupt-map" and the mac-io interrupt controller, not here.
	return B_UNSUPPORTED;
}


status_t
OpenFirmwarePCIController::WriteIrq(uint8 bus, uint8 device, uint8 function,
	uint8 pin, uint8 irq)
{
	return B_UNSUPPORTED;
}


status_t
OpenFirmwarePCIController::Finalize()
{
	return B_OK;
}


// #pragma mark - module


static pci_controller_module_info sControllerModuleInfo = {
	.info = {
		.info = {
			.name = OF_PCI_DRIVER_MODULE_NAME,
		},
		.supports_device = OpenFirmwarePCIController::SupportsDevice,
		.register_device = OpenFirmwarePCIController::RegisterDevice,
		.init_driver = [](device_node* node, void** driverCookie) {
			return OpenFirmwarePCIController::InitDriver(node,
				*(OpenFirmwarePCIController**)driverCookie);
		},
		.uninit_driver = [](void* driverCookie) {
			static_cast<OpenFirmwarePCIController*>(driverCookie)
				->UninitDriver();
		},
	},
	.read_pci_config = [](void* cookie, uint8 bus, uint8 device,
		uint8 function, uint16 offset, uint8 size, uint32* value) {
		return static_cast<OpenFirmwarePCIController*>(cookie)
			->ReadConfig(bus, device, function, offset, size, *value);
	},
	.write_pci_config = [](void* cookie, uint8 bus, uint8 device,
		uint8 function, uint16 offset, uint8 size, uint32 value) {
		return static_cast<OpenFirmwarePCIController*>(cookie)
			->WriteConfig(bus, device, function, offset, size, value);
	},
	.get_max_bus_devices = [](void* cookie, int32* count) {
		return static_cast<OpenFirmwarePCIController*>(cookie)
			->GetMaxBusDevices(*count);
	},
	.read_pci_irq = [](void* cookie, uint8 bus, uint8 device, uint8 function,
		uint8 pin, uint8* irq) {
		return static_cast<OpenFirmwarePCIController*>(cookie)
			->ReadIrq(bus, device, function, pin, *irq);
	},
	.write_pci_irq = [](void* cookie, uint8 bus, uint8 device, uint8 function,
		uint8 pin, uint8 irq) {
		return static_cast<OpenFirmwarePCIController*>(cookie)
			->WriteIrq(bus, device, function, pin, irq);
	},
	.get_range = [](void* cookie, uint32 index, pci_resource_range* range) {
		return static_cast<OpenFirmwarePCIController*>(cookie)
			->GetRange(index, range);
	},
	.finalize = [](void* cookie) {
		return static_cast<OpenFirmwarePCIController*>(cookie)->Finalize();
	},
};


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager },
	{ B_PCI_MODULE_NAME, (module_info**)&gPCI },
	{}
};

module_info* modules[] = {
	(module_info*)&sControllerModuleInfo,
	NULL
};
