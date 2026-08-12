/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <platform/wii/wii.h>

#include <string.h>

#include <KernelExport.h>
#include <arch/cpu.h>
#include <lock.h>
#include <util/AutoLock.h>


// Storage is only reachable by RPC to IOS, the OS on Hollywood's ARM core.

#define IOS_OPEN				1
#define IOS_CLOSE				2
#define IOS_IOCTL				6
#define IOS_IOCTLV				7

#define IPC_AREA_SIZE			(128 * 1024)
#define IPC_BLOCK_OFFSET		0x000
#define IPC_VECTOR_OFFSET		0x040
#define IPC_PATH_OFFSET			0x100
#define IPC_BOUNCE_OFFSET		0x200
#define IPC_BOUNCE_SIZE			(IPC_AREA_SIZE - IPC_BOUNCE_OFFSET)

#define IPC_MAX_VECTORS			16
#define IPC_PATH_SIZE			0x100

// IOS wants its buffers cache line aligned, and Broadway's line is 32 bytes.
#define IPC_ALIGN				32

#define IPC_TIMEOUT				5000000
#define IPC_POLL_INTERVAL		100


struct ipc_request {
	uint32	cmd;
	int32	result;
	int32	fd;
	uint32	args[5];
};

struct ipc_vector {
	uint32	address;
	uint32	size;
};


static mutex sLock = MUTEX_INITIALIZER("wii ipc");
static area_id sArea = -1;
static addr_t sBase;
static phys_addr_t sPhysicalBase;
static addr_t sHollywoodBase;
static size_t sBounceUsed;
static bool sReportedStaleReply;


static inline volatile uint32 *
hw_reg(uint32 offset)
{
	return (volatile uint32 *)(sHollywoodBase + offset);
}


static inline struct ipc_request *
ipc_block(void)
{
	return (struct ipc_request *)(sBase + IPC_BLOCK_OFFSET);
}


static inline uint32
ipc_physical(const void *address)
{
	return (uint32)(sPhysicalBase + ((addr_t)address - sBase));
}


// IOS reaches memory behind Broadway's data cache, so sharing is hand written.
static void
ipc_flush(const void *address, size_t size)
{
	if (size == 0)
		return;

	addr_t end = (addr_t)address + size;
	for (addr_t line = (addr_t)address & ~(addr_t)(IPC_ALIGN - 1); line < end;
			line += IPC_ALIGN) {
		asm volatile("dcbf 0,%0" :: "r"(line));
	}
	asm volatile("sync");
}


static void
ipc_invalidate(const void *address, size_t size)
{
	if (size == 0)
		return;

	addr_t end = (addr_t)address + size;
	for (addr_t line = (addr_t)address & ~(addr_t)(IPC_ALIGN - 1); line < end;
			line += IPC_ALIGN) {
		asm volatile("dcbi 0,%0" :: "r"(line));
	}
	asm volatile("sync");
}


// Sets bits in the control register, preserving the interrupt enables.
static void
ipc_control(uint32 bits)
{
	*hw_reg(WII_HW_IPC_PPCCTRL)
		= (*hw_reg(WII_HW_IPC_PPCCTRL) & WII_IPC_CTRL_IY_MASK) | bits;
	eieio();
}


// IOS hands over nothing further while this source stays latched.
static void
ipc_clear_interrupt(void)
{
	*hw_reg(WII_HW_PPCIRQFLAG) = 1 << WII_IRQ_IPC;
	eieio();
}


static status_t
ipc_wait(uint32 bit)
{
	bigtime_t timeout = system_time() + IPC_TIMEOUT;

	while (true) {
		uint32 control = *hw_reg(WII_HW_IPC_PPCCTRL);
		eieio();
		if ((control & bit) != 0)
			return B_OK;

		if (system_time() >= timeout)
			return B_TIMED_OUT;

		snooze(IPC_POLL_INTERVAL);
	}
}


// Clears what an earlier session or an abandoned transaction left latched.
static void
ipc_drain(void)
{
	uint32 control = *hw_reg(WII_HW_IPC_PPCCTRL);
	eieio();

	if ((control & WII_IPC_CTRL_Y2) != 0) {
		ipc_control(WII_IPC_CTRL_Y2);
		ipc_clear_interrupt();
	}

	if ((control & WII_IPC_CTRL_Y1) == 0)
		return;

	if (!sReportedStaleReply) {
		dprintf("wii_ipc: dropping a reply left latched at %#" B_PRIx32 "\n",
			*hw_reg(WII_HW_IPC_ARMMSG));
		sReportedStaleReply = true;
	}

	ipc_control(WII_IPC_CTRL_Y1);
	ipc_clear_interrupt();
	ipc_control(WII_IPC_CTRL_X2);
}


static void *
ipc_bounce(size_t size)
{
	size_t offset = (sBounceUsed + IPC_ALIGN - 1) & ~(size_t)(IPC_ALIGN - 1);
	if (size > IPC_BOUNCE_SIZE - offset)
		return NULL;

	sBounceUsed = offset + size;
	return (void *)(sBase + IPC_BOUNCE_OFFSET + offset);
}


static status_t
ipc_init_locked(void)
{
	if (sArea >= 0)
		return B_OK;

	sHollywoodBase = wii_hollywood_registers();
	if (sHollywoodBase == 0)
		return B_NO_INIT;

	// IOS only ever sees physical addresses, so the staging area is contiguous.
	void *address;
	area_id area = create_area("wii ipc buffer", &address, B_ANY_KERNEL_ADDRESS,
		IPC_AREA_SIZE, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (area < 0)
		return area;

	physical_entry entry;
	status_t status = get_memory_map(address, IPC_AREA_SIZE, &entry, 1);
	if (status != B_OK) {
		delete_area(area);
		return status;
	}

	sArea = area;
	sBase = (addr_t)address;
	sPhysicalBase = entry.address;

	// This driver polls, so drop the enables the loader handed over set.
	*hw_reg(WII_HW_IPC_PPCCTRL) = 0;
	eieio();

	ipc_clear_interrupt();
	ipc_drain();

	return B_OK;
}


// Sends the block and waits for its reply, mirroring libogc's register order.
static status_t
ipc_transact(int32 *_result, void *output, size_t outputSize)
{
	struct ipc_request *request = ipc_block();
	uint32 address = ipc_physical(request);

	ipc_drain();
	ipc_flush(request, sizeof(struct ipc_request));

	*hw_reg(WII_HW_IPC_PPCMSG) = address;
	eieio();
	ipc_control(WII_IPC_CTRL_X1);

	status_t status = ipc_wait(WII_IPC_CTRL_Y2);
	if (status != B_OK)
		return status;

	ipc_control(WII_IPC_CTRL_Y2);
	ipc_clear_interrupt();

	// A reply the loader's own IOS session still owed can arrive ahead of ours.
	for (;;) {
		status = ipc_wait(WII_IPC_CTRL_Y1);
		if (status != B_OK)
			return status;

		uint32 reply = *hw_reg(WII_HW_IPC_ARMMSG);
		eieio();

		ipc_control(WII_IPC_CTRL_Y1);
		ipc_clear_interrupt();

		if (reply == address)
			break;

		if (!sReportedStaleReply) {
			dprintf("wii_ipc: ignoring stale IOS reply at %#" B_PRIx32 "\n",
				reply);
			sReportedStaleReply = true;
		}

		ipc_control(WII_IPC_CTRL_X2);
	}

	ipc_invalidate(request, sizeof(struct ipc_request));
	ipc_invalidate(output, outputSize);

	ipc_control(WII_IPC_CTRL_X2);

	*_result = request->result;
	return B_OK;
}


status_t
wii_ipc_init(void)
{
	MutexLocker locker(sLock);
	return ipc_init_locked();
}


int32
wii_ios_open(const char *path, uint32 mode)
{
	if (path == NULL)
		return B_BAD_VALUE;

	MutexLocker locker(sLock);
	status_t status = ipc_init_locked();
	if (status != B_OK)
		return status;

	size_t length = strnlen(path, IPC_PATH_SIZE);
	if (length == IPC_PATH_SIZE)
		return B_NAME_TOO_LONG;

	char *buffer = (char *)(sBase + IPC_PATH_OFFSET);
	memcpy(buffer, path, length + 1);
	ipc_flush(buffer, length + 1);

	struct ipc_request *request = ipc_block();
	memset(request, 0, sizeof(struct ipc_request));
	request->cmd = IOS_OPEN;
	request->args[0] = ipc_physical(buffer);
	request->args[1] = mode;

	int32 result;
	status = ipc_transact(&result, NULL, 0);
	if (status != B_OK)
		return status;

	return result;
}


int32
wii_ios_close(int32 fd)
{
	MutexLocker locker(sLock);
	status_t status = ipc_init_locked();
	if (status != B_OK)
		return status;

	struct ipc_request *request = ipc_block();
	memset(request, 0, sizeof(struct ipc_request));
	request->cmd = IOS_CLOSE;
	request->fd = fd;

	int32 result;
	status = ipc_transact(&result, NULL, 0);
	if (status != B_OK)
		return status;

	return result;
}


int32
wii_ios_ioctl(int32 fd, uint32 op, const void *in, size_t inSize, void *io,
	size_t ioSize)
{
	MutexLocker locker(sLock);
	status_t status = ipc_init_locked();
	if (status != B_OK)
		return status;

	sBounceUsed = 0;

	void *inBuffer = NULL;
	if (in != NULL && inSize > 0) {
		inBuffer = ipc_bounce(inSize);
		if (inBuffer == NULL)
			return B_NO_MEMORY;

		memcpy(inBuffer, in, inSize);
		ipc_flush(inBuffer, inSize);
	}

	void *ioBuffer = NULL;
	if (io != NULL && ioSize > 0) {
		ioBuffer = ipc_bounce(ioSize);
		if (ioBuffer == NULL)
			return B_NO_MEMORY;

		// Handed over filled: plenty of ioctls read their output buffer too.
		memcpy(ioBuffer, io, ioSize);
		ipc_flush(ioBuffer, ioSize);
	}

	struct ipc_request *request = ipc_block();
	memset(request, 0, sizeof(struct ipc_request));
	request->cmd = IOS_IOCTL;
	request->fd = fd;
	request->args[0] = op;
	request->args[1] = inBuffer != NULL ? ipc_physical(inBuffer) : 0;
	request->args[2] = inBuffer != NULL ? (uint32)inSize : 0;
	request->args[3] = ioBuffer != NULL ? ipc_physical(ioBuffer) : 0;
	request->args[4] = ioBuffer != NULL ? (uint32)ioSize : 0;

	int32 result;
	status = ipc_transact(&result, ioBuffer, ioSize);
	if (status != B_OK)
		return status;

	if (ioBuffer != NULL)
		memcpy(io, ioBuffer, ioSize);

	return result;
}


int32
wii_ios_ioctlv(int32 fd, uint32 op, uint32 countIn, uint32 countIO,
	struct wii_ios_vector *vectors)
{
	uint32 count = countIn + countIO;
	if (count > IPC_MAX_VECTORS || (count > 0 && vectors == NULL))
		return B_BAD_VALUE;

	MutexLocker locker(sLock);
	status_t status = ipc_init_locked();
	if (status != B_OK)
		return status;

	sBounceUsed = 0;

	struct ipc_vector *table = (struct ipc_vector *)(sBase + IPC_VECTOR_OFFSET);
	void *buffers[IPC_MAX_VECTORS];

	for (uint32 i = 0; i < count; i++) {
		buffers[i] = NULL;
		table[i].address = 0;
		table[i].size = 0;
		if (vectors[i].buffer == NULL || vectors[i].size == 0)
			continue;

		buffers[i] = ipc_bounce(vectors[i].size);
		if (buffers[i] == NULL)
			return B_NO_MEMORY;

		memcpy(buffers[i], vectors[i].buffer, vectors[i].size);
		ipc_flush(buffers[i], vectors[i].size);

		table[i].address = ipc_physical(buffers[i]);
		table[i].size = (uint32)vectors[i].size;
	}

	ipc_flush(table, count * sizeof(struct ipc_vector));

	struct ipc_request *request = ipc_block();
	memset(request, 0, sizeof(struct ipc_request));
	request->cmd = IOS_IOCTLV;
	request->fd = fd;
	request->args[0] = op;
	request->args[1] = countIn;
	request->args[2] = countIO;
	request->args[3] = ipc_physical(table);

	// Every vector was staged in order, so one span covers all the outputs.
	int32 result;
	status = ipc_transact(&result, (void *)(sBase + IPC_BOUNCE_OFFSET),
		sBounceUsed);
	if (status != B_OK)
		return status;

	for (uint32 i = countIn; i < count; i++) {
		if (buffers[i] != NULL)
			memcpy(vectors[i].buffer, buffers[i], vectors[i].size);
	}

	return result;
}
