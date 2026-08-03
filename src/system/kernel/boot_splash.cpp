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

// A persistent copy of the framebuffer info. sInfo is pointed at this so the
// loading status line can keep drawing AFTER boot_splash_uninit() frees the
// kernel args (during the slow userspace part of boot). The framebuffer memory
// itself persists. sStatusActive gates the updates and is cleared when
// app_server takes over the framebuffer (boot_splash_status_done()).
static struct frame_buffer_boot_info sPersistedInfo;
static bool sStatusActive = false;


// The kernel debug console's 6x12 ASCII bitmap font (src/system/kernel/debug/
// font.cpp). Reused here to stamp a build label onto the boot splash.
struct FramebufferFont {
	int glyphWidth;
	int glyphHeight;
	uint8 data[1548];
};
extern FramebufferFont smallFont;

// The build label drawn centered under the boot-splash icons (pre-alpha).
#ifdef HAIKU_DISTRO_COMPATIBILITY_COMPATIBLE
static const char* const kSplashBuildLabel = "pre-pre-pre-alpha";
#endif


static void
set_white_pixel(uint8* p, int32 depth)
{
	switch (depth) {
		case 8:		p[0] = 63; break;			// kSystemPalette opaque white
		case 15:	*(uint16*)p = 0x7fff; break;
		case 16:	*(uint16*)p = 0xffff; break;
		case 32:	p[0] = p[1] = p[2] = p[3] = 0xff; break;
		case 24:
		default:	p[0] = p[1] = p[2] = 0xff; break;
	}
}


// Draw a text string in white, horizontally centered on centerX, top at topY.
// Every framebuffer write is clipped to the screen so an off-screen label (tiny
// display) can never fault.
static void
draw_splash_label(const char* text, int centerX, int topY, int scale)
{
	if (sInfo == NULL || text == NULL)
		return;

	const int gw = smallFont.glyphWidth;
	const int gh = smallFont.glyphHeight;
	const int32 depth = sInfo->depth;
	const int bpp = (depth + 7) / 8;
	uint8* fb = (uint8*)sInfo->frame_buffer;

	int len = 0;
	while (text[len] != '\0')
		len++;

	int startX = centerX - (len * gw * scale) / 2;

	for (int i = 0; i < len; i++) {
		uint8 c = (uint8)text[i];
		if (c > 127)
			c = '?';
		for (int y = 0; y < gh; y++) {
			uint8 bits = smallFont.data[c * gh + y];
			for (int x = 0; x < gw; x++) {
				if (((bits >> x) & 1) == 0)
					continue;
				for (int sy = 0; sy < scale; sy++) {
					int py = topY + y * scale + sy;
					if (py < 0 || py >= (int)sInfo->height)
						continue;
					for (int sx = 0; sx < scale; sx++) {
						int px = startX + (i * gw + x) * scale + sx;
						if (px < 0 || px >= (int)sInfo->width)
							continue;
						set_white_pixel(fb + (size_t)py * sInfo->bytes_per_row
							+ (size_t)px * bpp, depth);
					}
				}
			}
		}
	}
}


// Short status messages shown under the build label, one per kernel boot stage.
static const char* const kStageMessages[BOOT_SPLASH_STAGE_MAX] = {
	"Loading modules",			// STAGE_1_INIT_MODULES
	"Starting file systems",	// STAGE_2_BOOTSTRAP_FS
	"Detecting devices",		// STAGE_3_INIT_DEVICES
	"Mounting boot disk",		// STAGE_4_MOUNT_BOOT_FS
	"Configuring CPU",			// STAGE_5_INIT_CPU_MODULES
	"Preparing memory",			// STAGE_6_INIT_VM_MODULES
	"Starting up",				// STAGE_7_RUN_BOOT_SCRIPT
};


// Top Y of the one-line status band, just below the build label.
static int
splash_status_top(void)
{
	int iconsWidth, iconsHeight, iconsX, iconsY;
	compute_splash_icons_placement(sInfo->width, sInfo->height,
		iconsWidth, iconsHeight, iconsX, iconsY);
	return iconsY + iconsHeight + 6 + smallFont.glyphHeight * 2 + 18;
}


// Clear a full-width horizontal band to black (erases the previous status text).
static void
clear_splash_band(int topY, int height)
{
	if (sInfo == NULL)
		return;
	const int bpp = (sInfo->depth + 7) / 8;
	for (int y = topY; y < topY + height; y++) {
		if (y < 0 || y >= (int)sInfo->height)
			continue;
		memset((uint8*)sInfo->frame_buffer + (size_t)y * sInfo->bytes_per_row,
			0, (size_t)sInfo->width * bpp);
	}
}


// Replace the on-screen loading status line with a short message. Exported so
// the boot path (kernel and drivers) can report progress. A no-op once
// app_server has taken over the framebuffer (boot_splash_status_done()).
void
boot_splash_set_status(const char* text)
{
	if (!sStatusActive || sInfo == NULL || text == NULL)
		return;
	int top = splash_status_top();
	clear_splash_band(top, smallFont.glyphHeight * 2);
	draw_splash_label(text, sInfo->width / 2, top, 2);
}


// Stop the loading status line (app_server is taking over the framebuffer).
void
boot_splash_status_done(void)
{
	sStatusActive = false;
}


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

	sPersistedInfo = *sInfo;
	sInfo = &sPersistedInfo;
	sStatusActive = true;


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

#ifdef HAIKU_DISTRO_COMPATIBILITY_COMPATIBLE
	// Stamp the build label centered just below the icon row.
	int iconsWidth, iconsHeight, iconsX, iconsY;
	compute_splash_icons_placement(sInfo->width, sInfo->height,
		iconsWidth, iconsHeight, iconsX, iconsY);
	draw_splash_label(kSplashBuildLabel, sInfo->width / 2,
		iconsY + iconsHeight + 6, 2);
#endif
}


void
boot_splash_uninit(void)
{
	// Keep sInfo (it points at the persistent copy) so boot_splash_set_status()
	// can keep updating the loading line during userspace boot. The icon image
	// is part of the freed kernel args, so drop it.
	sUncompressedIcons = NULL;
}


void
boot_splash_set_stage(int stage)
{
	TRACE("boot_splash_set_stage: stage=%d\n", stage);

	if (sInfo == NULL || stage < 0 || stage >= BOOT_SPLASH_STAGE_MAX)
		return;

	// Colour in this stage's icons. sUncompressedIcons is NULL when the platform
	// loader supplied no icon image (e.g. the ppc/OpenFirmware loader) - the
	// status text below still works without it.
	if (sUncompressedIcons != NULL) {
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

	// Update the loading status line under the build label.
	boot_splash_set_status(kStageMessages[stage]);
}
