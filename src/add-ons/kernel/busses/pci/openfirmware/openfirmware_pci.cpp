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

// Host bridge types (must match arch_kernel_args.h).
#define PCI_HOST_BRIDGE_GRACKLE		0
#define PCI_HOST_BRIDGE_UNINORTH	1

extern "C" void ppc_get_pci_host_bridge(uint32* type,
	phys_addr_t* configAddress, phys_addr_t* configData);
extern "C" uint32 ppc_get_pci_host_bridge_count();
extern "C" status_t ppc_get_pci_host_bridge_at(uint32 index, uint32* type,
	phys_addr_t* configAddress, phys_addr_t* configData);
extern "C" uint32 ppc_get_gmac_irq();


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
	status_t InitController();

	inline void SetConfigAddress(uint8 bus, uint8 device, uint8 function,
		uint16 offset);

private:
	device_node*	fNode = NULL;

	AreaDeleter		fRegsArea;
	addr_t			fConfigAddr = 0;
	addr_t			fConfigData = 0;
	uint32			fHostBridgeType = PCI_HOST_BRIDGE_GRACKLE;
	uint32			fBridgeIndex = 0;
	addr_t			fConfigDataMask = 0x03;

	pci_resource_range	fRanges[4];
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
	// A Power Mac G4 has several UniNorth PCI host bridges (the boot disk,
	// the built-in Ethernet, and FireWire can each live on a different one).
	// Register one controller node per bridge so each becomes its own PCI
	// domain and every bus gets enumerated.
	uint32 count = ppc_get_pci_host_bridge_count();
	if (count == 0)
		count = 1;

	status_t lastError = B_OK;
	for (uint32 i = 0; i < count; i++) {
		device_attr attrs[] = {
			{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
				{ .string = "OpenFirmware PCI Host Bridge" } },
			{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
				{ .string = "bus_managers/pci/root/driver_v1" } },
			{ "of_pci/bridge_index", B_UINT32_TYPE, { .ui32 = i } },
			{}
		};

		status_t error = gDeviceManager->register_node(parent,
			OF_PCI_DRIVER_MODULE_NAME, attrs, NULL, NULL);
		if (error != B_OK)
			lastError = error;
	}

	return lastError;
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

	CHECK_RET(driver->InitController());

	outDriver = driver.Detach();
	return B_OK;
}


void
OpenFirmwarePCIController::UninitDriver()
{
	delete this;
}



// The GMAC Ethernet clock is gated by the UniNorth (not KeyLargo) clock
// control register. Until it is running the GMAC's PCI config space reads
// all-ones and the bus scan cannot see it. Enable it before the Ethernet
// bridge (the UniNorth bus at config 0xf4800000) is enumerated. The UniNorth
// control registers live at physical 0xf8000000 on these Power Mac G4s; the
// clock control register is at +0x20 and is accessed little-endian. We only
// OR in the GMAC bit (read-modify-write), so other clocks are untouched.
// This mirrors Linux's core99_gmac_enable().
#define UNINORTH_PHYS_BASE		0xf8000000
#define UNI_N_CLOCK_CNTL		0x20
#define UNI_N_CLOCK_CNTL_GMAC		0x02
#define ETHERNET_BRIDGE_CONFIG_ADDR	0xf4800000

static void
enable_gmac_clock()
{
	void* regs = NULL;
	area_id area = map_physical_memory("uni-n clock cntl",
		UNINORTH_PHYS_BASE, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &regs);
	if (area < B_OK) {
		dprintf("of_pci: GMAC clock enable: cannot map uni-n (%#lx)\n",
			(addr_t)area);
		return;
	}

	volatile uint32* clockCntl
		= (volatile uint32*)((addr_t)regs + UNI_N_CLOCK_CNTL);
	// UniNorth CONTROL registers are big-endian/native (unlike its PCI config
	// space, which is little-endian). Access this register natively.
	uint32 before = B_BENDIAN_TO_HOST_INT32(*clockCntl);
	*clockCntl = B_HOST_TO_BENDIAN_INT32(before | UNI_N_CLOCK_CNTL_GMAC);
	asm volatile("eieio" ::: "memory");
	(void)*clockCntl;
	asm volatile("eieio" ::: "memory");
	spin(20);
	uint32 after = B_BENDIAN_TO_HOST_INT32(*clockCntl);
	dprintf("of_pci: GMAC clock enable: UNI_N_CLOCK_CNTL %#08x -> %#08x\n",
		(unsigned)before, (unsigned)after);
	// Give the cell time to come out of clock-gating before it is probed.
	spin(3000);

	delete_area(area);
}


status_t
OpenFirmwarePCIController::InitController()
{
	// Which host bridge is this node? (Set by RegisterDevice.)
	uint32 index = 0;
	gDeviceManager->get_attr_uint32(fNode, "of_pci/bridge_index", &index,
		false);
	fBridgeIndex = index;

	uint32 type = PCI_HOST_BRIDGE_GRACKLE;
	phys_addr_t configAddrPhys = GRACKLE_CONFIG_ADDR;
	phys_addr_t configDataPhys = GRACKLE_CONFIG_DATA;
	if (ppc_get_pci_host_bridge_at(index, &type, &configAddrPhys,
			&configDataPhys) != B_OK) {
		ppc_get_pci_host_bridge(&type, &configAddrPhys, &configDataPhys);
	}
	fHostBridgeType = type;
	// UniNorth CONFIG_DATA is an 8-byte window (offset & 0x07); Grackle a
	// 4-byte one. The boot bridge (index 0) is deliberately kept on the
	// 4-byte access that every prior boot used: widening it there makes
	// odd-offset registers (header_type, BAR1/3/5) of the mac-io/ATA devices
	// read "correctly" and re-triggers a pre-existing KDiskDeviceManager
	// scan recursion during boot-volume mount. gem only needs correct access
	// on the Ethernet bridge, so apply the wider window to the non-boot
	// bridges only.
	fConfigDataMask = (type == PCI_HOST_BRIDGE_UNINORTH && fBridgeIndex != 0)
		? 0x07 : 0x03;

	// Map a window covering both config registers (uncached device memory).
	phys_addr_t lo = configAddrPhys < configDataPhys
		? configAddrPhys : configDataPhys;
	phys_addr_t hi = configAddrPhys > configDataPhys
		? configAddrPhys : configDataPhys;
	phys_addr_t regsBase = lo & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	phys_addr_t regsEnd = (hi + sizeof(uint32) + B_PAGE_SIZE - 1)
		& ~(phys_addr_t)(B_PAGE_SIZE - 1);
	void* regs = NULL;
	fRegsArea.SetTo(map_physical_memory("PCI host bridge config",
		regsBase, regsEnd - regsBase,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &regs));
	CHECK_RET(fRegsArea.Get());

	fConfigAddr = (addr_t)regs + (addr_t)(configAddrPhys - regsBase);
	fConfigData = (addr_t)regs + (addr_t)(configDataPhys - regsBase);

	if (fHostBridgeType == PCI_HOST_BRIDGE_UNINORTH) {
		// UniNorth host-side windows. Each bridge's registers sit at its OF
		// "reg" base; the config regs are base+0x800000, so recover the base.
		// The near I/O and MMIO windows are relative to that base; the main
		// 32-bit MMIO window is 1:1, so device BARs (incl. mac-io) are
		// CPU-addressable. (BARs are mapped directly by their physical value,
		// so these ranges only need to be good enough for resource lookup.)
		phys_addr_t base = configAddrPhys - 0x800000;

		pci_resource_range& mmio = fRanges[fRangeCount++];
		mmio = {};
		mmio.type = B_IO_MEMORY;
		mmio.address_type = PCI_address_type_32;
		mmio.host_address = 0x80000000;
		mmio.pci_address = 0x80000000;
		mmio.size = 0x10000000;

		pci_resource_range& mmio2 = fRanges[fRangeCount++];
		mmio2 = {};
		mmio2.type = B_IO_MEMORY;
		mmio2.address_type = PCI_address_type_32;
		mmio2.host_address = base + 0x01000000;
		mmio2.pci_address = base + 0x01000000;
		mmio2.size = 0x01000000;

		pci_resource_range& io = fRanges[fRangeCount++];
		io = {};
		io.type = B_IO_PORT;
		io.host_address = base;
		io.pci_address = 0x00000000;
		io.size = 0x00800000;
	} else {
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
	}

	dprintf("of_pci: %s host bridge %u ready (config %#lx/%#lx)\n",
		fHostBridgeType == PCI_HOST_BRIDGE_UNINORTH ? "UniNorth" : "Grackle",
		(unsigned)fBridgeIndex, (addr_t)configAddrPhys, (addr_t)configDataPhys);

	// The GMAC Ethernet cell hangs off this bridge but is clock-gated until
	// enabled; do it now, before the PCI stack enumerates this domain.
	if (fHostBridgeType == PCI_HOST_BRIDGE_UNINORTH
			&& (addr_t)configAddrPhys == ETHERNET_BRIDGE_CONFIG_ADDR) {
		enable_gmac_clock();

		// The GMAC's config interrupt_line register is unrouted (reads 0xff)
		// on these Macs; the boot loader resolved its real OpenPIC input from
		// the OF interrupt-map. Program it so the network driver's
		// bus_alloc_resource(SYS_RES_IRQ) finds a usable vector. (ethernet@f
		// is device 15 on this bridge.)
		uint32 gmacIRQ = ppc_get_gmac_irq();
		if (gmacIRQ != 0 && gmacIRQ < 0xff)
			WriteConfig(0, 15, 0, 0x3c, 1, gmacIRQ);
	}

	return B_OK;
}


// #pragma mark - config space access (Grackle indirect)


void
OpenFirmwarePCIController::SetConfigAddress(uint8 bus, uint8 device,
	uint8 function, uint16 offset)
{
	uint32 address;
	if (fHostBridgeType == PCI_HOST_BRIDGE_UNINORTH) {
		// UniNorth: bus 0 uses a 1-hot IDSEL in the high bits; other buses
		// use type-1 config (low bit set).
		if (bus == 0) {
			address = (1u << device) | ((uint32)function << 8)
				| (offset & 0xFC);
		} else {
			address = ((uint32)bus << 16) | ((uint32)device << 11)
				| ((uint32)function << 8) | (offset & 0xFC) | 1;
		}
	} else {
		address = 0x80000000 | ((uint32)bus << 16)
			| ((uint32)device << 11) | ((uint32)function << 8)
			| (offset & 0xFC);
	}

	// Both bridges run PCI little-endian: a byte-reversed store latches the
	// natural CONFIG_ADDR value (equivalent to PowerPC out_le32).
	*(volatile uint32*)fConfigAddr = B_HOST_TO_LENDIAN_INT32(address);
	asm volatile("eieio" ::: "memory");
	if (fHostBridgeType == PCI_HOST_BRIDGE_UNINORTH) {
		// UniNorth returns garbage unless the address register is read back.
		(void)*(volatile uint32*)fConfigAddr;
		asm volatile("eieio" ::: "memory");
	}
}


status_t
OpenFirmwarePCIController::ReadConfig(uint8 bus, uint8 device, uint8 function,
	uint16 offset, uint8 size, uint32& value)
{
	if (fHostBridgeType == PCI_HOST_BRIDGE_UNINORTH && bus == 0
			&& device < 11) {
		value = 0xffffffff;
		return B_OK;
	}
	SetConfigAddress(bus, device, function, offset);

	// UniNorth exposes CONFIG_DATA as an 8-byte window: the register byte is
	// at cfg_data + (offset & 0x07), NOT (offset & 0x03). Getting this wrong
	// makes every odd dword (command @0x04, BAR1 @0x14, ...) alias the wrong
	// location - which is why the GMAC command register could not be written.
	addr_t data = fConfigData + (offset & fConfigDataMask);
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
	if (fHostBridgeType == PCI_HOST_BRIDGE_UNINORTH && bus == 0
			&& device < 11) {
		return B_OK;
	}
	if (size != 1 && size != 2 && size != 4)
		return B_BAD_VALUE;

	// The UniNorth host bridge only reliably accepts 32-bit writes to its
	// CONFIG_DATA register; a narrower store to a byte lane does not stick
	// (this is why enabling the GMAC's command-register memory bit silently
	// failed). Perform sub-dword writes as a 32-bit read-modify-write.
	// See ReadConfig: the register dword lives at cfg_data + (offset & mask)
	// where mask is 0x07 on UniNorth. The bridge only reliably accepts 32-bit
	// CONFIG_DATA stores, so sub-dword writes are done as a read-modify-write
	// of the containing 32-bit word.
	addr_t dwordAddr = fConfigData + (offset & fConfigDataMask & ~(addr_t)3);
	if (size == 4) {
		SetConfigAddress(bus, device, function, offset);
		*(volatile uint32*)dwordAddr = B_HOST_TO_LENDIAN_INT32(value);
		asm volatile("eieio" ::: "memory");
		return B_OK;
	}

	SetConfigAddress(bus, device, function, offset);
	uint32 dword = B_LENDIAN_TO_HOST_INT32(*(volatile uint32*)dwordAddr);
	uint32 shift = (uint32)(offset & 3) * 8;
	uint32 mask = ((size == 1) ? 0xffu : 0xffffu) << shift;
	dword = (dword & ~mask) | ((value << shift) & mask);
	SetConfigAddress(bus, device, function, offset);
	*(volatile uint32*)dwordAddr = B_HOST_TO_LENDIAN_INT32(dword);
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
