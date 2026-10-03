/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_PLATFORM_WII_SD_IMAGE_H
#define _KERNEL_PLATFORM_WII_SD_IMAGE_H

#include <SupportDefs.h>


// The system volume ships as tanka/tanka.img on the card's FAT32 file system.
// The loader and the wii_sd driver both find it by walking the FAT, and then
// read it straight off the card through the cluster runs it occupies.

#define WII_SD_IMAGE_PATH		"tanka/tanka.img"
#define WII_SD_IMAGE_MAX_RUNS	64


typedef struct {
	uint32	offset;		// first image sector of the run
	uint32	sector;		// card sector it starts at
	uint32	count;
} wii_sd_image_run;

typedef struct {
	uint32				sectors;
	uint32				run_count;
	wii_sd_image_run	runs[WII_SD_IMAGE_MAX_RUNS];
} wii_sd_image;

typedef status_t (*wii_sd_read_sectors)(void *cookie, uint32 sector,
	uint32 count, void *buffer);


#ifdef __cplusplus
extern "C" {
#endif

status_t wii_sd_image_find(wii_sd_read_sectors read, void *cookie,
	wii_sd_image *image);
uint32 wii_sd_image_map(const wii_sd_image *image, uint32 sector,
	uint32 *_count);

#ifdef __cplusplus
}
#endif

#endif	// _KERNEL_PLATFORM_WII_SD_IMAGE_H
