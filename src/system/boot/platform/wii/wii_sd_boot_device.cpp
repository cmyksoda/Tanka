/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include <boot/vfs.h>
#include <boot/platform.h>
#include <boot/partitions.h>
#include <string.h>
#include <stdio.h>

#include <gccore.h>
#include <sdcard/wiisd_io.h>

// A virtual block device that exposes the haiku.img file 
// residing on the physical Wii SD card.
// For now, we just expose the RAW SD card! The user will have to 
// write the anyboot image directly to the SD card, bypassing FAT32!
// Later, we can add a FAT32 parser to find haiku.img.

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


WiiSDBootDevice::WiiSDBootDevice()
	:
	fInitialized(false),
	fSize(0)
{
	if (__io_wiisd.startup() && __io_wiisd.isInserted()) {
		fInitialized = true;
		// Arbitrary large size for raw SD card (e.g. 32GB)
		fSize = 32ULL * 1024 * 1024 * 1024;
	}
}


WiiSDBootDevice::~WiiSDBootDevice()
{
	if (fInitialized)
		__io_wiisd.shutdown();
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

	// pos and bufferSize must be sector aligned (512 bytes)
	sec_t startSector = pos / 512;
	sec_t numSectors = bufferSize / 512;

	if (__io_wiisd.readSectors(startSector, numSectors, buffer))
		return bufferSize;

	return B_IO_ERROR;
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
