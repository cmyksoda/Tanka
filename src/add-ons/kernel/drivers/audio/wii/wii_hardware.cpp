/*
 * Copyright 2007 Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Bek, host.haiku@gmx.de
 */
#include "driver.h"


status_t
wii_hw_create_virtual_buffers(device_stream_t* stream, const char* name)
{
	uint32 i;
	int buffer_size;
	int area_size;
	uint8* buffer;

	buffer_size = stream->num_channels
				* format_to_sample_size(stream->format)
				* stream->buffer_length;
	// 32-byte alignment required for Wii DMA
	buffer_size = (buffer_size + 31) & (~31);

	area_size = buffer_size * stream->num_buffers;
	area_size = (area_size + B_PAGE_SIZE - 1) & (~(B_PAGE_SIZE -1));

	stream->buffer_area = create_area("wii_audio_buffers", (void**)&buffer,
						B_ANY_KERNEL_ADDRESS, area_size,
						B_CONTIGUOUS, B_READ_AREA | B_WRITE_AREA);
	if (stream->buffer_area < B_OK)
		return stream->buffer_area;

	for (i = 0; i < stream->num_buffers; i++)
		stream->buffers[i] = buffer + (i * buffer_size);

	stream->buffer_ready_sem = create_sem(0, name);
	return B_OK;
}


// AI register offsets (base 0xCD006C00, 32-bit access, big-endian)
#define WII_AI_BASE      0xCD006C00
#define WII_AI_SIZE      0x20

// AI_CONTROL bits
#define AI_PSTAT         (1 << 0)   // Play status: 1 = playing
#define AI_AFR           (1 << 1)   // Auxiliary frequency (match RATE)
#define AI_AIINTMSK      (1 << 2)   // Interrupt mask
#define AI_AIINT         (1 << 3)   // Interrupt status (write 1 to clear)
#define AI_AIINTVLD      (1 << 4)   // Interrupt valid
#define AI_SCRESET       (1 << 5)   // Sample counter reset
#define AI_RATE_48KHZ    0          // bit 6 = 0: 48kHz
#define AI_RATE_32KHZ    (1 << 6)   // bit 6 = 1: 32kHz

// AI register indices (32-bit word offsets)
#define AI_CONTROL_REG   0   // 0xCD006C00
#define AI_VOLUME_REG    1   // 0xCD006C04
#define AI_AISCNT_REG    2   // 0xCD006C08
#define AI_AIIT_REG      3   // 0xCD006C0C


static int32
wii_audio_thread(void* cookie)
{
	bigtime_t sleepTime;
	device_t* device = (device_t*) cookie;
	int sampleRate;

	switch (device->playback_stream.rate) {
		case B_SR_48000:
			sampleRate = 48000;
			break;
		case B_SR_44100:
		default:
			sampleRate = 44100;
			break;
	}

	sleepTime = (device->playback_stream.buffer_length * 1000000LL) / sampleRate;

	// Map AI registers
	area_id aiArea = -1;
	void* aiRegs = NULL;
	aiArea = map_physical_memory("wii ai registers", WII_AI_BASE, WII_AI_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &aiRegs);
	volatile uint32* aiReg = (volatile uint32*)aiRegs;

	if (aiReg) {
		// Set 48kHz, clear any pending interrupt, enable playback
		uint32 ctrl = aiReg[AI_CONTROL_REG];
		ctrl &= ~AI_RATE_32KHZ;           // 48kHz (bit 6 = 0)
		ctrl &= ~AI_AFR;                  // AFR matches rate
		ctrl |= AI_AIINT;                 // Clear pending interrupt
		ctrl |= AI_SCRESET;               // Reset sample counter
		aiReg[AI_CONTROL_REG] = ctrl;

		// Set volume to max on both channels (0xFF each)
		aiReg[AI_VOLUME_REG] = 0x00FF00FF;

		// Enable playback
		ctrl = aiReg[AI_CONTROL_REG];
		ctrl &= ~AI_SCRESET;              // Clear reset bit
		ctrl |= AI_PSTAT;                 // Start playing
		aiReg[AI_CONTROL_REG] = ctrl;
	}

	bigtime_t nextTime = system_time();

	while (device->running) {
		cpu_status status;
		status = disable_interrupts();
		acquire_spinlock(&device->playback_stream.lock);
		device->playback_stream.real_time = system_time();
		device->playback_stream.frames_count += device->playback_stream.buffer_length;
		int cycle = device->playback_stream.buffer_cycle;
		device->playback_stream.buffer_cycle = (cycle + 1) % device->playback_stream.num_buffers;
		release_spinlock(&device->playback_stream.lock);

		acquire_spinlock(&device->record_stream.lock);
		device->record_stream.real_time = device->playback_stream.real_time;
		device->record_stream.frames_count += device->record_stream.buffer_length;
		device->record_stream.buffer_cycle = (device->record_stream.buffer_cycle +1) % device->record_stream.num_buffers;
		release_spinlock(&device->record_stream.lock);

		restore_interrupts(status);

		// The Wii's AI hardware fetches audio from the DSP output.
		// Without a running DSP microcode program, the AI plays silence.
		// For now, we rely on the polling thread for buffer timing only.
		// A full implementation requires loading the Wii DSP ucode and
		// programming the DSP's DMEM/ARAM DMA to stream from our buffers.
		//
		// The buffer data is still valid and available for any DSP-based
		// audio path that gets wired up later.

		release_sem_etc(device->playback_stream.buffer_ready_sem, 1, B_DO_NOT_RESCHEDULE);
		release_sem_etc(device->record_stream.buffer_ready_sem, 1, B_DO_NOT_RESCHEDULE);
		nextTime += sleepTime;
		snooze_until(nextTime, B_SYSTEM_TIMEBASE);
	}

	// Stop playback and clean up
	if (aiReg) {
		uint32 ctrl = aiReg[AI_CONTROL_REG];
		ctrl &= ~AI_PSTAT;
		aiReg[AI_CONTROL_REG] = ctrl;
	}

	if (aiArea >= 0) delete_area(aiArea);
	
	return B_OK;
}


status_t
wii_start_hardware(device_t* device)
{
	dprintf("wii_audio: %s\n", __func__);
	device->running = true;
	device->interrupt_thread = spawn_kernel_thread(wii_audio_thread, "wii_audio interrupter",
							B_REAL_TIME_PRIORITY, (void*)device);
	return resume_thread(device->interrupt_thread);
}


void
wii_stop_hardware(device_t* device)
{
	device->running = false;
	status_t status;
	wait_for_thread(device->interrupt_thread, &status);
}
