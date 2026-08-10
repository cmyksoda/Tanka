/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Antigravity
 */
#ifndef WIIMOTE_INPUT_DEVICE_H
#define WIIMOTE_INPUT_DEVICE_H

#include <InputServerDevice.h>
#include <InterfaceDefs.h>

class WiimoteInputDevice : public BInputServerDevice {
public:
							WiimoteInputDevice();
	virtual					~WiimoteInputDevice();

	virtual status_t		InitCheck();

	virtual status_t		Start(const char* name, void* cookie);
	virtual status_t		Stop(const char* name, void* cookie);

	virtual status_t		Control(const char* name, void* cookie,
								uint32 command, BMessage* message);

private:
	static int32			_ThreadEntry(void* arg);
	void					_PollLoop();

	thread_id				fThread;
	volatile bool			fActive;
	int						fDeviceFd;
	uint32					fLastButtons;
};

extern "C" BInputServerDevice* instantiate_input_device();

#endif	// WIIMOTE_INPUT_DEVICE_H
