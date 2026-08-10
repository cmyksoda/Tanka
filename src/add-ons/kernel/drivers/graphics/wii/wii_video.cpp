/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Antigravity
 */

#include <KernelExport.h>
#include <Drivers.h>
#include <Errors.h>
#include <string.h>

#define WII_VIDEO_DEVICE_NAME "graphics/wii_video"

int32 api_version = B_OS_API_VERSION;


static status_t
wii_video_open(const char* name, uint32 flags, void** cookie)
{
	*cookie = NULL;
	return B_OK;
}


static status_t
wii_video_close(void* cookie)
{
	return B_OK;
}


static status_t
wii_video_free(void* cookie)
{
	return B_OK;
}


static status_t
wii_video_read(void* cookie, off_t position, void* buf, size_t* num_bytes)
{
	*num_bytes = 0;
	return B_NOT_ALLOWED;
}


static status_t
wii_video_write(void* cookie, off_t position, const void* buf, size_t* num_bytes)
{
	*num_bytes = 0;
	return B_NOT_ALLOWED;
}


static status_t
wii_video_control(void* cookie, uint32 op, void* arg, size_t length)
{
	// TODO: Handle accelerant ioctls (B_GET_ACCELERANT_SIGNATURE, etc.)
	// TODO: Handle custom ioctl for GX hardware conversion of RGB to YCbCr
	return B_BAD_VALUE;
}


static const char* sDeviceNames[] = {
	WII_VIDEO_DEVICE_NAME,
	NULL
};


extern "C" status_t
init_hardware(void)
{
	// TODO: Check if we are running on a Wii
	return B_OK;
}


extern "C" status_t
init_driver(void)
{
	return B_OK;
}


extern "C" void
uninit_driver(void)
{
}


extern "C" const char**
publish_devices(void)
{
	return sDeviceNames;
}


extern "C" device_hooks*
find_device(const char* name)
{
	static device_hooks hooks = {
		wii_video_open,
		wii_video_close,
		wii_video_free,
		wii_video_control,
		wii_video_read,
		wii_video_write,
		NULL, // select
		NULL  // deselect
	};

	if (!strcmp(name, WII_VIDEO_DEVICE_NAME))
		return &hooks;

	return NULL;
}
