/*
 * VIA-CUDA ADB input driver for PowerPC Macs (and dingusppc).
 *
 * The Cuda MCU is an ADB/I2C/RTC/power microcontroller attached to a 6522 VIA
 * inside the mac-io chip. The host talks to Cuda by shifting bytes through the
 * VIA shift register (SR) with a three-wire handshake (TIP/BYTEACK/TREQ) in the
 * VIA data-B register. Cuda auto-polls the ADB bus and hands us keyboard
 * (address 2) and mouse (address 3) reports, which we turn into Haiku
 * raw_key_info / mouse_movement events on /dev/input/{keyboard,mouse}/adb/0.
 *
 * Authors:
 *		Sean Malseed
 *		Claude (Anthropic), paired via Claude Code
 */

#include <KernelExport.h>
#include <Drivers.h>
#include <OS.h>

#include <string.h>
#include <stdio.h>

#include <keyboard_mouse_driver.h>


#define INFO(x...)		dprintf("adb: " x)
//#define TRACE(x...)	dprintf("adb: " x)
#define TRACE(x...)		do {} while (0)


int32 api_version = B_CUR_DRIVER_API_VERSION;

// VIA register indices; byte offset of register N is (N << 9) (regs 0x200 apart).
enum {
	VIA_B    = 0x00, VIA_SR = 0x0A, VIA_ACR = 0x0B,
	VIA_PCR  = 0x0C, VIA_IFR = 0x0D, VIA_IER = 0x0E,
};

// Cuda handshake bits in VIA_B (active low: 0 = asserted).
#define CUDA_TIP		0x20	// transaction in progress (host -> Cuda)
#define CUDA_BYTEACK	0x10	// byte acknowledge (host -> Cuda)
#define CUDA_TREQ		0x08	// Cuda requests a transaction (Cuda -> host)

#define VIA_IF_SR		0x04	// shift-register interrupt flag
#define VIA_IER_SET		0x80	// bit7=1 in an IER write sets the given bits

#define VIA_ACR_SR_MASK	0x1c
#define VIA_ACR_SR_OUT	0x1c	// shift out under Cuda's clock
#define VIA_ACR_SR_IN	0x0c	// shift in  under Cuda's clock

#define CUDA_PKT_ADB	0x00
#define CUDA_PKT_PSEUDO	0x01
#define CUDA_START_STOP_AUTOPOLL	0x01

// dingusppc / Grackle mac-io physical base; Cuda VIA is at + 0x16000.
// TODO: discover from OpenFirmware / mac-io PCI BAR (UniNorth uses 0x80000000).
#define MACIO_PHYS_BASE		0x80800000
#define VIA_OFFSET			0x16000
#define VIA_CUDA_IRQ		0x12

// Map ADB keycodes (register-0 talk data) to Haiku keycodes (as used by the
// system keymap). Best-effort for the main keys; unmapped keys pass 0. ADB
// keycodes are 7-bit (bit7 = key-up). Index = ADB keycode.
static const uint8 kAdbToHaiku[128] = {
	/* 0x00 */ 0x3c,0x3e,0x3f,0x40,0x42,0x41,0x26,0x28, // a s d f h g z x
	/* 0x08 */ 0x25,0x27,0x00,0x24,0x29,0x2a,0x2b,0x2c, // c v (§) b q w e r
	/* 0x10 */ 0x2e,0x2d,0x12,0x13,0x14,0x15,0x17,0x16, // y t 1 2 3 4 6 5
	/* 0x18 */ 0x1c,0x1a,0x19,0x1b,0x21,0x18,0x30,0x20, // = 9 7 - 8 0 ] o
	/* 0x20 */ 0x31,0x2f,0x33,0x32,0x47,0x34,0x00,0x3d, // u [ i p enter l (') j
	/* 0x28 */ 0x45,0x38,0x43,0x39,0x3a,0x44,0x3b,0x00, // ; k , ' n m . (/)
	/* 0x30 */ 0x26,0x5e,0x22,0x1e,0x00,0x11,0x5c,0x4b, // tab space ` back (enter) esc ctrl cmd
	/* 0x38 */ 0x4c,0x4d,0x5d,0x5b,0x66,0x67,0x00,0x00, // shift caps opt lctrl lshift rshift
	/* 0x40 */ 0x00,0x64,0x00,0x37,0x00,0x3a,0x00,0x1f, // . (kp) * (kp) + clear
	/* 0x48 */ 0x00,0x00,0x00,0x23,0x5b,0x00,0x00,0x00, // = (kp) / (kp) enter
	/* 0x50 */ 0x00,0x37,0x00,0x58,0x59,0x5a,0x48,0x49, // - (kp) 0 1 2 3
	/* 0x58 */ 0x4a,0x53,0x54,0x55,0x63,0x64,0x65,0x00, // 4 5 6 7 8 9
	/* 0x60 */ 0x03,0x04,0x05,0x02,0x06,0x07,0x00,0x08, // F5 F6 F7 F3 F8 F9 F11
	/* 0x68 */ 0x00,0x0e,0x00,0x0c,0x00,0x0a,0x00,0x09, // F13 F14 F10 F12
	/* 0x70 */ 0x00,0x0f,0x1f,0x20,0x21,0x37,0x0b,0x35, // F15 help home pgup del F4 end
	/* 0x78 */ 0x0d,0x36,0x01,0x62,0x61,0x63,0x57,0x00, // F2 pgdn F1 left right down up
};


static area_id sRegisterArea = -1;
static addr_t sVIABase = 0;

// Cuda receive-transaction assembly.
static uint8 sReplyBuffer[16];
static int sReplyLength = 0;
static bool sReading = false;

// Keyboard event ring + blocking read.
#define KB_QUEUE_SIZE	64
static raw_key_info sKeyQueue[KB_QUEUE_SIZE];
static int sKeyHead = 0, sKeyTail = 0;
static sem_id sKeySem = -1;
static spinlock sKeyLock = B_SPINLOCK_INITIALIZER;

// Mouse event ring + blocking read.
#define MS_QUEUE_SIZE	256
static mouse_movement sMouseQueue[MS_QUEUE_SIZE];
static int sMouseHead = 0, sMouseTail = 0;
static sem_id sMouseSem = -1;
static spinlock sMouseLock = B_SPINLOCK_INITIALIZER;
static uint32 sMouseButtons = 0;

// Haiku mouse button bits (from InterfaceDefs.h; not pulled into kernel land).
#define ADB_PRIMARY_BUTTON		0x01
#define ADB_SECONDARY_BUTTON	0x02

// Double-click tracking. The input_server expects the driver to fill in the
// click count (matching the USB/PS2 mouse drivers); app_server only treats a
// press as a double-click when clicks == 2. fClickSpeed default 250 ms.
static uint32 sLastButtons = 0;
static int32 sClickCount = 0;
static bigtime_t sLastClickTime = 0;
static bigtime_t sClickSpeed = 250000;


static inline uint8
via_read(int reg)
{
	uint8 value = *(volatile uint8*)(sVIABase + (reg << 9));
	asm volatile("eieio" ::: "memory");
	return value;
}


static inline void
via_write(int reg, uint8 value)
{
	*(volatile uint8*)(sVIABase + (reg << 9)) = value;
	asm volatile("eieio" ::: "memory");
}


static void
queue_key(uint8 adbCode)
{
	raw_key_info info;
	info.timestamp = system_time();
	info.keycode = kAdbToHaiku[adbCode & 0x7f];
	info.is_keydown = (adbCode & 0x80) == 0;
	if (info.keycode == 0)
		return;

	acquire_spinlock(&sKeyLock);
	int next = (sKeyHead + 1) % KB_QUEUE_SIZE;
	if (next != sKeyTail) {
		sKeyQueue[sKeyHead] = info;
		sKeyHead = next;
		release_spinlock(&sKeyLock);
		release_sem_etc(sKeySem, 1, B_DO_NOT_RESCHEDULE);
	} else
		release_spinlock(&sKeyLock);
}


static void
queue_mouse(uint32 buttons, int dx, int dy)
{
	sMouseButtons = buttons;

	// Derive the click count on each fresh press, like MouseProtocolHandler.
	bigtime_t now = system_time();
	int32 clicks = 0;
	if (buttons != 0) {
		if (sLastButtons == 0) {
			if (sLastClickTime + sClickSpeed > now)
				sClickCount++;
			else
				sClickCount = 1;
		}
		sLastClickTime = now;
		clicks = sClickCount;
	}
	sLastButtons = buttons;

	mouse_movement m;
	memset(&m, 0, sizeof(m));
	m.buttons = buttons;
	m.xdelta = dx;
	// ADB/SDL accumulate Y positive-down; Haiku mouse deltas are positive-up
	// (BeOS convention - the USB HID driver negates Y the same way).
	m.ydelta = -dy;
	m.clicks = clicks;
	m.timestamp = now;

	acquire_spinlock(&sMouseLock);
	int next = (sMouseHead + 1) % MS_QUEUE_SIZE;
	if (next != sMouseTail) {
		sMouseQueue[sMouseHead] = m;
		sMouseHead = next;
		release_spinlock(&sMouseLock);
		release_sem_etc(sMouseSem, 1, B_DO_NOT_RESCHEDULE);
	} else
		release_spinlock(&sMouseLock);
}


static void
adb_process_reply(uint8* data, int length)
{
	TRACE("reply len %d: %02x %02x %02x %02x\n", length, data[0],
		length > 1 ? data[1] : 0, length > 2 ? data[2] : 0,
		length > 3 ? data[3] : 0);

	if (length < 3 || data[0] != CUDA_PKT_ADB)
		return;

	// dingusppc Cuda ADB response framing (viacuda.cpp response_header):
	//   data[0] = CUDA_PKT_ADB (0x00)
	//   data[1] = status flags  (ADB_STAT_AUTOPOLL | RESPONSE | ...)
	//   data[2] = the ADB command byte (bits7-4 = device address, low = reg)
	//   data[3..] = the device's register-0 payload
	uint8 address = (data[2] >> 4) & 0x0f;
	if (address == 2 && length >= 5) {
		// Keyboard reg 0: two 7-bit ADB keycodes, bit7 = up. 0xff = no key.
		TRACE("KEY %02x %02x\n", data[3], data[4]);
		if (data[3] != 0xff)
			queue_key(data[3]);
		if (data[4] != 0xff)
			queue_key(data[4]);
	} else if (address == 3 && length >= 5) {
		// Mouse reg 0 (standard 2-byte protocol, dingusppc adbmouse.cpp):
		//   data[3]: bit7 = ~button0 (left/primary), bits0-6 = signed Y
		//   data[4]: bit7 = ~button1 (right/secondary), bits0-6 = signed X
		// A cleared button bit means pressed; the second button was being
		// masked off before, so right-click never reached the desktop.
		uint32 buttons = 0;
		if ((data[3] & 0x80) == 0)
			buttons |= ADB_PRIMARY_BUTTON;
		if ((data[4] & 0x80) == 0)
			buttons |= ADB_SECONDARY_BUTTON;
		int dy = (int)(data[3] & 0x7f); if (dy & 0x40) dy -= 0x80;
		int dx = (int)(data[4] & 0x7f); if (dx & 0x40) dx -= 0x80;
		TRACE("MOUSE btn %#lx dx %d dy %d\n", buttons, dx, dy);
		queue_mouse(buttons, dx, dy);
	}
}


static int32
adb_interrupt(void* arg)
{
	uint8 ifr = via_read(VIA_IFR);
	if ((ifr & VIA_IF_SR) == 0)
		return B_UNHANDLED_INTERRUPT;

	via_write(VIA_IFR, VIA_IF_SR);
	uint8 portB = via_read(VIA_B);

	if (!sReading) {
		// An incoming transaction only begins when Cuda asserts TREQ (active
		// low, so bit clear = data available). Cuda also raises an SR
		// interrupt ~61us AFTER we end a transaction (its "idle acknowledge",
		// see ViaCuda::write) - with TREQ negated. If we treated that (or any
		// other spurious SR int) as a new transaction we would assert TIP,
		// read the stale last SR byte as a bogus 1-byte packet, end, and
		// thereby trigger the next idle-ack: an endless storm. Ignore any SR
		// int that is not an actual TREQ request.
		if ((portB & CUDA_TREQ) != 0)
			return B_HANDLED_INTERRUPT;

		sReading = true;
		sReplyLength = 0;
		via_write(VIA_ACR, (via_read(VIA_ACR) & ~VIA_ACR_SR_MASK)
			| VIA_ACR_SR_IN);
		(void)via_read(VIA_SR);
		via_write(VIA_B, (portB & ~CUDA_TIP) | CUDA_BYTEACK);
		return B_HANDLED_INTERRUPT;
	}

	uint8 byte = via_read(VIA_SR);
	if (sReplyLength < (int)sizeof(sReplyBuffer))
		sReplyBuffer[sReplyLength++] = byte;

	if ((portB & CUDA_TREQ) != 0) {
		via_write(VIA_B, portB | CUDA_TIP | CUDA_BYTEACK);
		sReading = false;
		adb_process_reply(sReplyBuffer, sReplyLength);
		return B_INVOKE_SCHEDULER;
	}

	via_write(VIA_B, portB ^ CUDA_BYTEACK);
	return B_HANDLED_INTERRUPT;
}


static void
cuda_send_polled(const uint8* data, int length)
{
	for (int i = 0; i < 100000; i++) {
		if ((via_read(VIA_B) & (CUDA_TIP | CUDA_TREQ)) == (CUDA_TIP | CUDA_TREQ))
			break;
	}
	via_write(VIA_ACR, (via_read(VIA_ACR) & ~VIA_ACR_SR_MASK) | VIA_ACR_SR_OUT);

	uint8 portB = via_read(VIA_B);
	for (int i = 0; i < length; i++) {
		via_write(VIA_SR, data[i]);
		if (i == 0) {
			portB &= ~CUDA_TIP;
			via_write(VIA_B, portB);
		}
		portB ^= CUDA_BYTEACK;
		via_write(VIA_B, portB);
		for (int j = 0; j < 100000; j++) {
			if ((via_read(VIA_IFR) & VIA_IF_SR) != 0)
				break;
		}
		via_write(VIA_IFR, VIA_IF_SR);
	}
	via_write(VIA_B, portB | CUDA_TIP | CUDA_BYTEACK);
	via_write(VIA_ACR, (via_read(VIA_ACR) & ~VIA_ACR_SR_MASK) | VIA_ACR_SR_IN);
}


// #pragma mark - device hooks


static status_t
adb_open(const char* name, uint32 flags, void** cookie)
{
	TRACE("open %s\n", name);
	*cookie = NULL;
	return B_OK;
}


static status_t
adb_close(void* cookie)
{
	return B_OK;
}


static status_t
adb_free(void* cookie)
{
	return B_OK;
}


static status_t
adb_read(void* cookie, off_t pos, void* buffer, size_t* length)
{
	*length = 0;
	return B_OK;
}


static status_t
adb_write(void* cookie, off_t pos, const void* buffer, size_t* length)
{
	*length = 0;
	return B_OK;
}


static status_t
keyboard_control(void* cookie, uint32 op, void* arg, size_t length)
{
	switch (op) {
		case KB_READ:
		{
			if (acquire_sem(sKeySem) != B_OK)
				return B_ERROR;
			raw_key_info info;
			cpu_status state = disable_interrupts();
			acquire_spinlock(&sKeyLock);
			info = sKeyQueue[sKeyTail];
			sKeyTail = (sKeyTail + 1) % KB_QUEUE_SIZE;
			release_spinlock(&sKeyLock);
			restore_interrupts(state);
			return user_memcpy(arg, &info, sizeof(raw_key_info));
		}
		case KB_GET_KEYBOARD_ID:
		{
			uint16 id = 0x83;	// generic extended keyboard
			return user_memcpy(arg, &id, sizeof(id));
		}
		case KB_SET_LEDS:
		case KB_SET_KEY_REPEATING:
		case KB_SET_KEY_NONREPEATING:
		case KB_SET_KEY_REPEAT_RATE:
		case KB_SET_KEY_REPEAT_DELAY:
		case KB_SET_CONTROL_ALT_DEL_TIMEOUT:
			return B_OK;
	}
	return B_DEV_INVALID_IOCTL;
}


static status_t
mouse_control(void* cookie, uint32 op, void* arg, size_t length)
{
	switch (op) {
		case MS_READ:
		{
			if (acquire_sem(sMouseSem) != B_OK)
				return B_ERROR;
			mouse_movement m;
			cpu_status state = disable_interrupts();
			acquire_spinlock(&sMouseLock);
			m = sMouseQueue[sMouseTail];
			sMouseTail = (sMouseTail + 1) % MS_QUEUE_SIZE;
			release_spinlock(&sMouseLock);
			restore_interrupts(state);
			return user_memcpy(arg, &m, sizeof(mouse_movement));
		}
		case MS_NUM_EVENTS:
		{
			int32 count;
			get_sem_count(sMouseSem, &count);
			return count;
		}
		case MS_SET_CLICKSPEED:
			return user_memcpy(&sClickSpeed, arg, sizeof(bigtime_t));
		case MS_SET_TYPE:
		case MS_SET_MAP:
		case MS_SET_ACCEL:
			return B_OK;
	}
	return B_DEV_INVALID_IOCTL;
}


static device_hooks sKeyboardHooks = {
	adb_open, adb_close, adb_free, keyboard_control,
	adb_read, adb_write, NULL, NULL, NULL, NULL
};

static device_hooks sMouseHooks = {
	adb_open, adb_close, adb_free, mouse_control,
	adb_read, adb_write, NULL, NULL, NULL, NULL
};


// #pragma mark - driver hooks


status_t
init_hardware(void)
{
	return B_OK;
}


status_t
init_driver(void)
{
	void* virtualBase = NULL;
	sRegisterArea = map_physical_memory("via-cuda registers",
		MACIO_PHYS_BASE + VIA_OFFSET, B_PAGE_SIZE * 16,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &virtualBase);
	if (sRegisterArea < 0)
		return sRegisterArea;
	sVIABase = (addr_t)virtualBase;

	sKeySem = create_sem(0, "adb keyboard");
	sMouseSem = create_sem(0, "adb mouse");
	if (sKeySem < 0 || sMouseSem < 0) {
		delete_area(sRegisterArea);
		return B_NO_MEMORY;
	}

	// Cuda init: SR shift-in under external clock, idle handshake lines,
	// clear + enable the SR interrupt.
	via_write(VIA_ACR, (via_read(VIA_ACR) & ~VIA_ACR_SR_MASK) | VIA_ACR_SR_IN);
	via_write(VIA_B, via_read(VIA_B) | CUDA_TIP | CUDA_BYTEACK);
	(void)via_read(VIA_SR);
	via_write(VIA_IFR, 0x7f);

	// Install the handler BEFORE unmasking the SR interrupt in the VIA, so a
	// pending/immediate interrupt can never dispatch into an empty vector.
	install_io_interrupt_handler(VIA_CUDA_IRQ, adb_interrupt, NULL, 0);

	via_write(VIA_IER, VIA_IER_SET | VIA_IF_SR);

	uint8 autopoll[] = { CUDA_PKT_PSEUDO, CUDA_START_STOP_AUTOPOLL, 0x01 };
	cuda_send_polled(autopoll, sizeof(autopoll));

	INFO("VIA-CUDA up (phys %#x), autopoll on\n", MACIO_PHYS_BASE + VIA_OFFSET);
	return B_OK;
}


void
uninit_driver(void)
{
	if (sRegisterArea >= 0) {
		remove_io_interrupt_handler(VIA_CUDA_IRQ, adb_interrupt, NULL);
		delete_area(sRegisterArea);
		sRegisterArea = -1;
	}
	if (sKeySem >= 0) delete_sem(sKeySem);
	if (sMouseSem >= 0) delete_sem(sMouseSem);
}


const char**
publish_devices(void)
{
	static const char* devices[] = {
		"input/keyboard/adb/0",
		"input/mouse/adb/0",
		NULL
	};
	return devices;
}


device_hooks*
find_device(const char* name)
{
	if (strstr(name, "keyboard") != NULL)
		return &sKeyboardHooks;
	if (strstr(name, "mouse") != NULL)
		return &sMouseHooks;
	return NULL;
}
