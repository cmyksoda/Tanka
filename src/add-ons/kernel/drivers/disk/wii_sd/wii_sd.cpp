/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <Drivers.h>
#include <KernelExport.h>
#include <string.h>

#include <lock.h>
#include <util/AutoLock.h>

#include <platform/wii/wii.h>
#include <platform/wii/wii_sdio.h>


// The raw SD card, driven through IOS the same way the boot loader does.

#define DEVICE_NAME				"disk/wii_sd/0/raw"

// One transfer is staged whole by the IPC layer, so stay inside its buffer.
#define MAX_TRANSFER_SECTORS	128
#define DMA_BUFFER_SIZE			(MAX_TRANSFER_SECTORS * WII_SDIO_SECTOR_SIZE)

#define RETRY_COUNT				10
#define RETRY_DELAY				10000


int32 api_version = B_CUR_DRIVER_API_VERSION;

static const size_t kSectorSize = WII_SDIO_SECTOR_SIZE;

static mutex sLock = MUTEX_INITIALIZER("wii_sd");
static int32 sFD = -1;
static uint16 sRCA = 0;
static bool sSDHC = false;
static uint64 sCapacity = 0;

static area_id sDMAArea = -1;
static uint8 *sDMABuffer = NULL;
static uint32 sDMAPhysical = 0;

static wii_sdio_request sRequest;
static wii_sdio_response sResponse;
static uint32 sRegisterQuery[6];
static uint32 sCSD[4];


static status_t
sdio_send_command(uint32 command, uint32 type, uint32 responseType,
	uint32 argument, uint32 blockCount, uint32 blockSize, bool dma)
{
	sRequest.cmd = command;
	sRequest.cmd_type = type;
	sRequest.rsp_type = responseType;
	sRequest.arg = argument;
	sRequest.blk_cnt = blockCount;
	sRequest.blk_size = blockSize;
	// The data lands wherever this points, so it has to be a real address.
	sRequest.dma_addr = dma ? sDMAPhysical : 0;
	sRequest.isdma = dma ? 1 : 0;
	sRequest.pad0 = 0;

	memset(&sResponse, 0, sizeof(sResponse));

	int32 result;
	if (dma || sSDHC) {
		wii_ios_vector vectors[3];
		vectors[0].buffer = &sRequest;
		vectors[0].size = sizeof(sRequest);
		vectors[1].buffer = dma ? sDMABuffer : NULL;
		vectors[1].size = dma ? blockSize * blockCount : 0;
		vectors[2].buffer = &sResponse;
		vectors[2].size = sizeof(sResponse);
		result = wii_ios_ioctlv(sFD, WII_SDIO_IOCTL_SENDCMD, 2, 1, vectors);
	} else {
		result = wii_ios_ioctl(sFD, WII_SDIO_IOCTL_SENDCMD, &sRequest,
			sizeof(sRequest), &sResponse, sizeof(sResponse));
	}

	return result < 0 ? B_IO_ERROR : B_OK;
}


static status_t
sdio_set_clock(uint32 clock)
{
	if (wii_ios_ioctl(sFD, WII_SDIO_IOCTL_SETCLK, &clock, sizeof(clock),
			NULL, 0) < 0)
		return B_IO_ERROR;

	return B_OK;
}


static status_t
sdio_get_status(uint32 *_status)
{
	uint32 status = 0;
	if (wii_ios_ioctl(sFD, WII_SDIO_IOCTL_GETSTATUS, NULL, 0, &status,
			sizeof(status)) < 0)
		return B_IO_ERROR;

	*_status = status;
	return B_OK;
}


static status_t
sdio_reset_card(void)
{
	uint32 status = 0;
	sRCA = 0;
	if (wii_ios_ioctl(sFD, WII_SDIO_IOCTL_RESETCARD, NULL, 0, &status,
			sizeof(status)) < 0)
		return B_IO_ERROR;

	// The relative card address comes back in the upper half of the reply.
	sRCA = (uint16)(status >> 16);
	return B_OK;
}


static status_t
sdio_get_register(uint8 reg, uint8 size, uint32 *_value)
{
	uint32 value = 0;
	sRegisterQuery[0] = reg;
	sRegisterQuery[1] = 0;
	sRegisterQuery[2] = 0;
	sRegisterQuery[3] = size;
	sRegisterQuery[4] = 0;
	sRegisterQuery[5] = 0;

	if (wii_ios_ioctl(sFD, WII_SDIO_IOCTL_READHCREG, sRegisterQuery,
			sizeof(sRegisterQuery), &value, sizeof(value)) < 0)
		return B_IO_ERROR;

	*_value = value;
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

	if (wii_ios_ioctl(sFD, WII_SDIO_IOCTL_WRITEHCREG, sRegisterQuery,
			sizeof(sRegisterQuery), NULL, 0) < 0)
		return B_IO_ERROR;

	return B_OK;
}


static status_t
sdio_wait_register(uint8 reg, uint8 size, bool unset, uint32 mask)
{
	for (int32 tries = 0; tries < RETRY_COUNT; tries++) {
		uint32 value;
		if (sdio_get_register(reg, size, &value) != B_OK)
			return B_IO_ERROR;
		if (((value & mask) == 0) == unset)
			return B_OK;

		snooze(RETRY_DELAY);
	}

	return B_TIMED_OUT;
}


static status_t
sdio_select(void)
{
	return sdio_send_command(WII_SDIO_CMD_SELECT, WII_SDIO_TYPE_AC,
		WII_SDIO_RESPONSE_R1B, (uint32)sRCA << 16, 0, 0, false);
}


static status_t
sdio_deselect(void)
{
	return sdio_send_command(WII_SDIO_CMD_DESELECT, WII_SDIO_TYPE_AC,
		WII_SDIO_RESPONSE_R1B, 0, 0, 0, false);
}


static status_t
sdio_set_bus_width(uint32 width)
{
	uint32 value = width == 4
		? WII_SDIO_BUSWIDTH_4BIT : WII_SDIO_BUSWIDTH_1BIT;
	status_t status = sdio_send_command(WII_SDIO_CMD_APPCMD, WII_SDIO_TYPE_AC,
		WII_SDIO_RESPONSE_R1, (uint32)sRCA << 16, 0, 0, false);
	if (status != B_OK)
		return status;

	status = sdio_send_command(WII_SDIO_ACMD_SETBUSWIDTH, WII_SDIO_TYPE_AC,
		WII_SDIO_RESPONSE_R1, value, 0, 0, false);
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
	wii_ios_close(sFD);
	sFD = wii_ios_open(WII_SDIO_DEVICE, 1);
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
			0, 0, false) != B_OK)
		return B_ERROR;

	if (sdio_send_command(WII_SDIO_CMD_SENDIFCOND, 0, WII_SDIO_RESPONSE_R6,
			WII_SDIO_IFCOND_ARG, 0, 0, false) != B_OK)
		return B_ERROR;
	if ((sResponse.rsp_fields[0] & 0xff) != WII_SDIO_IFCOND_PATTERN)
		return B_ERROR;

	int32 tries = RETRY_COUNT;
	while (tries-- > 0) {
		if (sdio_send_command(WII_SDIO_CMD_APPCMD, WII_SDIO_TYPE_AC,
				WII_SDIO_RESPONSE_R1, 0, 0, 0, false) != B_OK)
			return B_ERROR;
		if (sdio_send_command(WII_SDIO_ACMD_SENDOPCOND, 0,
				WII_SDIO_RESPONSE_R3, WII_SDIO_OPCOND_ARG, 0, 0, false) != B_OK)
			return B_ERROR;
		if ((sResponse.rsp_fields[0] & WII_SDIO_OCR_BUSY) != 0)
			break;

		snooze(RETRY_DELAY);
	}
	if (tries < 0)
		return B_ERROR;

	// Cards that answer without the capacity bit stay on byte addressing.
	sSDHC = (sResponse.rsp_fields[0] & WII_SDIO_OCR_CCS) != 0;

	if (sdio_send_command(WII_SDIO_CMD_ALL_SENDCID, 0, WII_SDIO_RESPONSE_R2,
			(uint32)sRCA << 16, 0, 0, false) != B_OK)
		return B_ERROR;

	if (sdio_send_command(WII_SDIO_CMD_SENDRCA, 0, WII_SDIO_RESPONSE_R5, 0,
			0, 0, false) != B_OK)
		return B_ERROR;

	sRCA = (uint16)(sResponse.rsp_fields[0] >> 16);
	return B_OK;
}


static void
sdio_uninit(void)
{
	if (sFD >= 0)
		wii_ios_close(sFD);

	sFD = -1;
}


static status_t
sdio_init(void)
{
	sRCA = 0;
	sSDHC = false;

	sFD = wii_ios_open(WII_SDIO_DEVICE, 1);
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
			wii_ios_close(sFD);
			sFD = wii_ios_open(WII_SDIO_DEVICE, 1);
			return B_ERROR;
		}
	} else
		sSDHC = (status & WII_SDIO_STATUS_CARD_SDHC) != 0;

	// CMD9 is only legal in standby, so ask before CMD7 enters transfer state.
	if (sdio_send_command(WII_SDIO_CMD_SENDCSD, WII_SDIO_TYPE_AC,
			WII_SDIO_RESPONSE_R2, (uint32)sRCA << 16, 0, 0, false) != B_OK)
		return B_ERROR;
	memcpy(sCSD, &sResponse, sizeof(sCSD));

	if (sdio_select() != B_OK)
		return B_ERROR;

	if (sdio_send_command(WII_SDIO_CMD_SETBLOCKLEN, WII_SDIO_TYPE_AC,
			WII_SDIO_RESPONSE_R1, kSectorSize, 0, 0, false) != B_OK) {
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


// Moves whole sectors between the card and the DMA buffer; caller holds sLock.
static status_t
sdio_transfer(uint32 sector, uint32 count, bool write)
{
	status_t status = sdio_select();
	if (status != B_OK)
		return status;

	// SDHC cards count the argument in sectors, the others in bytes.
	uint32 address = sSDHC ? sector : sector * kSectorSize;
	status = sdio_send_command(write
			? WII_SDIO_CMD_WRITEMULTIBLOCK : WII_SDIO_CMD_READMULTIBLOCK,
		WII_SDIO_TYPE_AC, WII_SDIO_RESPONSE_R1, address, count, kSectorSize,
		true);

	sdio_deselect();
	return status;
}


//	#pragma mark - device hooks


static status_t
wii_sd_open(const char *name, uint32 flags, void **cookie)
{
	if (sFD < 0)
		return B_NO_INIT;

	*cookie = NULL;
	return B_OK;
}


static status_t
wii_sd_close(void *cookie)
{
	return B_OK;
}


static status_t
wii_sd_free(void *cookie)
{
	return B_OK;
}


static status_t
wii_sd_control(void *cookie, uint32 op, void *buffer, size_t length)
{
	switch (op) {
		case B_GET_GEOMETRY:
		case B_GET_BIOS_GEOMETRY:
		{
			if (buffer == NULL
				|| (length != 0 && length < sizeof(device_geometry)))
				return B_BAD_VALUE;

			// One sector per cylinder keeps the product exactly the capacity.
			device_geometry geometry;
			memset(&geometry, 0, sizeof(geometry));
			geometry.bytes_per_sector = kSectorSize;
			geometry.sectors_per_track = 1;
			geometry.cylinder_count = (uint32)(sCapacity / kSectorSize);
			geometry.head_count = 1;
			geometry.device_type = B_DISK;
			geometry.removable = true;
			geometry.read_only = false;
			geometry.write_once = false;
			geometry.bytes_per_physical_sector = kSectorSize;

			return user_memcpy(buffer, &geometry, sizeof(geometry));
		}

		case B_GET_DEVICE_SIZE:
		{
			if (buffer == NULL)
				return B_BAD_VALUE;

			size_t size = (size_t)sCapacity;
			return user_memcpy(buffer, &size, sizeof(size));
		}

		case B_GET_MEDIA_STATUS:
		{
			if (buffer == NULL)
				return B_BAD_VALUE;

			status_t status = B_OK;
			return user_memcpy(buffer, &status, sizeof(status));
		}

		case B_FLUSH_DRIVE_CACHE:
			return B_OK;
	}

	return B_DEV_INVALID_IOCTL;
}


static status_t
wii_sd_read(void *cookie, off_t pos, void *buffer, size_t *_length)
{
	size_t length = *_length;
	*_length = 0;

	if (sFD < 0)
		return B_NO_INIT;
	if (pos < 0 || buffer == NULL)
		return B_BAD_VALUE;
	if ((uint64)pos >= sCapacity)
		return B_OK;
	if ((uint64)pos + length > sCapacity)
		length = (size_t)(sCapacity - pos);

	MutexLocker locker(sLock);

	uint8 *out = (uint8 *)buffer;
	size_t remaining = length;

	while (remaining > 0) {
		uint32 sector = (uint32)(pos / kSectorSize);
		size_t offset = pos % kSectorSize;
		size_t chunk = kSectorSize - offset;
		uint32 count = 1;

		if (offset == 0 && remaining >= kSectorSize) {
			count = (uint32)(remaining / kSectorSize);
			if (count > MAX_TRANSFER_SECTORS)
				count = MAX_TRANSFER_SECTORS;
			chunk = count * kSectorSize;
		} else if (chunk > remaining)
			chunk = remaining;

		status_t status = sdio_transfer(sector, count, false);
		if (status != B_OK)
			return status;

		status = user_memcpy(out, sDMABuffer + offset, chunk);
		if (status != B_OK)
			return status;

		out += chunk;
		pos += chunk;
		remaining -= chunk;
		*_length += chunk;
	}

	return B_OK;
}


static status_t
wii_sd_write(void *cookie, off_t pos, const void *buffer, size_t *_length)
{
	size_t length = *_length;
	*_length = 0;

	if (sFD < 0)
		return B_NO_INIT;
	if (pos < 0 || buffer == NULL)
		return B_BAD_VALUE;
	if ((uint64)pos >= sCapacity)
		return B_OK;
	if ((uint64)pos + length > sCapacity)
		length = (size_t)(sCapacity - pos);

	MutexLocker locker(sLock);

	const uint8 *in = (const uint8 *)buffer;
	size_t remaining = length;

	while (remaining > 0) {
		uint32 sector = (uint32)(pos / kSectorSize);
		size_t offset = pos % kSectorSize;
		size_t chunk = kSectorSize - offset;
		uint32 count = 1;
		bool partial = true;

		if (offset == 0 && remaining >= kSectorSize) {
			count = (uint32)(remaining / kSectorSize);
			if (count > MAX_TRANSFER_SECTORS)
				count = MAX_TRANSFER_SECTORS;
			chunk = count * kSectorSize;
			partial = false;
		} else if (chunk > remaining)
			chunk = remaining;

		// A partial sector keeps whatever surrounds the bytes we are writing.
		status_t status;
		if (partial) {
			status = sdio_transfer(sector, 1, false);
			if (status != B_OK)
				return status;
		}

		status = user_memcpy(sDMABuffer + offset, in, chunk);
		if (status != B_OK)
			return status;

		status = sdio_transfer(sector, count, true);
		if (status != B_OK)
			return status;

		in += chunk;
		pos += chunk;
		remaining -= chunk;
		*_length += chunk;
	}

	return B_OK;
}


//	#pragma mark - driver hooks


status_t
init_hardware(void)
{
	return B_OK;
}


const char **
publish_devices(void)
{
	static const char *devices[] = {
		DEVICE_NAME,
		NULL
	};

	return devices;
}


device_hooks *
find_device(const char *name)
{
	static device_hooks hooks = {
		&wii_sd_open,
		&wii_sd_close,
		&wii_sd_free,
		&wii_sd_control,
		&wii_sd_read,
		&wii_sd_write,
	};

	if (strcmp(name, DEVICE_NAME) == 0)
		return &hooks;

	return NULL;
}


status_t
init_driver(void)
{
	status_t status = wii_ipc_init();
	if (status != B_OK)
		return status;

	// IOS is handed the physical address of this buffer, so it stays put.
	void *address;
	sDMAArea = create_area("wii_sd dma", &address, B_ANY_KERNEL_ADDRESS,
		DMA_BUFFER_SIZE, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (sDMAArea < 0)
		return sDMAArea;

	physical_entry entry;
	status = get_memory_map(address, DMA_BUFFER_SIZE, &entry, 1);
	if (status == B_OK) {
		sDMABuffer = (uint8 *)address;
		sDMAPhysical = (uint32)entry.address;
		status = sdio_init();
	}

	if (status == B_OK) {
		sCapacity = wii_sdio_csd_capacity(sCSD);
		if (sCapacity < kSectorSize)
			status = B_DEV_NO_MEDIA;
	}

	if (status != B_OK) {
		sdio_uninit();
		delete_area(sDMAArea);
		sDMAArea = -1;
		sDMABuffer = NULL;
		return status;
	}

	dprintf("wii_sd: %s card, %" B_PRIu64 " bytes (%" B_PRIu64 " sectors)\n",
		sSDHC ? "SDHC" : "SD", sCapacity, sCapacity / kSectorSize);

	return B_OK;
}


void
uninit_driver(void)
{
	sdio_uninit();

	if (sDMAArea >= 0)
		delete_area(sDMAArea);

	sDMAArea = -1;
	sDMABuffer = NULL;
}
