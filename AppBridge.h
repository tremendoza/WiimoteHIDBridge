/*
Filename: AppBridge.h
Abstract: Defines the IO Queue bridge to serve Wiimote data to our user-mode app.
*/

#pragma once

#include <ntddk.h>
#include <wdf.h>

struct _DEVICE_CONTEXT;

NTSTATUS AppBridgeCreateQueues(_In_ WDFDEVICE Device, _In_ struct _DEVICE_CONTEXT* DeviceContext);
