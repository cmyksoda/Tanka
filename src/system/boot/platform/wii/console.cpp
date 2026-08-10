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
};

static Console sConsole;

extern "C" status_t
platform_init_video(void)
{
	stdout = stderr = (FILE *)&sConsole;
	return B_OK;
}


extern "C" void
platform_switch_to_logo(void)
{
}


extern "C" void
platform_switch_to_text_mode(void)
{
}


extern "C" void
platform_cleanup_devices()
{
}

extern "C" void
platform_run_menu(Menu *menu)
{
}

extern "C" void
platform_update_menu_item(Menu *menu, MenuItem *item)
{
}

extern "C" size_t
platform_get_user_input_text(Menu *menu, MenuItem *item, char *buffer,
	size_t bufferSize)
{
	return 0;
}

extern "C" void
platform_add_menus(Menu *menu)
{
}

extern "C" char *
platform_debug_get_log_buffer(size_t *_size)
{
	return NULL;
}

extern "C" void
platform_debug_syslog_early(const char *buffer, size_t length)
{
}

extern "C" void
platform_load_ucode(BootVolume &volume)
{
}
