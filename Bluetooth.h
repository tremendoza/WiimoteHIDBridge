/*
Filename: Bluetooth.h
Abstract: Declares Bluetooth structures, constants, and function prototypes.
          Manages connection state based on hardware MAC Bluetooth state without background timers.
*/

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <bthdef.h>
#include <bthguid.h>
#include <bthddi.h>
#include <bthioctl.h>

#ifndef BDIF_CONNECTED
#define BDIF_CONNECTED 0x00000010
#endif

#ifndef BTH_DEVICE_CONNECTED
#define BTH_DEVICE_CONNECTED BDIF_CONNECTED
#endif

#define BLUETOOTH_POOL_TAG 'BthW'
#define BLUETOOTH_ADDRESS_STRING_SIZE 13
#define SYNCHRONOUS_CALL_TIMEOUT WDF_REL_TIMEOUT_IN_SEC(5)

typedef enum _WIIMOTE_CONN_STATE {
    WiimoteStateDisconnected = 0,
    WiimoteStateConnecting,
    WiimoteStateConnected
} WIIMOTE_CONN_STATE;

typedef struct _BLUETOOTH_DEVICE_CONTEXT {
    BTH_PROFILE_DRIVER_INTERFACE ProfileDriverInterface;
    BTH_ADDR DeviceAddress;
    WCHAR DeviceAddressStringBuffer[BLUETOOTH_ADDRESS_STRING_SIZE];
    UNICODE_STRING DeviceAddressString;
    WCHAR DeviceNameStringBuffer[BTH_MAX_NAME_SIZE];
    UNICODE_STRING DeviceNameString;
    L2CAP_CHANNEL_HANDLE ControlChannelHandle;
    L2CAP_CHANNEL_HANDLE InterruptChannelHandle;
    size_t ReadBufferSize;

    // --- State Management ---
    WIIMOTE_CONN_STATE ConnectionState;
    LARGE_INTEGER LastConnectAttemptTime;
} BLUETOOTH_DEVICE_CONTEXT, * PBLUETOOTH_DEVICE_CONTEXT;

struct _DEVICE_CONTEXT;

NTSTATUS GetVendorAndProductID(_In_ WDFIOTARGET IoTarget, _Out_ USHORT* VendorID, _Out_ USHORT* ProductID);
NTSTATUS BluetoothPrepare(_In_ struct _DEVICE_CONTEXT* DeviceContext);
BOOLEAN BluetoothIsDeviceConnected(_In_ struct _DEVICE_CONTEXT* DeviceContext);
NTSTATUS BluetoothCheckAndConnect(_In_ struct _DEVICE_CONTEXT* DeviceContext);
NTSTATUS BluetoothStartReconnectionLoop(_In_ struct _DEVICE_CONTEXT* DeviceContext);
VOID BluetoothStopReconnectionLoop(_In_ struct _DEVICE_CONTEXT* DeviceContext);
NTSTATUS BluetoothOpenChannels(_In_ struct _DEVICE_CONTEXT* DeviceContext);
NTSTATUS BluetoothCloseChannels(_In_ struct _DEVICE_CONTEXT* DeviceContext);
NTSTATUS BluetoothTransferToDevice(_In_ struct _DEVICE_CONTEXT* DeviceContext, _In_ WDFREQUEST Request, _In_ WDFMEMORY Memory, _In_ BOOLEAN Synchronous);
NTSTATUS BluetoothStartContiniousReader(_In_ struct _DEVICE_CONTEXT* DeviceContext);

