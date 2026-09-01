/*
Filename: Device.h
Abstract: Master Device Context bridging Bluetooth, Wiimote State, and the AppBridge.
*/

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "Bluetooth.h"
#include "Wiimote.h"

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE Device;
    WDFIOTARGET IoTarget;

    BLUETOOTH_DEVICE_CONTEXT BluetoothContext;
    WIIMOTE_DEVICE_CONTEXT WiimoteContext;

} DEVICE_CONTEXT, * PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

NTSTATUS DeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit);
NTSTATUS PrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesRaw, _In_ WDFCMRESLIST ResourcesTranslated);
NTSTATUS ReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesTranslated);
NTSTATUS DeviceD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState);
NTSTATUS DeviceD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE TargetState);
NTSTATUS SignalDeviceIsGone(_In_ PDEVICE_CONTEXT DeviceContext);
