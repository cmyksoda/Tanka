/*
 * Copyright 2008-2010, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Artur Wyszynski <harakash@gmail.com>
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <KernelExport.h>

#define __BOOTSPLASH_KERNEL__
#include <boot/images.h>
#include <boot/platform/generic/video_blitter.h>
#include <boot/platform/generic/video_splash.h>

#include <boot_item.h>
#include <debug.h>
#include <frame_buffer_console.h>

#include <boot_splash.h>


//#define TRACE_BOOT_SPLASH 1
#ifdef TRACE_BOOT_SPLASH
#	define TRACE(x...) dprintf(x);
#else
#	define TRACE(x...) ;
#endif


static struct frame_buffer_boot_info *sInfo;
static uint8 *sUncompressedIcons;


//	#pragma mark - exported functions


void
boot_splash_init(uint8 *bootSplash, uint8 *bootSplashLogo)
{
	TRACE("boot_splash_init: enter\n");

	if (debug_screen_output_enabled())
		return;

	sInfo = (frame_buffer_boot_info *)get_boot_item(FRAME_BUFFER_BOOT_INFO,
		NULL);

	sUncompressedIcons = bootSplash;

	if (sInfo == NULL)
		return;


	// Clear the frame buffer (removing leftover boot-loader text), draw the
	// Haiku logo, and draw the initial grayed-out icons. As boot proceeds,
	// boot_splash_set_stage() colours the icons in stage by stage. The boot
	// loader normally does the clear/logo/gray-icons in video_display_splash(),
	// but platforms whose loader cannot write the frame buffer (ppc/OpenFirmware)
	// leave it to the kernel here.
	memset((void*)sInfo->frame_buffer, 0,
		(size_t)sInfo->bytes_per_row * sInfo->height);

	int width, height, x, y;

	if (bootSplashLogo != NULL) {
		compute_splash_logo_placement(sInfo->width, sInfo->height,
			width, height, x, y);

		BlitParameters params;
		params.from = bootSplashLogo;
		params.fromWidth = kSplashLogoWidth;
		params.fromLeft = 0;
		params.fromTop = 0;
		params.fromRight = width;
		params.fromBottom = height;
		params.to = (uint8*)sInfo->frame_buffer;
		params.toBytesPerRow = sInfo->bytes_per_row;
		params.toLeft = x;
		params.toTop = y;

		blit(params, sInfo->depth);
	}

	// initial grayed-out icons: the lower half of the icons image
	if (sUncompressedIcons != NULL) {
		const uint16 iconsHalfHeight = kSplashIconsHeight / 2;
		const int bytesPerPixel = sInfo->depth == 8 ? 1 : 3;

		compute_splash_icons_placement(sInfo->width, sInfo->height,
			width, height, x, y);

		BlitParameters params;
		params.from = sUncompressedIcons
			+ (size_t)kSplashIconsWidth * iconsHalfHeight * bytesPerPixel;
		params.fromWidth = kSplashIconsWidth;
		params.fromLeft = 0;
		params.fromTop = 0;
		params.fromRight = width;
		params.fromBottom = height;
		params.to = (uint8*)sInfo->frame_buffer;
		params.toBytesPerRow = sInfo->bytes_per_row;
		params.toLeft = x;
		params.toTop = y;

		blit(params, sInfo->depth);
	}
}


void
boot_splash_uninit(void)
{
	sInfo = NULL;
}


void
boot_splash_set_stage(int stage)
{
	TRACE("boot_splash_set_stage: stage=%d\n", stage);

	// sUncompressedIcons is NULL when the platform loader supplied no boot
	// splash image (e.g. the ppc/OpenFirmware loader) - blitting from it would
	// dereference NULL. Guard against it as well as a missing frame buffer.
	if (sInfo == NULL || sUncompressedIcons == NULL || stage < 0
			|| stage >= BOOT_SPLASH_STAGE_MAX) {
		return;
	}

	int width, height, x, y;
	compute_splash_icons_placement(sInfo->width, sInfo->height,
		width, height, x, y);

	int stageLeftEdge = width * stage / BOOT_SPLASH_STAGE_MAX;
	int stageRightEdge = width * (stage + 1) / BOOT_SPLASH_STAGE_MAX;

	BlitParameters params;
	params.from = sUncompressedIcons;
	params.fromWidth = kSplashIconsWidth;
	params.fromLeft = stageLeftEdge;
	params.fromTop = 0;
	params.fromRight = stageRightEdge;
	params.fromBottom = height;
	params.to = (uint8*)sInfo->frame_buffer;
	params.toBytesPerRow = sInfo->bytes_per_row;
	params.toLeft = stageLeftEdge + x;
	params.toTop = y;

	blit(params, sInfo->depth);
}
