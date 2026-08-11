/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Antigravity
 */

#include "WiimoteInputDevice.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <Window.h>
#include <View.h>
#include <Message.h>
#include <OS.h>
#include <new>

// Wiimote HID button bitmasks (standard)
#define WIIMOTE_BUTTON_A 0x0008
#define WIIMOTE_BUTTON_B 0x0004
#define WIIMOTE_BUTTON_LEFT 0x0100
#define WIIMOTE_BUTTON_RIGHT 0x0200
#define WIIMOTE_BUTTON_DOWN 0x0400
#define WIIMOTE_BUTTON_UP 0x0800
#define WIIMOTE_BUTTON_PLUS 0x1000
#define WIIMOTE_BUTTON_2 0x0001
#define WIIMOTE_BUTTON_1 0x0002
#define WIIMOTE_BUTTON_MINUS 0x0010
#define WIIMOTE_BUTTON_HOME 0x0080


extern "C" BInputServerDevice*
instantiate_input_device()
{
	return new(std::nothrow) WiimoteInputDevice();
}


WiimoteInputDevice::WiimoteInputDevice()
	: fThread(-1),
	  fActive(false),
	  fDeviceFd(-1),
	  fLastButtons(0)
{
}


WiimoteInputDevice::~WiimoteInputDevice()
{
	if (fActive)
		Stop(NULL, NULL);
}


status_t
WiimoteInputDevice::InitCheck()
{
	input_device_ref deviceRef = { "Wiimote", B_POINTING_DEVICE, (void*)this };
	input_device_ref* deviceList[2] = { &deviceRef, NULL };
	RegisterDevices(deviceList);

	return B_OK;
}


status_t
WiimoteInputDevice::Start(const char* name, void* cookie)
{
	if (fActive)
		return B_OK;

	// Open the Wiimote HID device node (this assumes a kernel driver exposes it here)
	fDeviceFd = open("/dev/input/wiimote/0", O_RDONLY);
	if (fDeviceFd < 0) {
		// Device not found, but we'll start anyway in case it gets plugged in later
		// (A full implementation would use B_DEVICE_ADDED notifications)
	}

	fActive = true;
	fThread = spawn_thread(_ThreadEntry, "Wiimote Polling Thread",
		B_DISPLAY_PRIORITY, this);
	
	if (fThread >= B_OK)
		resume_thread(fThread);
	else {
		fActive = false;
		if (fDeviceFd >= 0)
			close(fDeviceFd);
		return fThread;
	}

	return B_OK;
}


status_t
WiimoteInputDevice::Stop(const char* name, void* cookie)
{
	if (!fActive)
		return B_OK;

	fActive = false;
	
	if (fDeviceFd >= 0) {
		close(fDeviceFd);
		fDeviceFd = -1;
	}

	status_t status;
	wait_for_thread(fThread, &status);
	return B_OK;
}


status_t
WiimoteInputDevice::Control(const char* name, void* cookie,
	uint32 command, BMessage* message)
{
	return B_OK;
}


int32
WiimoteInputDevice::_ThreadEntry(void* arg)
{
	WiimoteInputDevice* device = (WiimoteInputDevice*)arg;
	device->_PollLoop();
	return B_OK;
}


void
WiimoteInputDevice::_PollLoop()
{
	uint8 buffer[32];

	while (fActive) {
		if (fDeviceFd < 0) {
			snooze(1000000); // 1 second
			fDeviceFd = open("/dev/input/wiimote/0", O_RDONLY);
			continue;
		}

		ssize_t bytesRead = read(fDeviceFd, buffer, sizeof(buffer));
		if (bytesRead < 0) {
			close(fDeviceFd);
			fDeviceFd = -1;
			continue;
		}

		// A standard Wiimote Core Buttons report is 0x30 or 0x31.
		// Byte 0: Report ID
		// Byte 1-2: Button data
		// Byte 3-12: Accelerometer/IR data (depending on report type)
		
		if (bytesRead >= 3) {
			uint16 buttons = (buffer[1] << 8) | buffer[2];
			
			uint32 mouseButtons = 0;
			
			// Map A button to Left Click
			if (buttons & WIIMOTE_BUTTON_A)
				mouseButtons |= B_PRIMARY_MOUSE_BUTTON;
				
			// Map B button to Right Click
			if (buttons & WIIMOTE_BUTTON_B)
				mouseButtons |= B_SECONDARY_MOUSE_BUTTON;

			if (mouseButtons != fLastButtons) {
				BMessage* msg = new BMessage(mouseButtons == 0 ? B_MOUSE_UP : B_MOUSE_DOWN);
				msg->AddInt64("when", system_time());
				msg->AddInt32("buttons", mouseButtons);
				msg->AddInt32("clicks", 1);
				
				EnqueueMessage(msg);
				fLastButtons = mouseButtons;
			}
			
			// Parse IR data from Report 0x33
			if (bytesRead >= 18 && buffer[0] == 0x33) {
				uint8 b0 = buffer[6];
				uint8 b1 = buffer[7];
				uint8 b2 = buffer[8];
				
				if (b0 != 0xFF && b1 != 0xFF && b2 != 0xFF) {
					uint16 irX = b0 | (((b2 >> 4) & 0x03) << 8);
					uint16 irY = b1 | (((b2 >> 6) & 0x03) << 8);
					
					// Wii IR camera is 1024x768. 
					// Since the camera is on the remote and the sensor bar is fixed, 
					// moving the remote left moves the sensor bar right in the camera's view.
					float xPos = (1023.0f - irX) / 1023.0f;
					float yPos = irY / 767.0f; // Depending on bar position, this might need inversion
					
					BMessage* moveMsg = new BMessage(B_MOUSE_MOVED);
					moveMsg->AddInt64("when", system_time());
					moveMsg->AddFloat("x", xPos);
					moveMsg->AddFloat("y", yPos);
					// Add tablet subtype so app_server scales it absolutely
					moveMsg->AddInt32("be:device_subtype", B_TABLET_POINTING_DEVICE);
					EnqueueMessage(moveMsg);
				}
			}
		}
	}
}
