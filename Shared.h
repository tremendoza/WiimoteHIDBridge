/*
Filename: Shared.h
Abstract: Shared constants, GUIDs, and IOCTL definitions for the KMDF Driver and Companion App.
*/

#pragma once

#ifndef CTL_CODE
#include <winioctl.h>
#endif

#include <initguid.h>

// Custom IOCTL to fetch the 100% accurate Wiimote state struct from the driver
#define IOCTL_WIIMOTE_GET_STATE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Custom IOCTL to force immediate Wiimote hardware report mode initialization (0x37)
#define IOCTL_WIIMOTE_INITIALIZE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Device Interface GUID so the user-mode app can find the driver
// {61674F5B-D7B6-488E-8B28-4E6EEE7E6E11}
DEFINE_GUID(GUID_DEVINTERFACE_WIIMOTE_BRIDGE,
    0x61674f5b, 0xd7b6, 0x488e, 0x8b, 0x28, 0x4e, 0x6e, 0xee, 0x7e, 0x6e, 0x11);
