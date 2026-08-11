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

#include <boot_item.h>
#include <frame_buffer_console.h>
#include <vm/vm.h>
#include <graphic_driver.h>

#include <vesa_info.h>

#define WII_VIDEO_DEVICE_NAME "graphics/wii_video"
#define WII_ACCELERANT_NAME "framebuffer.accelerant"

int32 api_version = B_OS_API_VERSION;

struct wii_video_info {
	vesa_shared_info*	shared_info;
	area_id				shared_area;
	area_id				frame_buffer_area;
	addr_t				frame_buffer;
	int32				id;
	int32				open_count;
};

static wii_video_info sInfo;

static uint32
get_color_space_for_depth(uint32 depth)
{
	switch (depth) {
		case 8: return B_CMAP8;
		case 15: return B_RGB15;
		case 16: return B_RGB16;
		case 24: return B_RGB24;
		case 32: return B_RGB32;
	}
	return 0;
}


static status_t
wii_video_init_info()
{
	frame_buffer_boot_info* bufferInfo
		= (frame_buffer_boot_info*)get_boot_item(FRAME_BUFFER_BOOT_INFO, NULL);
	if (bufferInfo == NULL)
		return B_ERROR;

	size_t sharedSize = (sizeof(vesa_shared_info) + 7) & ~7;

	sInfo.shared_area = create_area("wii video shared info",
		(void**)&sInfo.shared_info, B_ANY_KERNEL_ADDRESS,
		ROUND_TO_PAGE_SIZE(sharedSize), B_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_CLONEABLE_AREA);
	if (sInfo.shared_area < 0)
		return sInfo.shared_area;

	memset(sInfo.shared_info, 0, sizeof(vesa_shared_info));

	// Map the framebuffer with write-combining for better performance
	// on the Broadway CPU (gathered stores vs. strictly ordered bus writes)
	addr_t frameBuffer;
	size_t fbSize = bufferInfo->bytes_per_row * bufferInfo->height;

	sInfo.frame_buffer_area = map_physical_memory("wii video framebuffer",
		bufferInfo->physical_frame_buffer, fbSize,
		B_ANY_KERNEL_ADDRESS | B_WRITE_COMBINING_MEMORY,
		B_READ_AREA | B_WRITE_AREA, (void**)&frameBuffer);
	if (sInfo.frame_buffer_area < 0) {
		delete_area(sInfo.shared_area);
		return sInfo.frame_buffer_area;
	}

	sInfo.frame_buffer = frameBuffer;
	vm_set_area_memory_type(sInfo.frame_buffer_area,
		bufferInfo->physical_frame_buffer, B_WRITE_COMBINING_MEMORY);

	vesa_shared_info& sharedInfo = *sInfo.shared_info;
	sharedInfo.current_mode.virtual_width = bufferInfo->width;
	sharedInfo.current_mode.virtual_height = bufferInfo->height;
	sharedInfo.current_mode.space = get_color_space_for_depth(bufferInfo->depth);
	sharedInfo.bytes_per_row = bufferInfo->bytes_per_row;
	strlcpy(sharedInfo.name, "Nintendo Wii", sizeof(sharedInfo.name));

	// EDID is not available on Wii
	sharedInfo.has_edid = false;

	frame_buffer_update(frameBuffer, bufferInfo->width, bufferInfo->height,
		bufferInfo->depth, bufferInfo->bytes_per_row);

	dprintf("wii_video: initialized %ux%ux%u framebuffer\n",
		bufferInfo->width, bufferInfo->height, bufferInfo->depth);

	return B_OK;
}


static void
wii_video_uninit_info()
{
	if (sInfo.frame_buffer_area >= 0) {
		vm_change_clones_to_null_areas(sInfo.frame_buffer_area);
		delete_area(sInfo.frame_buffer_area);
	}
	if (sInfo.shared_area >= 0)
		delete_area(sInfo.shared_area);
}


static status_t
wii_video_open(const char* name, uint32 flags, void** cookie)
{
	status_t status = B_OK;

	if (sInfo.open_count == 0) {
		status = wii_video_init_info();
		if (status != B_OK)
			return status;
	}

	sInfo.open_count++;
	*cookie = &sInfo;
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
	if (--sInfo.open_count == 0)
		wii_video_uninit_info();
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
wii_video_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	wii_video_info* info = (wii_video_info*)cookie;

	switch (op) {
		case B_GET_ACCELERANT_SIGNATURE:
			dprintf("wii_video: accelerant signature: %s\n", WII_ACCELERANT_NAME);
			if (user_strlcpy((char*)buffer, WII_ACCELERANT_NAME,
					B_FILE_NAME_LENGTH) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;

		case VESA_GET_PRIVATE_DATA:
			return user_memcpy(buffer, &info->shared_area, sizeof(area_id));

		case VESA_CLONE_FRAME_BUFFER:
		{
			void* dummy;
			area_id area = vm_clone_area(B_CURRENT_TEAM, "wii cloned framebuffer",
				&dummy, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, 0,
				info->frame_buffer_area, true);
			if (area < 0)
				return area;
			return _user_get_area_info(area, (area_info*)buffer);
		}

		case VESA_GET_DEVICE_NAME:
			if (user_strlcpy((char*)buffer, WII_VIDEO_DEVICE_NAME,
					B_PATH_NAME_LENGTH) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;

		default:
			break;
	}

	return B_DEV_INVALID_IOCTL;
}


static const char* sDeviceNames[] = {
	WII_VIDEO_DEVICE_NAME,
	NULL
};


extern "C" status_t
init_hardware(void)
{
	frame_buffer_boot_info* bufferInfo
		= (frame_buffer_boot_info*)get_boot_item(FRAME_BUFFER_BOOT_INFO, NULL);
	if (bufferInfo == NULL)
		return B_ERROR;
	return B_OK;
}


extern "C" status_t
init_driver(void)
{
	memset(&sInfo, 0, sizeof(sInfo));
	sInfo.shared_area = -1;
	sInfo.frame_buffer_area = -1;
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
