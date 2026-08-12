/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include <boot/vfs.h>
#include <boot/platform.h>
#include <boot/partitions.h>
#include <boot/stage2.h>
#include <string.h>
#include <stdio.h>

#include <gccore.h>
#include <ogc/ipc.h>

#include <platform/wii/wii_sdio.h>

// A virtual block device that exposes the tanka.img file
// residing on the physical Wii SD card.
// For now, we just expose the RAW SD card! The user will have to
// write the anyboot image directly to the SD card, bypassing FAT32!
// Later, we can add a FAT32 parser to find tanka.img.

// This drives /dev/sdio/slot0 itself instead of using libogc's DISC_INTERFACE,
// which cannot report the capacity the kernel matches the boot device against.

static const size_t kSectorSize = WII_SDIO_SECTOR_SIZE;
static const uint32 kBounceSectors = 8;

// not declared by any libogc header, but exported by the library
extern "C" void udelay(int microSeconds);


class WiiSDBootDevice : public Node {
public:
	WiiSDBootDevice();
	virtual ~WiiSDBootDevice();

	virtual status_t InitCheck();
	virtual ssize_t ReadAt(void *cookie, off_t pos, void *buffer, size_t bufferSize);
	virtual ssize_t WriteAt(void *cookie, off_t pos, const void *buffer, size_t bufferSize);
	virtual off_t Size() const;

private:
	bool fInitialized;
	off_t fSize;
};


// Not members: over-aligning one makes GCC want the aligned operator new,
// which drags libsupc++'s verbose terminate handler into the loader.
static char sDevicePath[] __attribute__((aligned(32))) = WII_SDIO_DEVICE;
static ioctlv sVectors[3] __attribute__((aligned(32)));
static wii_sdio_request sRequest __attribute__((aligned(32)));
static wii_sdio_response sResponse __attribute__((aligned(32)));
static uint32 sRegisterQuery[6] __attribute__((aligned(32)));
static uint32 sRegisterValue __attribute__((aligned(32)));
static uint32 sCardStatus __attribute__((aligned(32)));
static uint32 sClock __attribute__((aligned(32)));
static uint32 sCSD[4] __attribute__((aligned(32)));
static uint8 sBounceBuffer[kBounceSectors * kSectorSize] __attribute__((aligned(32)));

static int32 sFD = -1;
static uint16 sRCA = 0;
static bool sSDHC = false;


static status_t
sdio_send_command(uint32 command, uint32 type, uint32 responseType,
	uint32 argument, uint32 blockCount, uint32 blockSize, void *buffer)
{
	sRequest.cmd = command;
	sRequest.cmd_type = type;
	sRequest.rsp_type = responseType;
	sRequest.arg = argument;
	sRequest.blk_cnt = blockCount;
	sRequest.blk_size = blockSize;
	// IOS feeds this straight to the DMA engine, so it has to be physical.
	sRequest.dma_addr = buffer != NULL ? MEM_VIRTUAL_TO_PHYSICAL(buffer) : 0;
	sRequest.isdma = buffer != NULL ? 1 : 0;
	sRequest.pad0 = 0;

	int32 result;
	if (sRequest.isdma != 0 || sSDHC) {
		// IOS_Ioctlv translates the vectors in place, so refill them every time.
		sVectors[0].data = &sRequest;
		sVectors[0].len = sizeof(sRequest);
		sVectors[1].data = buffer;
		sVectors[1].len = blockSize * blockCount;
		sVectors[2].data = &sResponse;
		sVectors[2].len = sizeof(sResponse);
		result = IOS_Ioctlv(sFD, WII_SDIO_IOCTL_SENDCMD, 2, 1, sVectors);
	} else {
		result = IOS_Ioctl(sFD, WII_SDIO_IOCTL_SENDCMD, &sRequest,
			sizeof(sRequest), &sResponse, sizeof(sResponse));
	}

	return result < 0 ? B_IO_ERROR : B_OK;
}


static status_t
sdio_set_clock(uint32 clock)
{
	sClock = clock;
	if (IOS_Ioctl(sFD, WII_SDIO_IOCTL_SETCLK, &sClock, sizeof(sClock),
			NULL, 0) < 0)
		return B_IO_ERROR;

	return B_OK;
}


static status_t
sdio_get_status(uint32 *_status)
{
	if (IOS_Ioctl(sFD, WII_SDIO_IOCTL_GETSTATUS, NULL, 0, &sCardStatus,
			sizeof(sCardStatus)) < 0)
		return B_IO_ERROR;

	*_status = sCardStatus;
	return B_OK;
}


static status_t
sdio_reset_card(void)
{
	sRCA = 0;
	if (IOS_Ioctl(sFD, WII_SDIO_IOCTL_RESETCARD, NULL, 0, &sCardStatus,
			sizeof(sCardStatus)) < 0)
		return B_IO_ERROR;

	// The relative card address comes back in the upper half of the reply.
	sRCA = (uint16)(sCardStatus >> 16);
	return B_OK;
}


static status_t
sdio_get_register(uint8 reg, uint8 size, uint32 *_value)
{
	sRegisterValue = 0;
	sRegisterQuery[0] = reg;
	sRegisterQuery[1] = 0;
	sRegisterQuery[2] = 0;
	sRegisterQuery[3] = size;
	sRegisterQuery[4] = 0;
	sRegisterQuery[5] = 0;

	if (IOS_Ioctl(sFD, WII_SDIO_IOCTL_READHCREG, sRegisterQuery,
			sizeof(sRegisterQuery), &sRegisterValue,
			sizeof(sRegisterValue)) < 0)
		return B_IO_ERROR;

	*_value = sRegisterValue;
	return B_OK;
}


static status_t
sdio_set_register(uint8 reg, uint8 size, uint32 value)
{
	sRegisterQuery[0] = reg;
	sRegisterQuery[1] = 0;
	sRegisterQuery[2] = 0;
	sRegisterQuery[3] = size;
	sRegisterQuery[4] = value;
	sRegisterQuery[5] = 0;

	if (IOS_Ioctl(sFD, WII_SDIO_IOCTL_WRITEHCREG, sRegisterQuery,
			sizeof(sRegisterQuery), NULL, 0) < 0)
		return B_IO_ERROR;

	return B_OK;
}


static status_t
sdio_wait_register(uint8 reg, uint8 size, bool unset, uint32 mask)
{
	for (int32 tries = 0; tries < 10; tries++) {
		uint32 value;
		if (sdio_get_register(reg, size, &value) != B_OK)
			return B_IO_ERROR;
		if (((value & mask) == 0) == unset)
			return B_OK;

		udelay(10000);
	}

	return B_TIMED_OUT;
}


static status_t
sdio_select(void)
{
	return sdio_send_command(WII_SDIO_CMD_SELECT, WII_SDIO_TYPE_AC,
		WII_SDIO_RESPONSE_R1B, (uint32)sRCA << 16, 0, 0, NULL);
}


static status_t
sdio_deselect(void)
{
	return sdio_send_command(WII_SDIO_CMD_DESELECT, WII_SDIO_TYPE_AC,
		WII_SDIO_RESPONSE_R1B, 0, 0, 0, NULL);
}


static status_t
sdio_set_bus_width(uint32 width)
{
	uint32 value = width == 4
		? WII_SDIO_BUSWIDTH_4BIT : WII_SDIO_BUSWIDTH_1BIT;
	status_t status = sdio_send_command(WII_SDIO_CMD_APPCMD, WII_SDIO_TYPE_AC,
		WII_SDIO_RESPONSE_R1, (uint32)sRCA << 16, 0, 0, NULL);
	if (status != B_OK)
		return status;

	status = sdio_send_command(WII_SDIO_ACMD_SETBUSWIDTH, WII_SDIO_TYPE_AC,
		WII_SDIO_RESPONSE_R1, value, 0, 0, NULL);
	if (status != B_OK)
		return status;

	uint32 hostControl;
	status = sdio_get_register(WII_SDIOHCR_HOSTCONTROL, 1, &hostControl);
	if (status != B_OK)
		return status;

	hostControl &= 0xff;
	hostControl &= ~(uint32)WII_SDIOHCR_HOSTCONTROL_4BIT;
	if (width == 4)
		hostControl |= WII_SDIOHCR_HOSTCONTROL_4BIT;

	return sdio_set_register(WII_SDIOHCR_HOSTCONTROL, 1, hostControl);
}


static status_t
sdio_host_controller_init(void)
{
	// Reopening the handle makes IOS clean up after its own failed probe.
	IOS_Close(sFD);
	sFD = IOS_Open(sDevicePath, 1);
	if (sFD < 0)
		return B_ERROR;

	if (sdio_set_register(WII_SDIOHCR_SOFTWARERESET, 1, 7) != B_OK
		|| sdio_wait_register(WII_SDIOHCR_SOFTWARERESET, 1, true, 7) != B_OK)
		return B_ERROR;

	// Interrupt setup a successful RESETCARD would have done for us.
	sdio_set_register(0x34, 4, 0x13f00c3);
	sdio_set_register(0x38, 4, 0x13f00c3);

	// Every command from here on carries the DMA vector shape IOS expects.
	sSDHC = true;

	if (sdio_set_register(WII_SDIOHCR_POWERCONTROL, 1, 0x0e) != B_OK
		|| sdio_set_register(WII_SDIOHCR_POWERCONTROL, 1, 0x0f) != B_OK)
		return B_ERROR;

	// Start the internal clock, wait for it to settle, then run the SD clock.
	if (sdio_set_register(WII_SDIOHCR_CLOCKCONTROL, 2, 0) != B_OK
		|| sdio_set_register(WII_SDIOHCR_CLOCKCONTROL, 2, 0x101) != B_OK
		|| sdio_wait_register(WII_SDIOHCR_CLOCKCONTROL, 2, false,
			WII_SDIOHCR_CLOCK_STABLE) != B_OK
		|| sdio_set_register(WII_SDIOHCR_CLOCKCONTROL, 2, 0x107) != B_OK)
		return B_ERROR;

	if (sdio_set_register(WII_SDIOHCR_TIMEOUTCONTROL, 1,
			WII_SDIO_DEFAULT_TIMEOUT) != B_OK)
		return B_ERROR;

	if (sdio_send_command(WII_SDIO_CMD_GOIDLE, 0, WII_SDIO_RESPONSE_NONE, 0,
			0, 0, NULL) != B_OK)
		return B_ERROR;

	if (sdio_send_command(WII_SDIO_CMD_SENDIFCOND, 0, WII_SDIO_RESPONSE_R6,
			WII_SDIO_IFCOND_ARG, 0, 0, NULL) != B_OK)
		return B_ERROR;
	if ((sResponse.rsp_fields[0] & 0xff) != WII_SDIO_IFCOND_PATTERN)
		return B_ERROR;

	int32 tries = 10;
	while (tries-- > 0) {
		if (sdio_send_command(WII_SDIO_CMD_APPCMD, WII_SDIO_TYPE_AC,
				WII_SDIO_RESPONSE_R1, 0, 0, 0, NULL) != B_OK)
			return B_ERROR;
		if (sdio_send_command(WII_SDIO_ACMD_SENDOPCOND, 0, WII_SDIO_RESPONSE_R3,
				WII_SDIO_OPCOND_ARG, 0, 0, NULL) != B_OK)
			return B_ERROR;
		if ((sResponse.rsp_fields[0] & WII_SDIO_OCR_BUSY) != 0)
			break;

		udelay(10000);
	}
	if (tries < 0)
		return B_ERROR;

	// Cards that answer without the capacity bit stay on byte addressing.
	sSDHC = (sResponse.rsp_fields[0] & WII_SDIO_OCR_CCS) != 0;

	if (sdio_send_command(WII_SDIO_CMD_ALL_SENDCID, 0, WII_SDIO_RESPONSE_R2,
			(uint32)sRCA << 16, 0, 0, NULL) != B_OK)
		return B_ERROR;

	if (sdio_send_command(WII_SDIO_CMD_SENDRCA, 0, WII_SDIO_RESPONSE_R5, 0,
			0, 0, NULL) != B_OK)
		return B_ERROR;

	sRCA = (uint16)(sResponse.rsp_fields[0] >> 16);
	return B_OK;
}


static void
sdio_uninit(void)
{
	if (sFD >= 0)
		IOS_Close(sFD);

	sFD = -1;
}


static status_t
sdio_init(void)
{
	sRCA = 0;
	sSDHC = false;

	sFD = IOS_Open(sDevicePath, 1);
	if (sFD < 0)
		return B_ERROR;

	sdio_reset_card();

	uint32 status;
	if (sdio_get_status(&status) != B_OK)
		return B_ERROR;
	if ((status & WII_SDIO_STATUS_CARD_INSERTED) == 0)
		return B_ENTRY_NOT_FOUND;

	if ((status & WII_SDIO_STATUS_CARD_INITIALIZED) == 0) {
		// IOS does not like this card, so bring the host controller up by hand.
		if (sdio_host_controller_init() != B_OK) {
			sdio_set_register(WII_SDIOHCR_SOFTWARERESET, 1, 7);
			sdio_wait_register(WII_SDIOHCR_SOFTWARERESET, 1, true, 7);
			IOS_Close(sFD);
			sFD = IOS_Open(sDevicePath, 1);
			return B_ERROR;
		}
	} else
		sSDHC = (status & WII_SDIO_STATUS_CARD_SDHC) != 0;

	// CMD9 is only legal in standby, so ask before CMD7 enters transfer state.
	if (sdio_send_command(WII_SDIO_CMD_SENDCSD, WII_SDIO_TYPE_AC,
			WII_SDIO_RESPONSE_R2, (uint32)sRCA << 16, 0, 0, NULL) != B_OK)
		return B_ERROR;
	memcpy(sCSD, &sResponse, sizeof(sCSD));

	if (sdio_select() != B_OK)
		return B_ERROR;

	if (sdio_send_command(WII_SDIO_CMD_SETBLOCKLEN, WII_SDIO_TYPE_AC,
			WII_SDIO_RESPONSE_R1, kSectorSize, 0, 0, NULL) != B_OK) {
		sdio_deselect();
		return B_ERROR;
	}

	if (sdio_set_bus_width(4) != B_OK) {
		sdio_deselect();
		return B_ERROR;
	}

	if (sdio_set_clock(1) != B_OK) {
		sdio_deselect();
		return B_ERROR;
	}

	sdio_deselect();
	return B_OK;
}


static status_t
sdio_read_sectors(uint32 sector, uint32 count, void *buffer)
{
	status_t status = sdio_select();
	if (status != B_OK)
		return status;

	if (((addr_t)buffer & 0x1f) != 0) {
		// IOS DMAs whole cache lines, so misaligned targets need a bounce.
		uint8 *out = (uint8 *)buffer;
		while (count > 0) {
			uint32 chunk = count > kBounceSectors ? kBounceSectors : count;
			uint32 address = sSDHC ? sector : sector * kSectorSize;

			status = sdio_send_command(WII_SDIO_CMD_READMULTIBLOCK,
				WII_SDIO_TYPE_AC, WII_SDIO_RESPONSE_R1, address, chunk,
				kSectorSize, sBounceBuffer);
			if (status != B_OK)
				break;

			memcpy(out, sBounceBuffer, chunk * kSectorSize);
			out += chunk * kSectorSize;
			sector += chunk;
			count -= chunk;
		}
	} else {
		uint32 address = sSDHC ? sector : sector * kSectorSize;
		status = sdio_send_command(WII_SDIO_CMD_READMULTIBLOCK,
			WII_SDIO_TYPE_AC, WII_SDIO_RESPONSE_R1, address, count,
			kSectorSize, buffer);
	}

	sdio_deselect();
	return status;
}


WiiSDBootDevice::WiiSDBootDevice()
	:
	fInitialized(false),
	fSize(0)
{
	if (sdio_init() != B_OK) {
		sdio_uninit();
		return;
	}

	fInitialized = true;
	fSize = (off_t)wii_sdio_csd_capacity(sCSD);
}


WiiSDBootDevice::~WiiSDBootDevice()
{
	if (fInitialized)
		sdio_uninit();
}


status_t
WiiSDBootDevice::InitCheck()
{
	return fInitialized ? B_OK : B_ENTRY_NOT_FOUND;
}


ssize_t
WiiSDBootDevice::ReadAt(void *cookie, off_t pos, void *buffer, size_t bufferSize)
{
	if (!fInitialized)
		return B_NO_INIT;

	if (pos < 0)
		return B_BAD_VALUE;

	// The partition scanners read single fields, not sectors, so anything that
	// is not whole-sector aligned has to go through a bounce buffer.
	uint8 *out = (uint8 *)buffer;
	size_t remaining = bufferSize;

	while (remaining > 0) {
		uint32 sector = pos / kSectorSize;
		size_t offset = pos % kSectorSize;
		size_t chunk;

		if (offset == 0 && remaining >= kSectorSize) {
			uint32 count = remaining / kSectorSize;
			if (sdio_read_sectors(sector, count, out) != B_OK)
				return B_IO_ERROR;
			chunk = count * kSectorSize;
		} else {
			if (sdio_read_sectors(sector, 1, sBounceBuffer) != B_OK)
				return B_IO_ERROR;
			chunk = kSectorSize - offset;
			if (chunk > remaining)
				chunk = remaining;
			memcpy(out, sBounceBuffer + offset, chunk);
		}

		out += chunk;
		pos += chunk;
		remaining -= chunk;
	}

	return bufferSize;
}


ssize_t
WiiSDBootDevice::WriteAt(void *cookie, off_t pos, const void *buffer, size_t bufferSize)
{
	// Bootloader does not need write support right now
	return B_NOT_SUPPORTED;
}


off_t
WiiSDBootDevice::Size() const
{
	return fSize;
}


//	#pragma mark -


status_t
platform_add_boot_device(struct stage2_args *args, NodeList *devicesList)
{
	WiiSDBootDevice* device = new(std::nothrow) WiiSDBootDevice();
	if (device == NULL)
		return B_NO_MEMORY;

	if (device->InitCheck() != B_OK) {
		printf("platform_add_boot_device: SD card not found!\n");
		delete device;
		return B_ENTRY_NOT_FOUND;
	}

	// The kernel matches its own SD capacity against this, so log what we saw.
	printf("platform_add_boot_device: SD card is %llu bytes\n",
		(unsigned long long)device->Size());

	devicesList->Add(device);
	return B_OK;
}

status_t
platform_add_block_devices(struct stage2_args *args, NodeList *devicesList)
{
	return B_OK;
}

status_t
platform_register_boot_device(Node *device)
{
	disk_identifier disk;
	memset(&disk, 0, sizeof(disk_identifier));

	disk.bus_type = UNKNOWN_BUS;
	disk.device_type = UNKNOWN_DEVICE;
	disk.device.unknown.size = device->Size();

	for (int32 i = 0; i < NUM_DISK_CHECK_SUMS; i++) {
		disk.device.unknown.check_sums[i].offset = -1;
		disk.device.unknown.check_sums[i].sum = 0;
	}

	gBootParams.SetData(BOOT_VOLUME_DISK_IDENTIFIER, B_RAW_TYPE, &disk,
		sizeof(disk_identifier));

	return B_OK;
}

status_t
platform_get_boot_partitions(struct stage2_args *args, Node *device,
	NodeList *list, NodeList *partitionList)
{
	NodeIterator iterator = list->GetIterator();
	boot::Partition *partition = NULL;
	status_t status = B_ENTRY_NOT_FOUND;
	while ((partition = (boot::Partition *)iterator.Next()) != NULL) {
		partitionList->Insert(partition);
		status = B_OK;
	}

	return status;
}
