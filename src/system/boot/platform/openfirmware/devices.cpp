/*
 * Copyright 2003-2006, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2010, Andreas Färber <andreas.faerber@web.de>
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#include <stdlib.h>
#include <string.h>

#include <OS.h>

#include <boot/partitions.h>
#include <boot/platform.h>
#include <boot/vfs.h>
#include <boot/stdio.h>
#include <boot/stage2.h>
#include <boot/net/IP.h>
#include <boot/net/NetStack.h>
#include <boot/net/RemoteDisk.h>
#include <platform/openfirmware/devices.h>
#include <platform/openfirmware/openfirmware.h>
#include <util/kernel_cpp.h>

#include "Handle.h"
#include "machine.h"


#define ENABLE_ISCSI


char sBootPath[192];


status_t 
platform_add_boot_device(struct stage2_args *args, NodeList *devicesList)
{
	// print out the boot path (to be removed later?)

	int length = of_getprop(gChosen, "bootpath", sBootPath, sizeof(sBootPath));
	if (length <= 1)
		return B_ENTRY_NOT_FOUND;
	printf("boot path = \"%s\"\n", sBootPath);

	intptr_t node = of_finddevice(sBootPath);
	if (node != OF_FAILED) {
		char type[16];
		of_getprop(node, "device_type", type, sizeof(type));
		printf("boot type = %s\n", type);

		// If the boot device is a network device, we try to find a
		// "remote disk" at this point.
		if (strcmp(type, "network") == 0) {
			// init the net stack
			status_t error = net_stack_init();
			if (error != B_OK)
				return error;

			ip_addr_t bootAddress = 0;
			char* bootArgs = strrchr(sBootPath, ':');
			if (bootArgs != NULL) {
				bootArgs++;
				char* comma = strchr(bootArgs, ',');
				if (comma != NULL && comma - bootArgs > 0) {
					comma[0] = '\0';
					bootAddress = ip_parse_address(bootArgs);
					comma[0] = ',';
				}
			}
			if (bootAddress == 0) {
				intptr_t package = of_finddevice("/options");
				char defaultServerIP[16];
				int bytesRead = of_getprop(package, "default-server-ip",
					defaultServerIP, sizeof(defaultServerIP) - 1);
				if (bytesRead != OF_FAILED && bytesRead > 1) {
					defaultServerIP[bytesRead] = '\0';
					bootAddress = ip_parse_address(defaultServerIP);
				}
			}

			// init a native remote disk, if possible
			RemoteDisk *remoteDisk = RemoteDisk::FindAnyRemoteDisk();
			if (remoteDisk != NULL) {
				devicesList->Add(remoteDisk);
				return B_OK;
			}

			return B_ENTRY_NOT_FOUND;
		}

		if (strcmp("block", type) != 0) {
			printf("boot device is not a block device!\n");
			return B_ENTRY_NOT_FOUND;
		}
	} else
		printf("could not open boot path.\n");

	// "bootpath" is the path Open Firmware used to load this boot loader
	// itself: a device path plus a ":<boot-partition>,\haikuloader.elf"
	// argument. That boot-partition number is a CHRP/PReP bootstrap-only
	// pseudo partition used just to locate the loader image, not a
	// generically openable block device - re-opening it (with or without
	// the file part, with or without the ":0" disk-label-bypass argument
	// used below in platform_add_block_devices()) fails. Worse, on this
	// firmware a failed open attempt against the device leaves it in a
	// state where a *second* open of the same underlying device (e.g.
	// the one platform_add_block_devices() does moments later while
	// scanning for partitions) hangs indefinitely instead of failing.
	// So don't attempt to open the boot device here at all; just defer
	// entirely to the scan in platform_add_block_devices(), which finds
	// and opens every block device - including this one - itself.
	return B_ENTRY_NOT_FOUND;
}


status_t
platform_get_boot_partitions(struct stage2_args *args, Node *device,
	NodeList *list, NodeList *partitionList)
{
	NodeIterator iterator = list->GetIterator();
	boot::Partition *partition = NULL;
	while ((partition = (boot::Partition *)iterator.Next()) != NULL) {
		// ToDo: just take the first partition for now
		partitionList->Insert(partition);
		return B_OK;
	}

	return B_ENTRY_NOT_FOUND;
}


void
platform_cleanup_devices()
{
	net_stack_cleanup();
}


#define DUMPED_BLOCK_SIZE 16

void
dumpBlock(const char *buffer, int size, const char *prefix)
{
	int i;
	
	for (i = 0; i < size;) {
		int start = i;

		printf(prefix);
		for (; i < start+DUMPED_BLOCK_SIZE; i++) {
			if (!(i % 4))
				printf(" ");

			if (i >= size)
				printf("  ");
			else
				printf("%02x", *(unsigned char *)(buffer + i));
		}
		printf("  ");

		for (i = start; i < start + DUMPED_BLOCK_SIZE; i++) {
			if (i < size) {
				char c = buffer[i];

				if (c < 30)
					printf(".");
				else
					printf("%c", c);
			} else
				break;
		}
		printf("\n");
	}
}


status_t
platform_add_block_devices(stage2_args *args, NodeList *devicesList)
{
	// add all block devices to the list of possible boot devices

	intptr_t cookie = 0;
	char path[256];
	status_t status;
	while ((status = of_get_next_device(&cookie, 0, "block", path,
			sizeof(path))) == B_OK) {
		if (!strcmp(path, sBootPath)) {
			// don't add the boot device twice
			continue;
		}

		// Adjust the arguments passed to the open command so that
		// the disk-label package is by-passed - unfortunately,
		// this is implementation specific (and I found no docs
		// for the Apple OF disk-label usage, of course)

		// SUN's OpenBoot:
		//strcpy(path + strlen(path), ":nolabel");
		// Apple:
		char bypassPath[256];
		strlcpy(bypassPath, path, sizeof(bypassPath));
		if (gMachine & MACHINE_MAC)
			strlcat(bypassPath, ":0", sizeof(bypassPath));

		printf("\t%s\n", bypassPath);

		// Try the disk-label bypass path first (this is the original,
		// historical behavior). We deliberately do NOT retry with a
		// second open() call on ATAPI/CD-ROM media - a second open of
		// the same underlying device can hang outright on this
		// platform rather than cleanly failing, and this is true even
		// when the second attempt uses a different argument string (not
		// just an identical retry). For non-ATAPI disks, a second
		// open() using the plain path (letting the real disk-label
		// package parse the partition map) is safe and needed for at
		// least some disks where the bypass path fails to open at all.
		bool isAtapi = false;
		for (const char *scan = path; *scan != '\0'; scan++) {
			if (!strncmp(scan, "cdrom", 5)) {
				isAtapi = true;
				break;
			}
		}
		intptr_t handle = of_open(bypassPath);
		if (handle == OF_FAILED && (gMachine & MACHINE_MAC) && !isAtapi) {
			printf("\t\t(bypass path failed, trying plain path %s)\n", path);
			handle = of_open(path);
		}
		if (handle == OF_FAILED) {
			puts("\t\t(failed)");
			continue;
		}

		Handle *device = new(nothrow) Handle(handle);
		printf("\t\t(could open device, handle = %p, node = %p)\n",
			(void *)handle, device);

		devicesList->Add(device);
	}
	printf("\t(loop ended with %ld)\n", status);

	return B_OK;
}


status_t 
platform_register_boot_device(Node *device)
{
	disk_identifier disk;

	disk.bus_type = UNKNOWN_BUS;
	disk.device_type = UNKNOWN_DEVICE;
	disk.device.unknown.size = device->Size();

	gBootParams.SetData(BOOT_VOLUME_DISK_IDENTIFIER, B_RAW_TYPE, &disk,
		sizeof(disk_identifier));

	return B_OK;
}		

