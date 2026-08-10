/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Antigravity
 */

#include <boot/vfs.h>
#include <boot/platform.h>
#include <boot/partitions.h>
#include <string.h>
#include <stdio.h>

#include <fat.h>
#include <gccore.h>

// A virtual block device that exposes the haiku.img file 
// residing on the physical Wii SD card's FAT32 partition.

class WiiSDBootDevice : public Node {
public:
	WiiSDBootDevice();
	virtual ~WiiSDBootDevice();

	virtual status_t InitCheck();
	virtual ssize_t ReadAt(void *cookie, off_t pos, void *buffer, size_t bufferSize);
	virtual ssize_t WriteAt(void *cookie, off_t pos, const void *buffer, size_t bufferSize);
	virtual off_t Size() const;

private:
	FILE* fImageFile;
	off_t fSize;
};


WiiSDBootDevice::WiiSDBootDevice()
	:
	fImageFile(NULL),
	fSize(0)
{
	fImageFile = fopen("sd:/haiku/haiku.img", "rb");
	if (fImageFile != NULL) {
		fseek(fImageFile, 0, SEEK_END);
		fSize = ftell(fImageFile);
		fseek(fImageFile, 0, SEEK_SET);
	}
}


WiiSDBootDevice::~WiiSDBootDevice()
{
	if (fImageFile != NULL)
		fclose(fImageFile);
}


status_t
WiiSDBootDevice::InitCheck()
{
	return fImageFile != NULL ? B_OK : B_ENTRY_NOT_FOUND;
}


ssize_t
WiiSDBootDevice::ReadAt(void *cookie, off_t pos, void *buffer, size_t bufferSize)
{
	if (fImageFile == NULL)
		return B_NO_INIT;

	fseek(fImageFile, pos, SEEK_SET);
	size_t read = fread(buffer, 1, bufferSize, fImageFile);
	return read;
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
		printf("platform_add_boot_device: sd:/haiku/haiku.img not found!\n");
		delete device;
		return B_ENTRY_NOT_FOUND;
	}

	devicesList->Add(device);
	return B_OK;
}

status_t
platform_add_block_devices(struct stage2_args *args, NodeList *devicesList)
{
	// Adding the FAT32 virtual image is handled above
	return B_OK;
}
