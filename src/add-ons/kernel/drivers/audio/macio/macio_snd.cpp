/*
 * macio_snd - hmulti_audio driver for the Apple "mac-io" DBDMA audio output
 * (Burgundy codec path on dingusppc's iMac G3 / Paddington mac-io).
 *
 * Playback only (dingusppc's audio-in DBDMA channel is stubbed). The media kit
 * writes interleaved 16-bit stereo @ 44100 into a small ring of DMA buffers; a
 * self-looping DBDMA command list streams those buffers out the audio-out
 * channel continuously, and dingusppc's SoundServer pulls them to the host via
 * cubeb. A timer thread paces B_MULTI_BUFFER_EXCHANGE at the buffer rate (the
 * same approach null_audio uses) - dingusppc asserts the DBDMA completion IRQ
 * as a level it never deasserts between buffers, so a per-buffer hardware
 * interrupt is unreliable here; the pacing thread is the robust choice on the
 * emulator. (Real hardware wants the DMA_DAVBUS_Tx interrupt = mac-io IRQ 8,
 * plus Burgundy codec init - a later step.)
 *
 * Endianness (verified against dingusppc): DBDMA registers + command
 * descriptor fields are little-endian (B_HOST_TO_LENDIAN); audio samples are
 * big-endian int16 (native PPC stores).
 */

#include <KernelExport.h>
#include <Drivers.h>
#include <ByteOrder.h>
#include <hmulti_audio.h>
#include <string.h>


// ---- mac-io / DBDMA (see the Stage A tone proof) ----
#define MACIO_PHYS_BASE			0x80800000
#define MACIO_DBDMA_OFFSET		0x8000
#define AUDIO_OUT_CHANNEL		8
#define AUDIO_OUT_OFFSET		(MACIO_DBDMA_OFFSET | (AUDIO_OUT_CHANNEL << 8))

#define DBDMA_CH_CTRL			0x00
#define DBDMA_CH_STAT			0x04
#define DBDMA_CMD_PTR_LO		0x0C
#define DBDMA_RUN				0x8000

#define DBDMA_OUTPUT_MORE		0
#define DBDMA_BR_ALWAYS			0x0C		// cmd_bits branch field = always

typedef struct {
	uint16	req_count;
	uint8	cmd_bits;
	uint8	cmd_key;		// opcode = cmd_key >> 4
	uint32	address;
	uint32	cmd_arg;
	uint16	res_count;
	uint16	xfer_stat;
} dbdma_cmd;

// ---- audio format ----
#define SND_RATE				44100
#define SND_CHANNELS			2
#define SND_SAMPLE_SIZE			2			// signed 16-bit
#define SND_FRAME_SIZE			(SND_CHANNELS * SND_SAMPLE_SIZE)

#define NUM_BUFFERS				2
#define FRAMES_PER_BUFFER		1024

#define MULTI_AUDIO_DEV_PATH	"audio/hmulti"
#define MULTI_AUDIO_BASE_ID		1024
#define MULTI_AUDIO_MASTER_ID	0


typedef struct {
	// mac-io DBDMA registers
	area_id		reg_area;
	addr_t		channel_base;

	// playback DMA buffer ring (user-accessible; the media kit writes here)
	area_id		buffer_area;
	uint8*		buffer_base;
	phys_addr_t	buffer_phys;
	void*		buffers[NUM_BUFFERS];

	// DBDMA command list (kernel only)
	area_id		cmd_area;
	dbdma_cmd*	cmds;
	phys_addr_t	cmds_phys;

	uint32		num_buffers;
	uint32		buffer_frames;
	uint32		format;
	uint32		rate;

	// buffer-exchange pacing
	sem_id		buffer_ready_sem;
	thread_id	pace_thread;
	volatile bool running;
	spinlock	lock;
	uint32		buffer_cycle;
	uint32		frames_count;
	bigtime_t	real_time;
} device_t;


int32 api_version = B_CUR_DRIVER_API_VERSION;

static device_t sDevice;


static inline void
dbdma_write(uint32 reg, uint32 value)
{
	*(volatile uint32*)(sDevice.channel_base + reg)
		= B_HOST_TO_LENDIAN_INT32(value);
}


// Map the mac-io DBDMA register page once. Safe to call repeatedly.
static status_t
map_registers(void)
{
	if (sDevice.reg_area >= 0)
		return B_OK;

	void* regs = NULL;
	sDevice.reg_area = map_physical_memory("macio dbdma regs",
		MACIO_PHYS_BASE + MACIO_DBDMA_OFFSET, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &regs);
	if (sDevice.reg_area < 0)
		return sDevice.reg_area;
	sDevice.channel_base = (addr_t)regs
		+ (AUDIO_OUT_OFFSET - MACIO_DBDMA_OFFSET);
	return B_OK;
}


static void
free_buffers(void)
{
	if (sDevice.cmd_area >= 0) {
		delete_area(sDevice.cmd_area);
		sDevice.cmd_area = -1;
	}
	if (sDevice.buffer_area >= 0) {
		delete_area(sDevice.buffer_area);
		sDevice.buffer_area = -1;
	}
}


// Allocate the playback buffer ring (user-accessible) + the DBDMA command list.
static status_t
create_buffers(uint32 numBuffers, uint32 bufferFrames)
{
	free_buffers();

	uint32 bufBytes = bufferFrames * SND_FRAME_SIZE;
	uint32 alloc = (bufBytes * numBuffers + B_PAGE_SIZE - 1) & ~(B_PAGE_SIZE - 1);

	// Buffers: contiguous (single DBDMA data run per buffer) and user-readable/
	// writable so the media_addon_server can fill them (same as hda).
	void* base = NULL;
	sDevice.buffer_area = create_area("macio_snd buffers", &base,
		B_ANY_KERNEL_ADDRESS, alloc, B_CONTIGUOUS,
		B_READ_AREA | B_WRITE_AREA);
	if (sDevice.buffer_area < 0)
		return sDevice.buffer_area;

	physical_entry pe;
	get_memory_map(base, alloc, &pe, 1);
	sDevice.buffer_base = (uint8*)base;
	sDevice.buffer_phys = pe.address;
	for (uint32 i = 0; i < numBuffers; i++)
		sDevice.buffers[i] = (uint8*)base + i * bufBytes;

	// DBDMA command list: one page, contiguous, kernel only.
	void* cmdBase = NULL;
	sDevice.cmd_area = create_area("macio_snd dbdma cmds", &cmdBase,
		B_ANY_KERNEL_ADDRESS, B_PAGE_SIZE, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (sDevice.cmd_area < 0) {
		free_buffers();
		return sDevice.cmd_area;
	}
	get_memory_map(cmdBase, B_PAGE_SIZE, &pe, 1);
	sDevice.cmds = (dbdma_cmd*)cmdBase;
	sDevice.cmds_phys = pe.address;

	sDevice.num_buffers = numBuffers;
	sDevice.buffer_frames = bufferFrames;
	return B_OK;
}


// Build a DBDMA command ring that plays buffer 0, 1, ... and branches back to
// 0 forever. Commands are contiguous, so a non-branching command falls through
// to the next; only the last one branches (to command 0).
static void
build_dbdma_ring(void)
{
	uint32 bufBytes = sDevice.buffer_frames * SND_FRAME_SIZE;
	for (uint32 i = 0; i < sDevice.num_buffers; i++) {
		dbdma_cmd* c = &sDevice.cmds[i];
		memset(c, 0, sizeof(*c));
		c->req_count = B_HOST_TO_LENDIAN_INT16(bufBytes);
		c->cmd_key = DBDMA_OUTPUT_MORE << 4;
		c->address = B_HOST_TO_LENDIAN_INT32(
			(uint32)(sDevice.buffer_phys + i * bufBytes));
		if (i == sDevice.num_buffers - 1) {
			c->cmd_bits = DBDMA_BR_ALWAYS;
			c->cmd_arg = B_HOST_TO_LENDIAN_INT32((uint32)sDevice.cmds_phys);
		}
	}
	__asm__ volatile("sync; eieio" ::: "memory");
}


static int32
pace_thread(void* arg)
{
	// Fake the per-buffer completion the way null_audio does: advance the
	// buffer cycle and release the ready-sem every buffer period. dingusppc's
	// cubeb consumes the DBDMA ring at the real rate, so this stays in step.
	bigtime_t period = (bigtime_t)sDevice.buffer_frames * 1000000LL / sDevice.rate;
	while (sDevice.running) {
		cpu_status st = disable_interrupts();
		acquire_spinlock(&sDevice.lock);
		sDevice.real_time = system_time();
		sDevice.frames_count += sDevice.buffer_frames;
		sDevice.buffer_cycle = (sDevice.buffer_cycle + 1) % sDevice.num_buffers;
		release_spinlock(&sDevice.lock);
		restore_interrupts(st);

		release_sem_etc(sDevice.buffer_ready_sem, 1, B_DO_NOT_RESCHEDULE);
		snooze(period);
	}
	return 0;
}


static status_t
start_hardware(void)
{
	if (sDevice.running)
		return B_OK;

	build_dbdma_ring();

	// Stop the channel, point it at the command list, then run.
	dbdma_write(DBDMA_CH_CTRL, 0xFFFF0000);
	dbdma_write(DBDMA_CMD_PTR_LO, (uint32)sDevice.cmds_phys);
	dbdma_write(DBDMA_CH_CTRL, (DBDMA_RUN << 16) | DBDMA_RUN);

	sDevice.buffer_cycle = 0;
	sDevice.frames_count = 0;
	sDevice.real_time = system_time();
	sDevice.running = true;
	sDevice.pace_thread = spawn_kernel_thread(pace_thread, "macio_snd pacer",
		B_REAL_TIME_PRIORITY, NULL);
	if (sDevice.pace_thread < 0) {
		sDevice.running = false;
		return sDevice.pace_thread;
	}
	resume_thread(sDevice.pace_thread);
	dprintf("macio_snd: playback started (%" B_PRIu32 " x %" B_PRIu32
		" frames)\n", sDevice.num_buffers, sDevice.buffer_frames);
	return B_OK;
}


static void
stop_hardware(void)
{
	if (!sDevice.running)
		return;
	sDevice.running = false;
	if (sDevice.pace_thread >= 0) {
		status_t exitValue;
		wait_for_thread(sDevice.pace_thread, &exitValue);
		sDevice.pace_thread = -1;
	}
	// Clear RUN to stop the DBDMA channel.
	dbdma_write(DBDMA_CH_CTRL, DBDMA_RUN << 16);
}


// ---------------------------------------------------------------------------
//	hmulti_audio protocol
// ---------------------------------------------------------------------------

static multi_channel_info sChannels[] = {
	{ 0, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS, 0 },
	{ 1, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS, 0 },
	{ 2, B_MULTI_OUTPUT_BUS, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO },
	{ 3, B_MULTI_OUTPUT_BUS, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO },
};


static status_t
get_description(multi_description* data)
{
	multi_description description;
	if (user_memcpy(&description, data, sizeof(multi_description)) != B_OK)
		return B_BAD_ADDRESS;

	description.interface_version = B_CURRENT_INTERFACE_VERSION;
	description.interface_minimum = B_CURRENT_INTERFACE_VERSION;
	strcpy(description.friendly_name, "Burgundy (mac-io DBDMA)");
	strcpy(description.vendor_info, "Haiku/PowerPC");

	description.output_channel_count = 2;
	description.input_channel_count = 0;
	description.output_bus_channel_count = 2;
	description.input_bus_channel_count = 0;
	description.aux_bus_channel_count = 0;

	description.output_rates = B_SR_44100;
	description.input_rates = 0;
	description.max_cvsr_rate = 0;
	description.min_cvsr_rate = 0;
	description.output_formats = B_FMT_16BIT;
	description.input_formats = 0;
	description.lock_sources = B_MULTI_LOCK_INTERNAL;
	description.timecode_sources = 0;
	description.interface_flags = B_MULTI_INTERFACE_PLAYBACK;
	description.start_latency = 30000;
	strcpy(description.control_panel, "");

	if (user_memcpy(data, &description, sizeof(multi_description)) != B_OK)
		return B_BAD_ADDRESS;

	if ((size_t)description.request_channel_count >= B_COUNT_OF(sChannels)) {
		if (user_memcpy(data->channels, &sChannels, sizeof(sChannels)) != B_OK)
			return B_BAD_ADDRESS;
	}
	return B_OK;
}


static status_t
get_enabled_channels(multi_channel_enable* data)
{
	B_SET_CHANNEL(data->enable_bits, 0, true);
	B_SET_CHANNEL(data->enable_bits, 1, true);
	return B_OK;
}


static status_t
get_global_format(multi_format_info* data)
{
	data->output_latency = 0;
	data->input_latency = 0;
	data->timecode_kind = 0;
	data->output.format = sDevice.format;
	data->output.rate = sDevice.rate;
	data->input.format = 0;
	data->input.rate = 0;
	return B_OK;
}


static status_t
set_global_format(multi_format_info* data)
{
	// We only support 16-bit / 44100; accept whatever the mixer asks but keep
	// our fixed hardware format.
	sDevice.format = B_FMT_16BIT;
	sDevice.rate = B_SR_44100;
	return B_OK;
}


static int32
create_group_control(multi_mix_control* multi, int32 idx, int32 parent,
	int32 string, const char* name)
{
	multi->id = MULTI_AUDIO_BASE_ID + idx;
	multi->parent = parent;
	multi->flags = B_MULTI_MIX_GROUP;
	multi->master = MULTI_AUDIO_MASTER_ID;
	multi->string = (strind_id)string;
	if (name)
		strcpy(multi->name, name);
	return multi->id;
}


static status_t
list_mix_controls(multi_mix_control_info* data)
{
	create_group_control(data->controls + 0, 0, 0, 0, "Playback");
	data->control_count = 1;
	return B_OK;
}


static status_t
get_buffers(multi_buffer_list* data)
{
	uint32 sampleSize = SND_SAMPLE_SIZE;

	if (data->request_playback_buffers > NUM_BUFFERS
		|| data->request_playback_buffers < NUM_BUFFERS)
		data->request_playback_buffers = NUM_BUFFERS;
	if (data->request_playback_buffer_size == 0)
		data->request_playback_buffer_size = FRAMES_PER_BUFFER;
	// One DBDMA data run per buffer must fit in a 16-bit req_count.
	if (data->request_playback_buffer_size * SND_FRAME_SIZE > 0xF000)
		data->request_playback_buffer_size = 0xF000 / SND_FRAME_SIZE;

	data->flags = 0;

	status_t result = create_buffers(data->request_playback_buffers,
		data->request_playback_buffer_size);
	if (result != B_OK) {
		dprintf("macio_snd: create_buffers failed: %#" B_PRIx32 "\n", result);
		return result;
	}

	data->return_playback_buffers = sDevice.num_buffers;
	data->return_playback_channels = data->request_playback_channels;
	data->return_playback_buffer_size = sDevice.buffer_frames;

	for (int32 b = 0; b < data->return_playback_buffers; b++) {
		for (int32 c = 0; c < data->return_playback_channels; c++) {
			data->playback_buffers[b][c].base
				= (char*)sDevice.buffers[b] + sampleSize * c;
			data->playback_buffers[b][c].stride
				= sampleSize * data->return_playback_channels;
		}
	}

	data->return_record_buffers = 0;
	data->return_record_channels = 0;
	data->return_record_buffer_size = 0;
	return B_OK;
}


static status_t
buffer_exchange(multi_buffer_info* info)
{
	multi_buffer_info buffer_info;
	if (user_memcpy(&buffer_info, info, sizeof(multi_buffer_info)) != B_OK)
		return B_BAD_ADDRESS;

	if (!sDevice.running) {
		status_t result = start_hardware();
		if (result != B_OK)
			return result;
	}

	status_t result = acquire_sem(sDevice.buffer_ready_sem);
	if (result != B_OK)
		return result;

	cpu_status st = disable_interrupts();
	acquire_spinlock(&sDevice.lock);
	buffer_info.playback_buffer_cycle = sDevice.buffer_cycle;
	buffer_info.played_real_time = sDevice.real_time;
	buffer_info.played_frames_count = sDevice.frames_count;
	release_spinlock(&sDevice.lock);
	restore_interrupts(st);

	if (user_memcpy(info, &buffer_info, sizeof(multi_buffer_info)) != B_OK)
		return B_BAD_ADDRESS;
	return B_OK;
}


static status_t
buffer_force_stop(void)
{
	stop_hardware();
	return B_OK;
}


static status_t
multi_audio_control(uint32 op, void* arg)
{
	dprintf("macio_snd: ioctl op=%" B_PRIu32 " (base+%" B_PRIu32 ")\n", op,
		op - B_MULTI_GET_DESCRIPTION);
	switch (op) {
		case B_MULTI_GET_DESCRIPTION:		return get_description((multi_description*)arg);
		case B_MULTI_GET_ENABLED_CHANNELS:	return get_enabled_channels((multi_channel_enable*)arg);
		case B_MULTI_SET_ENABLED_CHANNELS:	return B_OK;
		case B_MULTI_GET_GLOBAL_FORMAT:		return get_global_format((multi_format_info*)arg);
		case B_MULTI_SET_GLOBAL_FORMAT:		return set_global_format((multi_format_info*)arg);
		case B_MULTI_LIST_MIX_CONTROLS:		return list_mix_controls((multi_mix_control_info*)arg);
		case B_MULTI_LIST_MIX_CONNECTIONS:	return B_ERROR;
		case B_MULTI_LIST_MIX_CHANNELS:		return B_ERROR;
		case B_MULTI_GET_MIX:				return B_ERROR;
		case B_MULTI_SET_MIX:				return B_ERROR;
		case B_MULTI_GET_BUFFERS:			return get_buffers((multi_buffer_list*)arg);
		case B_MULTI_BUFFER_EXCHANGE:		return buffer_exchange((multi_buffer_info*)arg);
		case B_MULTI_BUFFER_FORCE_STOP:		return buffer_force_stop();
	}
	return B_BAD_VALUE;
}


// ---------------------------------------------------------------------------
//	driver hooks
// ---------------------------------------------------------------------------

status_t
init_hardware(void)
{
	return B_OK;
}


status_t
init_driver(void)
{
	sDevice.reg_area = -1;
	sDevice.buffer_area = -1;
	sDevice.cmd_area = -1;
	sDevice.pace_thread = -1;
	sDevice.running = false;
	sDevice.format = B_FMT_16BIT;
	sDevice.rate = B_SR_44100;
	B_INITIALIZE_SPINLOCK(&sDevice.lock);
	sDevice.buffer_ready_sem = create_sem(0, "macio_snd buffer ready");
	if (sDevice.buffer_ready_sem < 0)
		return sDevice.buffer_ready_sem;
	return map_registers();
}


void
uninit_driver(void)
{
	stop_hardware();
	free_buffers();
	if (sDevice.reg_area >= 0)
		delete_area(sDevice.reg_area);
	if (sDevice.buffer_ready_sem >= 0)
		delete_sem(sDevice.buffer_ready_sem);
}


static status_t
snd_open(const char* name, uint32 flags, void** cookie)
{
	*cookie = &sDevice;
	return B_OK;
}


static status_t
snd_close(void* cookie)
{
	stop_hardware();
	return B_OK;
}


static status_t
snd_free(void* cookie)
{
	return B_OK;
}


static status_t
snd_control(void* cookie, uint32 op, void* arg, size_t len)
{
	return multi_audio_control(op, arg);
}


static status_t
snd_read(void* cookie, off_t position, void* data, size_t* numBytes)
{
	*numBytes = 0;
	return B_IO_ERROR;
}


static status_t
snd_write(void* cookie, off_t position, const void* data, size_t* numBytes)
{
	*numBytes = 0;
	return B_IO_ERROR;
}


static device_hooks sHooks = {
	snd_open,
	snd_close,
	snd_free,
	snd_control,
	snd_read,
	snd_write,
	NULL,
	NULL,
	NULL,
	NULL
};


const char**
publish_devices(void)
{
	static const char* names[] = {
		MULTI_AUDIO_DEV_PATH "/macio_snd/0",
		NULL
	};
	return names;
}


device_hooks*
find_device(const char* name)
{
	return &sHooks;
}
