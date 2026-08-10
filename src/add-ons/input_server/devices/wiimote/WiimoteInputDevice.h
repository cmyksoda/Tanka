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
	// TODO: Add bluetooth pairing, polling thread, and HID translation
};

extern "C" BInputServerDevice* instantiate_input_device();

#endif	// WIIMOTE_INPUT_DEVICE_H
