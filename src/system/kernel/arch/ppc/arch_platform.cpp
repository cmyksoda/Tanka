/*
 * Copyright 2006, Ingo Weinhold <bonefish@cs.tu-berlin.de>.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include <arch_platform.h>

#include <new>

#include <KernelExport.h>

#include <arch/generic/debug_uart.h>
#include <boot/kernel_args.h>
#include <platform/openfirmware/openfirmware.h>
#include <real_time_clock.h>
#include <util/kernel_cpp.h>
#include <vm/vm.h>


void *gFDT;
static PPCPlatform *sPPCPlatform;


PPCPlatform::PPCPlatform(ppc_platform_type platformType)
	: fPlatformType(platformType)
{
}


PPCPlatform::~PPCPlatform()
{
}


PPCPlatform *
PPCPlatform::Default()
{
	return sPPCPlatform;
}


// #pragma mark - Open Firmware


namespace BPrivate {

class PPCOpenFirmware : public PPCPlatform {
public:
	PPCOpenFirmware();
	virtual ~PPCOpenFirmware();

	virtual status_t Init(struct kernel_args *kernelArgs);
	virtual status_t InitSerialDebug(struct kernel_args *kernelArgs);
	virtual status_t InitPostVM(struct kernel_args *kernelArgs);
	virtual status_t InitRTC(struct kernel_args *kernelArgs,
		struct real_time_data *data);

	virtual char SerialDebugGetChar();
	virtual void SerialDebugPutChar(char c);

	virtual	void SetHardwareRTC(uint64 seconds);
	virtual	uint32 GetHardwareRTC();

	virtual	void ShutDown(bool reboot);

private:
	int	fInput;
	int	fOutput;
	int	fRTC;
};

}	// namespace BPrivate


using BPrivate::PPCOpenFirmware;


// OF debugger commands


static int
debug_command_of_exit(int argc, char **argv)
{
	of_exit();
	kprintf("of_exit() failed!\n");
	return 0;
}


static int
debug_command_of_enter(int argc, char **argv)
{
	of_call_client_function("enter", 0, 0);
	return 0;
}


PPCOpenFirmware::PPCOpenFirmware()
	: PPCPlatform(PPC_PLATFORM_OPEN_FIRMWARE),
	  fInput(-1),
	  fOutput(-1),
	  fRTC(-1)
{
}


PPCOpenFirmware::~PPCOpenFirmware()
{
}


status_t
PPCOpenFirmware::Init(struct kernel_args *kernelArgs)
{
	return of_init(
		(intptr_t(*)(void*))kernelArgs->platform_args.openfirmware_entry);
}


status_t
PPCOpenFirmware::InitSerialDebug(struct kernel_args *kernelArgs)
{
	if (of_getprop(gChosen, "stdin", &fInput, sizeof(int)) == OF_FAILED)
		return B_ERROR;
	// Always take OpenFirmware's stdout for the kernel debug console, even when
	// a framebuffer is enabled. When the firmware's output-device is a serial
	// line (our dingusppc/QEMU setup uses output-device=scca) this yields
	// kernel dprintf/panic/KDL over serial *and* a usable framebuffer for
	// app_server at the same time. The old code only took stdout when the
	// framebuffer was disabled, assuming stdout is always the screen - which is
	// untrue whenever output-device points at a UART, and is exactly why a boot
	// splash / framebuffer used to have to be disabled to get any serial log.
	if (of_getprop(gChosen, "stdout", &fOutput, sizeof(int)) == OF_FAILED)
		return B_ERROR;

	return B_OK;
}


status_t
PPCOpenFirmware::InitPostVM(struct kernel_args *kernelArgs)
{
	add_debugger_command("of_exit", &debug_command_of_exit,
		"Exit to the Open Firmware prompt. No way to get back into the OS!");
	add_debugger_command("of_enter", &debug_command_of_enter,
		"Enter a subordinate Open Firmware interpreter. Quitting it returns "
		"to KDL.");

	return B_OK;
}


// InitRTC
status_t
PPCOpenFirmware::InitRTC(struct kernel_args *kernelArgs,
	struct real_time_data *data)
{
	// open RTC
	fRTC = of_open(kernelArgs->platform_args.rtc_path);
	if (fRTC == OF_FAILED) {
		dprintf("PPCOpenFirmware::InitRTC(): Failed open RTC device!\n");
		return B_ERROR;
	}

	return B_OK;
}


char
PPCOpenFirmware::SerialDebugGetChar()
{
	int key;
	if (of_interpret("key", 0, 1, &key) == OF_FAILED)
		return 0;
	return (char)key;
}


void
PPCOpenFirmware::SerialDebugPutChar(char c)
{
	if (fOutput == -1)
		return;

	if (c == '\n')
		of_write(fOutput, "\r\n", 2);
	else
		of_write(fOutput, &c, 1);
}


void
PPCOpenFirmware::SetHardwareRTC(uint64 seconds)
{
	struct tm t;
	rtc_secs_to_tm(seconds, &t);

	t.tm_year += RTC_EPOCH_BASE_YEAR;
	t.tm_mon++;

	if (of_call_method(fRTC, "set-time", 6, 0, t.tm_year, t.tm_mon, t.tm_mday,
			t.tm_hour, t.tm_min, t.tm_sec) == OF_FAILED) {
		dprintf("PPCOpenFirmware::SetHardwareRTC(): Failed to set RTC!\n");
	}
}


uint32
PPCOpenFirmware::GetHardwareRTC()
{
	struct tm t;
	if (of_call_method(fRTC, "get-time", 0, 6, &t.tm_year, &t.tm_mon,
			&t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) == OF_FAILED) {
		dprintf("PPCOpenFirmware::GetHardwareRTC(): Failed to get RTC!\n");
		return 0;
	}

	t.tm_year -= RTC_EPOCH_BASE_YEAR;
	t.tm_mon--;

	return rtc_tm_to_secs(&t);
}


void
PPCOpenFirmware::ShutDown(bool reboot)
{
	if (reboot) {
		of_interpret("reset-all", 0, 0);
	} else {
		// not standardized, so it might fail
		of_interpret("shut-down", 0, 0);
	}
}


// #pragma mark - U-Boot + FDT


namespace BPrivate {

class PPCUBoot : public PPCPlatform {
public:
	PPCUBoot();
	virtual ~PPCUBoot();

	virtual status_t Init(struct kernel_args *kernelArgs);
	virtual status_t InitSerialDebug(struct kernel_args *kernelArgs);
	virtual status_t InitPostVM(struct kernel_args *kernelArgs);
	virtual status_t InitRTC(struct kernel_args *kernelArgs,
		struct real_time_data *data);

	virtual char SerialDebugGetChar();
	virtual void SerialDebugPutChar(char c);

	virtual	void SetHardwareRTC(uint64 seconds);
	virtual	uint32 GetHardwareRTC();

	virtual	void ShutDown(bool reboot);

private:
	int	fInput;
	int	fOutput;
	int	fRTC;
	DebugUART *fDebugUART;
};

}	// namespace BPrivate

using BPrivate::PPCUBoot;


PPCUBoot::PPCUBoot()
	: PPCPlatform(PPC_PLATFORM_U_BOOT),
	  fInput(-1),
	  fOutput(-1),
	  fRTC(-1),
	  fDebugUART(NULL)
{
}


PPCUBoot::~PPCUBoot()
{
}


status_t
PPCUBoot::Init(struct kernel_args *kernelArgs)
{
	gFDT = kernelArgs->platform_args.fdt;
	// XXX: do we error out if no FDT?
	return B_OK;
}


status_t
PPCUBoot::InitSerialDebug(struct kernel_args *kernelArgs)
{
	// TODO: get relevant debug uart from fdt
	//fDebugUART = debug_uart_from_fdt(gFDT);
	if (fDebugUART == NULL)
		return B_ERROR;
	return B_OK;
}


status_t
PPCUBoot::InitPostVM(struct kernel_args *kernelArgs)
{
	return B_ERROR;
}


status_t
PPCUBoot::InitRTC(struct kernel_args *kernelArgs,
	struct real_time_data *data)
{
	return B_ERROR;
}


char
PPCUBoot::SerialDebugGetChar()
{
	if (fDebugUART)
		return fDebugUART->GetChar(false);
	return 0;
}


void
PPCUBoot::SerialDebugPutChar(char c)
{
	if (fDebugUART)
		fDebugUART->PutChar(c);
}


void
PPCUBoot::SetHardwareRTC(uint64 seconds)
{
}


uint32
PPCUBoot::GetHardwareRTC()
{
	return 0;
}


void
PPCUBoot::ShutDown(bool reboot)
{
}


// #pragma mark - Wii

namespace BPrivate {

class PPCWii : public PPCPlatform {
public:
	PPCWii();
	virtual ~PPCWii();

	virtual status_t Init(struct kernel_args *kernelArgs);
	virtual status_t InitSerialDebug(struct kernel_args *kernelArgs);
	virtual status_t InitPostVM(struct kernel_args *kernelArgs);
	virtual status_t InitRTC(struct kernel_args *kernelArgs,
		struct real_time_data *data);

	virtual char SerialDebugGetChar();
	virtual void SerialDebugPutChar(char c);

	virtual void SetHardwareRTC(uint64 seconds);
	virtual uint32 GetHardwareRTC();

	virtual void ShutDown(bool reboot);

private:
	static int32 VideoThread(void* arg);
	
	area_id fFakeFrameBufferArea;
	area_id fRealFrameBufferArea;
	void* fFakeFrameBuffer;
	void* fRealFrameBuffer;
	int fFrameBufferWidth;
	int fFrameBufferHeight;
};

}	// namespace BPrivate
using BPrivate::PPCWii;

PPCWii::PPCWii()
	:
	PPCPlatform(PPC_PLATFORM_WII),
	fFakeFrameBufferArea(-1),
	fRealFrameBufferArea(-1),
	fFakeFrameBuffer(NULL),
	fRealFrameBuffer(NULL),
	fFrameBufferWidth(0),
	fFrameBufferHeight(0)
{
}


PPCWii::~PPCWii()
{
}


status_t
PPCWii::Init(struct kernel_args *kernelArgs)
{
	return B_OK;
}


status_t
PPCWii::InitSerialDebug(struct kernel_args *kernelArgs)
{
	return B_OK;
}


status_t
PPCWii::InitPostVM(struct kernel_args *kernelArgs)
{
	// Map the fake RGB32 framebuffer (which app_server draws to)
	fFrameBufferWidth = kernelArgs->frame_buffer.width;
	fFrameBufferHeight = kernelArgs->frame_buffer.height;
	size_t fakeSize = fFrameBufferWidth * fFrameBufferHeight * 4;
	
	fFakeFrameBufferArea = map_physical_memory("wii fake rgb framebuffer",
		kernelArgs->frame_buffer.physical_buffer.start, fakeSize,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &fFakeFrameBuffer);
		
	if (fFakeFrameBufferArea < 0) {
		dprintf("PPCWii: Failed to map fake framebuffer\n");
		return B_ERROR;
	}

	// Map the real hardware YUYV framebuffer
	size_t realSize = kernelArgs->arch_args.wii_hardware_framebuffer.size;
	fRealFrameBufferArea = map_physical_memory("wii real yuyv framebuffer",
		kernelArgs->arch_args.wii_hardware_framebuffer.start, realSize,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &fRealFrameBuffer);
		
	if (fRealFrameBufferArea < 0) {
		dprintf("PPCWii: Failed to map real framebuffer\n");
		return B_ERROR;
	}

	// Spawn the 60Hz conversion thread
	thread_id thread = spawn_kernel_thread(VideoThread, "wii video conversion",
		B_DISPLAY_PRIORITY, this);
	if (thread >= 0)
		resume_thread(thread);

	return B_OK;
}


int32
PPCWii::VideoThread(void* arg)
{
	PPCWii* self = (PPCWii*)arg;
	
	while (true) {
		// Wait ~16ms (60 FPS)
		snooze(16666);
		
		uint8* src = (uint8*)self->fFakeFrameBuffer;
		uint8* dst = (uint8*)self->fRealFrameBuffer;
		
		int pixels = self->fFrameBufferWidth * self->fFrameBufferHeight;
		
		// Convert RGB32 to YUYV (YUV422)
		// Haiku B_RGB32 is usually B G R A in memory (little endian) or A R G B (big endian)
		// On PowerPC, B_RGB32 is stored as A R G B (alpha byte at offset 0).
		for (int i = 0; i < pixels; i += 2) {
			uint8 r0 = src[i*4 + 1];
			uint8 g0 = src[i*4 + 2];
			uint8 b0 = src[i*4 + 3];
			
			uint8 r1 = src[(i+1)*4 + 1];
			uint8 g1 = src[(i+1)*4 + 2];
			uint8 b1 = src[(i+1)*4 + 3];
			
			// Approximation formulas:
			// Y = (77*R + 150*G + 29*B) >> 8
			// U = ((-43*R - 85*G + 128*B) >> 8) + 128
			// V = ((128*R - 107*G - 21*B) >> 8) + 128
			
			int y0 = (77 * r0 + 150 * g0 + 29 * b0) >> 8;
			int y1 = (77 * r1 + 150 * g1 + 29 * b1) >> 8;
			
			// Average R, G, B for U and V
			int r_avg = (r0 + r1) / 2;
			int g_avg = (g0 + g1) / 2;
			int b_avg = (b0 + b1) / 2;
			
			int u = ((-43 * r_avg - 85 * g_avg + 128 * b_avg) >> 8) + 128;
			int v = ((128 * r_avg - 107 * g_avg - 21 * b_avg) >> 8) + 128;
			
			// Clamp values
			if (y0 > 255) { y0 = 255; } else if (y0 < 0) { y0 = 0; }
			if (y1 > 255) { y1 = 255; } else if (y1 < 0) { y1 = 0; }
			if (u > 255) { u = 255; } else if (u < 0) { u = 0; }
			if (v > 255) { v = 255; } else if (v < 0) { v = 0; }
			
			// YUYV format: Y0 U0 Y1 V0
			dst[i*2 + 0] = y0;
			dst[i*2 + 1] = u;
			dst[i*2 + 2] = y1;
			dst[i*2 + 3] = v;
		}
	}
	
	return 0;
}


status_t
PPCWii::InitRTC(struct kernel_args *kernelArgs,
	struct real_time_data *data)
{
	return B_OK;
}


char
PPCWii::SerialDebugGetChar()
{
	return 0;
}


void
PPCWii::SerialDebugPutChar(char c)
{
}


void
PPCWii::SetHardwareRTC(uint64 seconds)
{
}


uint32
PPCWii::GetHardwareRTC()
{
	return 0;
}


void
PPCWii::ShutDown(bool reboot)
{
}


// # pragma mark -


#define PLATFORM_BUFFER_SIZE MAX(MAX(sizeof(PPCOpenFirmware),sizeof(PPCUBoot)),sizeof(PPCWii))
// static buffer for constructing the actual PPCPlatform
static char *sPPCPlatformBuffer[PLATFORM_BUFFER_SIZE];

// PCI host bridge info, captured by the boot loader (see arch_kernel_args).
// Legacy single-bridge fields mirror bridge 0 (the boot bridge).
static uint32 sPCIHostBridgeType = 0;
static phys_addr_t sPCIConfigAddress = 0xfec00000;
static phys_addr_t sPCIConfigData = 0xfee00000;

// All PCI host bridges (a Power Mac G4 has several UniNorth buses).
static uint32 sPCIHostBridgeCount = 0;
static struct {
	uint32		type;
	phys_addr_t	configAddress;
	phys_addr_t	configData;
} sPCIHostBridges[MAX_PCI_HOST_BRIDGES];

static uint32 sGmacIRQ = 0;
static uint8 sGmacMAC[6] = { 0, 0, 0, 0, 0, 0 };
static bool sGmacMACValid = false;


extern "C" void
ppc_get_pci_host_bridge(uint32* type, phys_addr_t* configAddress,
	phys_addr_t* configData)
{
	*type = sPCIHostBridgeType;
	*configAddress = sPCIConfigAddress;
	*configData = sPCIConfigData;
}

extern "C" uint32
ppc_get_pci_host_bridge_count()
{
	return sPCIHostBridgeCount;
}

extern "C" status_t
ppc_get_pci_host_bridge_at(uint32 index, uint32* type,
	phys_addr_t* configAddress, phys_addr_t* configData)
{
	if (index >= sPCIHostBridgeCount)
		return B_BAD_INDEX;
	*type = sPCIHostBridges[index].type;
	*configAddress = sPCIHostBridges[index].configAddress;
	*configData = sPCIHostBridges[index].configData;
	return B_OK;
}

extern "C" uint32
ppc_get_gmac_irq()
{
	return sGmacIRQ;
}

extern "C" bool
ppc_get_gmac_mac(uint8* address)
{
	if (!sGmacMACValid)
		return false;
	for (int i = 0; i < 6; i++)
		address[i] = sGmacMAC[i];
	return true;
}


status_t
arch_platform_init(struct kernel_args *kernelArgs)
{
	// only OpenFirmware supported for now
	switch (kernelArgs->arch_args.platform) {
		case PPC_PLATFORM_OPEN_FIRMWARE:
			sPPCPlatform = new(sPPCPlatformBuffer) PPCOpenFirmware;
			break;
		case PPC_PLATFORM_U_BOOT:
			sPPCPlatform = new(sPPCPlatformBuffer) PPCUBoot;
			break;
		case PPC_PLATFORM_WII:
			sPPCPlatform = new(sPPCPlatformBuffer) PPCWii;
			break;
		default:
			return B_ERROR;
	}

	sPCIHostBridgeType = kernelArgs->arch_args.pci_host_bridge_type;
	sPCIConfigAddress = kernelArgs->arch_args.pci_config_address;
	sPCIConfigData = kernelArgs->arch_args.pci_config_data;
	sGmacIRQ = kernelArgs->arch_args.gmac_irq;
	sGmacMACValid = kernelArgs->arch_args.gmac_mac_valid != 0;
	for (int i = 0; i < 6; i++)
		sGmacMAC[i] = kernelArgs->arch_args.gmac_mac[i];

	uint32 bridgeCount = kernelArgs->arch_args.pci_host_bridge_count;
	if (bridgeCount == 0 || bridgeCount > MAX_PCI_HOST_BRIDGES) {
		// Old loader (or none captured): synthesize bridge 0 from the
		// legacy fields so single-bridge machines keep working.
		sPCIHostBridgeCount = 1;
		sPCIHostBridges[0].type = sPCIHostBridgeType;
		sPCIHostBridges[0].configAddress = sPCIConfigAddress;
		sPCIHostBridges[0].configData = sPCIConfigData;
	} else {
		sPCIHostBridgeCount = bridgeCount;
		for (uint32 i = 0; i < bridgeCount; i++) {
			sPCIHostBridges[i].type
				= kernelArgs->arch_args.pci_host_bridges[i].type;
			sPCIHostBridges[i].configAddress
				= kernelArgs->arch_args.pci_host_bridges[i].config_address;
			sPCIHostBridges[i].configData
				= kernelArgs->arch_args.pci_host_bridges[i].config_data;
		}
	}

	return sPPCPlatform->Init(kernelArgs);
}


status_t
arch_platform_init_post_vm(struct kernel_args *kernelArgs)
{
	return sPPCPlatform->InitPostVM(kernelArgs);
}


status_t
arch_platform_init_post_thread(struct kernel_args *kernelArgs)
{
	return B_OK;
}
