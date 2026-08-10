/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include <KernelExport.h>
#include <boot/platform.h>
#include <boot/heap.h>
#include <boot/stage2.h>
#include <arch/cpu.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// libogc includes
#include <gccore.h>
#include <fat.h>
#include <wiiuse/wpad.h>
#include <wiikeyboard/keyboard.h>

extern "C" void _start(void);
extern "C" int main(stage2_args *args);

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

extern "C" void
platform_start_kernel(void)
{
	panic("platform_start_kernel not implemented on Wii yet!");
}

extern "C" void
platform_exit(void)
{
	// On Wii, maybe reboot or return to homebrew channel
	exit(0);
}

extern "C" uint32
platform_boot_options(void)
{
	return 0;
}

extern "C" void
_start(void)
{
	// Initialize Wii Video
	VIDEO_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(xfb,20,20,rmode->fbWidth,rmode->xfbHeight,rmode->fbWidth*VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if(rmode->viTVMode&VI_NON_INTERLACE) VIDEO_WaitVSync();

	printf("\nHaiku Bootloader for Nintendo Wii\n");

	// Initialize Inputs
	WPAD_Init();
	if (KEYBOARD_Init(NULL) == 0) {
		printf("USB Keyboard initialized.\n");
	} else {
		printf("USB Keyboard failed to initialize.\n");
	}

	// Initialize FAT
	if (!fatInitDefault()) {
		printf("fatInitDefault failed! SD card not found.\n");
	} else {
		printf("SD card FAT initialized successfully.\n");
	}

	stage2_args args;
	memset(&args, 0, sizeof(stage2_args));

	// TODO: init heap, Haiku console, mmu, etc.

	main(&args);
}

// Stubs for required generic boot platform functions
extern "C" void console_clear_screen(void) {}
extern "C" int32 console_width(void) { return 80; }
extern "C" int32 console_height(void) { return 25; }
extern "C" void console_set_cursor(int32 x, int32 y) {}
extern "C" void console_show_cursor(void) {}
extern "C" void console_hide_cursor(void) {}
extern "C" void console_set_color(int32 foreground, int32 background) {}
extern "C" int console_wait_for_key(void) {
	while (true) {
		// Poll Wii Remote
		WPAD_ScanPads();
		u32 pressed = WPAD_ButtonsDown(0);
		if (pressed & WPAD_BUTTON_UP) return 38; // Up arrow
		if (pressed & WPAD_BUTTON_DOWN) return 40; // Down arrow
		if (pressed & WPAD_BUTTON_A) return '\n'; // Enter
		if (pressed & WPAD_BUTTON_B) return 27; // Esc

		// Poll USB Keyboard
		keyboard_event ke;
		if (KEYBOARD_GetEvent(&ke)) {
			if (ke.type == KEYBOARD_PRESSED) {
				// We map basic ones or just return the character.
				// Since Haiku's boot menu mostly uses Up/Down/Enter/Esc:
				if (ke.keycode == 104) return 38; // Up
				if (ke.keycode == 105) return 40; // Down
				if (ke.keycode == 28) return '\n'; // Enter
				if (ke.keycode == 1) return 27; // Esc
				
				if (ke.symbol > 0 && ke.symbol < 128) {
					return ke.symbol;
				}
			}
		}
		
		VIDEO_WaitVSync();
	}
	return 0;
}
extern "C" void console_put_char(char c) {}

extern "C" void panic(const char* format, ...) {
	while (true) {}
}

extern "C" void dprintf(const char* format, ...) {
}

// MMU functions moved to mmu.cpp

extern "C" status_t boot_arch_cpu_init(void) {
	return B_OK;
}
