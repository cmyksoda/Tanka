/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * makebootable (Nintendo Wii)
 *
 * Nothing has to be written to a Haiku volume to make it bootable on a Wii.
 * The console boots whatever the Homebrew Channel hands it, so the boot chain
 * is boot.dol on the SD card's FAT partition, and that loader then reads the
 * BFS volume. This command exists so the generic image build and the
 * Installer keep a uniform interface across platforms.
 */

#include <stdio.h>
#include <string.h>


static const char* kCommandName = "makebootable";

static const char* kUsage =
"Usage: %s [ options ] <file or device> ...\n"
"\n"
"Makes the given Haiku volume bootable. On the Nintendo Wii the volume needs\n"
"no boot record: the Homebrew Channel loads apps/HaikuPowerPCii/boot.dol from\n"
"the SD card, and that loader reads the volume itself. This command therefore\n"
"only validates its arguments.\n"
"\n"
"Options:\n"
"  -h, --help            - Print this help text and exit.\n"
"  --dry-run             - Do not write anything.\n"
"  --start-offset <off>  - Accepted for compatibility; ignored.\n";


static void
print_usage(bool error)
{
	fprintf(error ? stderr : stdout, kUsage, kCommandName);
}


int
main(int argc, const char* const* argv)
{
	int volumes = 0;

	for (int i = 1; i < argc; i++) {
		const char* arg = argv[i];

		if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
			print_usage(false);
			return 0;
		}

		if (strcmp(arg, "--dry-run") == 0)
			continue;

		if (strcmp(arg, "--start-offset") == 0 || strcmp(arg, "--size") == 0) {
			if (++i >= argc) {
				fprintf(stderr, "%s: %s needs a value\n", kCommandName, arg);
				return 1;
			}
			continue;
		}

		if (arg[0] == '-') {
			fprintf(stderr, "%s: unknown option \"%s\"\n", kCommandName, arg);
			print_usage(true);
			return 1;
		}

		volumes++;
	}

	if (volumes == 0) {
		print_usage(true);
		return 1;
	}

	return 0;
}
