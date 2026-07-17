/*
 * Copyright 2003, Axel Dörfler, axeld@pinc-software.de.
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#include "Handle.h"

#include <SupportDefs.h>

#include <platform/openfirmware/openfirmware.h>
#include <util/kernel_cpp.h>


Handle::Handle(intptr_t handle, bool takeOwnership)
	:
	fHandle(handle),
	fOwnHandle(takeOwnership)
{
}


Handle::Handle(void)
	:
	fHandle(0)
{
}


Handle::~Handle()
{
	if (fOwnHandle)
		of_close(fHandle);
}


void
Handle::SetHandle(intptr_t handle, bool takeOwnership)
{
	if (fHandle && fOwnHandle)
		of_close(fHandle);

	fHandle = handle;
	fOwnHandle = takeOwnership;
}


ssize_t
Handle::ReadAt(void *cookie, off_t pos, void *buffer, size_t bufferSize)
{
	if (pos == -1 || of_seek(fHandle, pos) != OF_FAILED) {
		ssize_t result = of_read(fHandle, buffer, bufferSize);

		// Some Open Firmware disk devices return 0 on the very first
		// real read after being opened (seen even on the parent
		// device the disk-label package itself reads from at open
		// time) - a transient "not ready yet" condition, not EOF (a
		// 0-byte result at the very start of a multi-hundred-MB
		// device can't legitimately be EOF). Retry the *read* only
		// (never the open - a second open of the same device can
		// hang outright on this platform) a few times, with a plain
		// CPU busy-loop delay that makes no further Open Firmware
		// calls, since polling the firmware itself was observed to
		// destabilize it.
		for (int32 attempt = 0; result == 0 && bufferSize > 0
				&& pos >= 0 && attempt < 20; attempt++) {
			for (vint32 spin = 0; spin < 1000000; spin++)
				asm volatile("" ::: "memory");
			if (of_seek(fHandle, pos) == OF_FAILED)
				break;
			result = of_read(fHandle, buffer, bufferSize);
		}

		return result;
	}

	return B_ERROR;
}


ssize_t
Handle::WriteAt(void *cookie, off_t pos, const void *buffer, size_t bufferSize)
{
	if (pos == -1 || of_seek(fHandle, pos) != OF_FAILED)
		return of_write(fHandle, buffer, bufferSize);

	return B_ERROR;
}


off_t
Handle::Size() const
{
	// ToDo: fix this!
	return 1024LL * 1024 * 1024 * 1024;
		// 1024 GB
}

