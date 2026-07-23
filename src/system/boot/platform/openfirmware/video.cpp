/*
 * Copyright 2004, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2010 Andreas Färber <andreas.faerber@web.de>
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#include <boot/platform.h>
#include <boot/stage2.h>
#include <boot/platform/generic/text_console.h>
#include <boot/platform/generic/video.h>
#include <edid.h>
#include <platform/openfirmware/openfirmware.h>

#include <GraphicsDefs.h>
#include <Palette.h>
#include <boot/images.h>
	// kSystemPalette - the fixed 8-bit system color map


//#define TRACE_VIDEO


static intptr_t sScreen;


void
platform_blit4(addr_t frameBuffer, const uint8 *data,
	uint16 width, uint16 height, uint16 imageWidth, uint16 left, uint16 top)
{
	panic("platform_blit4(): not implemented\n");
}


extern "C" void
platform_set_palette(const uint8 *palette)
{
	switch (gKernelArgs.frame_buffer.depth) {
		case 8:
			if (of_call_method(sScreen, "set-colors", 3, 0,
					256, 0, palette) == OF_FAILED) {
				for (int index = 0; index < 256; index++) {
					of_call_method(sScreen, "color!", 4, 0, index,
						palette[index * 3 + 2],
						palette[index * 3 + 1],
						palette[index * 3 + 0]);
				}
			}
			break;
		default:
			break;
	}
}


extern "C" void* arch_mmu_map_device(void* physicalAddress, size_t size);


static inline uint32
swap32(uint32 value)
{
	return (value >> 24) | ((value >> 8) & 0x0000ff00)
		| ((value << 8) & 0x00ff0000) | (value << 24);
}


/*!	The ATI FCode driver only does 8-bit modes, and past OpenFirmware nothing
	can program this display's palette. But the Rage scanout itself is happy
	to do direct color: flip CRTC_GEN_CNTL's pixel-width field to ARGB8888 so
	the whole boot runs palette-free 32-bit. Gated to the exact device this
	was verified on (dingusppc's emulated Rage Pro).
*/
static bool
try_ati_true_color(intptr_t package, uint32 width, uint32 height,
	uint32 lineBytes)
{
	uint32 vendorID = 0, deviceID = 0;
	if (of_getprop(package, "vendor-id", &vendorID, 4) == OF_FAILED
		|| vendorID != 0x1002)
		return false;
	if (of_getprop(package, "device-id", &deviceID, 4) == OF_FAILED
		|| deviceID != 0x4750)
		return false;

	// the 32-bit frame must still fit the aperture reported for 8-bit
	// (the VRAM aperture BAR is 16 MB; 640*480*4 easily fits, but check)
	if (width * 4 * height > 8 * 1024 * 1024)
		return false;

	// find the 4 KB auxiliary register BAR (config register 0x18)
	uint32 assigned[40];
	int length = of_getprop(package, "assigned-addresses", assigned,
		sizeof(assigned));
	if (length <= 0)
		return false;
	uint32 regsPhysical = 0;
	uint32 regsSize = 0;
	for (int i = 0; i + 5 <= (int)(length / 4); i += 5) {
		if ((assigned[i] & 0xff) == 0x18) {
			regsPhysical = assigned[i + 2];
			regsSize = assigned[i + 4];
			break;
		}
	}
	if (regsPhysical == 0 || regsSize < 0x800)
		return false;

	volatile uint8* regs
		= (volatile uint8*)arch_mmu_map_device((void*)regsPhysical, regsSize);
	if (regs == NULL)
		return false;

	// CRTC_GEN_CNTL is byte 0x1c of register block 0, which lives at +0x400
	// in the (little-endian) auxiliary aperture; pixel width is bits 10-8.
	volatile uint32* crtcGenCntl = (volatile uint32*)(regs + 0x400 + 0x1c);
	uint32 value = swap32(*crtcGenCntl);
	if (((value >> 8) & 7) != 2) {
		// not the expected 8-bit CLUT mode - leave the hardware alone
		return false;
	}
	value = (value & ~(7UL << 8)) | (3UL << 8);
		// RGB555: direct color whose big-endian scanout matches our
		// big-endian 16-bit pixel stores

	// Program the depth with the classic disable -> program -> enable
	// sequence. That is the proper mach64 modeset order on real hardware,
	// and it also sidesteps a dingusppc quirk: its CRTC_GEN_CNTL write
	// handler recalculates the scanout *before* committing the written
	// value, so a single write is evaluated against the stale pixel width
	// and ignored. With two writes, the second one (re-enabling the CRTC)
	// recalculates against the already-committed new depth.
	*crtcGenCntl = swap32(value & ~(1UL << 25));
		// new depth, CRTC disabled
	asm volatile("eieio" ::: "memory");
	*crtcGenCntl = swap32(value);
		// new depth, CRTC re-enabled -> scanout recalculated
	asm volatile("eieio; sync" ::: "memory");

	dprintf("ATI Rage: switched CRTC to 15-bit direct color\n");
	return true;
}


extern "C" void
platform_load_system_palette(void)
{
	// In 8-bit mode the desktop's rendering assumes Haiku's fixed system
	// color map, but once OpenFirmware is gone nothing can program this
	// display's DAC (Haiku has no native driver for it), so whatever
	// palette the loader leaves behind is what every color is looked up
	// through forever - with the splash palette loaded, the whole desktop
	// renders in scrambled colors. Load the system color map through the
	// proven OF path as the very last loader act instead. The splash still
	// on screen shifts colors for the rest of the boot; the desktop is
	// then correct.
	if (gKernelArgs.frame_buffer.depth != 8)
		return;
	if (sScreen == 0 || sScreen == OF_FAILED) {
		sScreen = of_open("screen");
		if (sScreen == 0 || sScreen == OF_FAILED)
			return;
	}

	static uint8 palette[256 * 3];
	for (int index = 0; index < 256; index++) {
		palette[index * 3 + 0] = kSystemPalette[index].red;
		palette[index * 3 + 1] = kSystemPalette[index].green;
		palette[index * 3 + 2] = kSystemPalette[index].blue;
	}

	// The kernel draws the boot logo and progress icons from the buffers the
	// loader decompressed into kernel_args - their pixels are indices into
	// the *splash* palette. Since the DAC will hold the *system* palette from
	// here on, remap every pixel to its nearest system-palette equivalent so
	// the splash still looks right for the rest of the boot.
	static uint8 remap[256];
	for (int i = 0; i < 256; i++) {
		int r = k8BitPalette[i * 3 + 0];
		int g = k8BitPalette[i * 3 + 1];
		int b = k8BitPalette[i * 3 + 2];
		uint32 best = 0xffffffff;
		uint8 bestIndex = 0;
		for (int j = 0; j < 256; j++) {
			int dr = r - kSystemPalette[j].red;
			int dg = g - kSystemPalette[j].green;
			int db = b - kSystemPalette[j].blue;
			uint32 dist = uint32(dr * dr + dg * dg + db * db);
			if (dist < best) {
				best = dist;
				bestIndex = (uint8)j;
			}
		}
		remap[i] = bestIndex;
	}
	if (gKernelArgs.boot_splash_logo != NULL) {
		uint8* pixels = gKernelArgs.boot_splash_logo;
		for (uint32 i = 0; i < (uint32)kSplashLogoWidth * kSplashLogoHeight; i++)
			pixels[i] = remap[pixels[i]];
	}
	if (gKernelArgs.boot_splash != NULL) {
		uint8* pixels = gKernelArgs.boot_splash;
		for (uint32 i = 0; i < (uint32)kSplashIconsWidth * kSplashIconsHeight; i++)
			pixels[i] = remap[pixels[i]];
	}

	platform_set_palette(palette);
}


extern "C" void
platform_switch_to_logo(void)
{
	// in debug mode, we'll never show the logo
	if ((platform_boot_options() & BOOT_OPTION_DEBUG_OUTPUT) != 0)
		return;

	sScreen = of_open("screen");
	if (sScreen == OF_FAILED)
		return;

	intptr_t package = of_instance_to_package(sScreen);
	if (package == OF_FAILED)
		return;
	uintptr_t width, height;
	if (of_call_method(sScreen, "dimensions", 0, 2, &height, &width)
			== OF_FAILED) {
		if (of_getprop(package, "width", &width, sizeof(int32)) == OF_FAILED)
			return;
		if (of_getprop(package, "height", &height, sizeof(int32)) == OF_FAILED)
			return;
	}
	uint32 depth;
	if (of_getprop(package, "depth", &depth, sizeof(uint32)) == OF_FAILED)
		return;
	uint32 lineBytes;
	if (of_getprop(package, "linebytes", &lineBytes, sizeof(uint32))
			== OF_FAILED)
		return;
	uint32 address;
		// address is always 32 bit
	if (of_getprop(package, "address", &address, sizeof(uint32)) == OF_FAILED)
		return;
	// If this is the (emulator) ATI Rage stuck in 8-bit, upgrade the CRTC to
	// RGB555 direct color: no palette, correct colors everywhere. (15-bit is
	// the depth whose big-endian scanout matches our big-endian 16-bit pixel
	// stores; see try_ati_true_color().)
	if (depth == 8 && try_ati_true_color(package, width, height, lineBytes)) {
		depth = 15;
		lineBytes = width * 2;
	}

	gKernelArgs.frame_buffer.physical_buffer.start = address;
	gKernelArgs.frame_buffer.physical_buffer.size = lineBytes * height;
	gKernelArgs.frame_buffer.width = width;
	gKernelArgs.frame_buffer.height = height;
	gKernelArgs.frame_buffer.depth = depth;
	gKernelArgs.frame_buffer.bytes_per_row = lineBytes;

	// Move text to top of display so we don't scroll the boot logo out as soon
	// as we display some text
	console_set_cursor(0, 0);

	dprintf("video mode: %ux%ux%u\n", gKernelArgs.frame_buffer.width,
		gKernelArgs.frame_buffer.height, gKernelArgs.frame_buffer.depth);

	gKernelArgs.frame_buffer.enabled = true;

	// Don't call video_display_splash(): it clears and blits the raw frame
	// buffer, which is not mapped into the loader's address space here (writing
	// it faults under OpenFirmware/QEMU). Instead just decompress the boot
	// icons into gKernelArgs.boot_splash and set the palette; the kernel's
	// boot_splash_set_stage() draws the boot-progress icons itself, using its
	// own frame-buffer mapping.
	video_prepare_boot_splash();
}


extern "C" void
platform_switch_to_text_mode(void)
{
	// nothing to do if we're in text mode
	if (!gKernelArgs.frame_buffer.enabled)
		return;

	// ToDo: implement me

	gKernelArgs.frame_buffer.enabled = false;
}


extern "C" status_t
platform_init_video(void)
{
	gKernelArgs.frame_buffer.enabled = false;

	intptr_t screen = of_finddevice("screen");
	if (screen == OF_FAILED)
		return B_NO_INIT;
	edid1_raw edidRaw;
	if (of_getprop(screen, "EDID", &edidRaw, sizeof(edidRaw)) != OF_FAILED) {
		edid1_info info;
		edid_decode(&info, &edidRaw);
#ifdef TRACE_VIDEO
		edid_dump(&info);
#endif
		gKernelArgs.edid_info = kernel_args_malloc(sizeof(edid1_info));
		if (gKernelArgs.edid_info != NULL)
			memcpy(gKernelArgs.edid_info, &info, sizeof(edid1_info));
	}

	return B_OK;
}

