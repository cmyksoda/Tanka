#include <boot/platform.h>
#include <boot/stdio.h>
#include <boot/stage2.h>
#include <boot/menu.h>
#include <string.h>

FILE *stdin, *stdout, *stderr;

class Console : public ConsoleNode {
	public:
		Console() : ConsoleNode() {}

		virtual ssize_t ReadAt(void *cookie, off_t pos, void *buffer,
			size_t bufferSize) { return B_OK; }
		virtual ssize_t WriteAt(void *cookie, off_t pos, const void *buffer,
			size_t bufferSize) { return bufferSize; }
		virtual void ClearScreen() {}
		virtual int32 Width() { return 80; }
		virtual int32 Height() { return 25; }
		virtual void SetCursor(int32 x, int32 y) {}
		virtual void SetCursorVisible(bool visible) {}
		virtual void SetColors(int32 foreground, int32 background) {}
};

static Console sConsole;

status_t
platform_init_video(void)
{
	stdout = stderr = (FILE *)&sConsole;
	return B_OK;
}


void
platform_switch_to_logo(void)
{
}


void
platform_switch_to_text_mode(void)
{
}


void
platform_cleanup_devices()
{
}

void
platform_run_menu(Menu *menu)
{
}

void
platform_update_menu_item(Menu *menu, MenuItem *item)
{
}

size_t
platform_get_user_input_text(Menu *menu, MenuItem *item, char *buffer,
	size_t bufferSize)
{
	return 0;
}

void
platform_add_menus(Menu *menu)
{
}

char *
platform_debug_get_log_buffer(size_t *_size)
{
	return NULL;
}

void
platform_debug_syslog_early(const char *buffer, size_t length)
{
}

void
platform_load_ucode(BootVolume &volume)
{
}

extern "C" {
	float sqrtf(float x) { return 0.0f; }
	float atan2f(float y, float x) { return 0.0f; }
	void sincosf(float x, float *s, float *c) { *s = 0.0f; *c = 1.0f; }
	float atanf(float x) { return 0.0f; }

	void* __sf[3];
	void* devoptab_list[8];

	int setvbuf(FILE *file, char *buff, int mode, size_t size) {
		return 0;
	}

	int __errno;
	void* FindDevice(const char*) { return NULL; }
	int AddDevice(void*) { return -1; }
	int RemoveDevice(const char*) { return -1; }
	const unsigned char _ctype_[257] = {0};
	void* __Arena1Lo;
	void build_argv() {}
}
