/*
Filename: AppBridge.cpp
Abstract: Handles user-mode requests from the companion app, intercepts internal
          handshakes from the HID minidriver, and processes WriteFile requests
          at PASSIVE_LEVEL using WDF_OBJECT_ATTRIBUTES to prevent DPC deadlocks.
*/

#include "AppBridge.h"
#include "Device.h"
#include "Shared.h"
#include "Trace.h"
#include "hidminiport.h"

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL AppBridgeIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL AppBridgeIoInternalDeviceControl;
EVT_WDF_IO_QUEUE_IO_WRITE AppBridgeIoWrite;

NTSTATUS AppBridgeCreateQueues(_In_ WDFDEVICE Device, _In_ PDEVICE_CONTEXT DeviceContext)
{
    NTSTATUS Status;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDF_OBJECT_ATTRIBUTES QueueAttributes;
    WDFQUEUE Queue;
    UNREFERENCED_PARAMETER(DeviceContext);

    WDF_IO_QUEUE_CONFIG_INIT(&QueueConfig, WdfIoQueueDispatchParallel);

    QueueConfig.EvtIoDeviceControl = AppBridgeIoDeviceControl;
    QueueConfig.EvtIoInternalDeviceControl = AppBridgeIoInternalDeviceControl;
    QueueConfig.EvtIoWrite = AppBridgeIoWrite;

    // Initialize object attributes and enforce PASSIVE_LEVEL execution on the queue
    WDF_OBJECT_ATTRIBUTES_INIT(&QueueAttributes);
    QueueAttributes.ExecutionLevel = WdfExecutionLevelPassive;

    Status = WdfIoQueueCreate(Device, &QueueConfig, &QueueAttributes, &Queue);
    if (!NT_SUCCESS(Status))
    {
        TraceStatus("AppBridgeCreateQueues failed", Status);
        return Status;
    }

    WdfDeviceConfigureRequestDispatching(Device, Queue, WdfRequestTypeDeviceControl);
    WdfDeviceConfigureRequestDispatching(Device, Queue, WdfRequestTypeDeviceControlInternal);
    WdfDeviceConfigureRequestDispatching(Device, Queue, WdfRequestTypeWrite);

    return Status;
}

VOID AppBridgeIoInternalDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t OutputBufferLength, _In_ size_t InputBufferLength, _In_ ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (IoControlCode == IOCTL_WIIMOTE_ADDRESSES)
    {
        Trace("Intercepted IOCTL_WIIMOTE_ADDRESSES from HID Minidriver.");
        WdfRequestComplete(Request, STATUS_SUCCESS);
    }
    else
    {
        WDF_REQUEST_SEND_OPTIONS SendOptions;
        WDF_REQUEST_SEND_OPTIONS_INIT(&SendOptions, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);

        if (!WdfRequestSend(Request, WdfDeviceGetIoTarget(WdfIoQueueGetDevice(Queue)), &SendOptions))
        {
            WdfRequestComplete(Request, WdfRequestGetStatus(Request));
        }
    }
}

VOID AppBridgeIoWrite(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t Length)
{
    WDFDEVICE Device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT DeviceContext = GetDeviceContext(Device);
    PBLUETOOTH_DEVICE_CONTEXT BluetoothContext = &(DeviceContext->BluetoothContext);

    if (BluetoothContext->ConnectionState != WiimoteStateConnected)
    {
        BluetoothCheckAndConnect(DeviceContext);
    }

    if (BluetoothContext->ConnectionState != WiimoteStateConnected)
    {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_CONNECTED);
        return;
    }

    PVOID inputBuffer = NULL;
    size_t inputBufferLength = 0;
    NTSTATUS Status = WdfRequestRetrieveInputBuffer(Request, Length, &inputBuffer, &inputBufferLength);
    if (!NT_SUCCESS(Status))
    {
        WdfRequestComplete(Request, Status);
        return;
    }

    // Allocate memory for Payload + 1 byte for the mandatory 0xA2 HID Output Header
    WDFMEMORY Memory;
    Status = WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES, NonPagedPool, BLUETOOTH_POOL_TAG, inputBufferLength + 1, &Memory, NULL);
    if (!NT_SUCCESS(Status))
    {
        WdfRequestComplete(Request, Status);
        return;
    }

    PUCHAR buffer = (PUCHAR)WdfMemoryGetBuffer(Memory, NULL);
    buffer[0] = 0xA2; // Inject L2CAP HID Output Transaction Header
    RtlCopyMemory(buffer + 1, inputBuffer, inputBufferLength);

    // Safe execution at PASSIVE_LEVEL
    Status = BluetoothTransferToDevice(DeviceContext, NULL, Memory, TRUE);

    WdfObjectDelete(Memory);

    WdfRequestComplete(Request, Status);
}

VOID AppBridgeIoDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t OutputBufferLength, _In_ size_t InputBufferLength, _In_ ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(InputBufferLength);
    WDFDEVICE Device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT DeviceContext = GetDeviceContext(Device);
    PBLUETOOTH_DEVICE_CONTEXT BluetoothContext = &(DeviceContext->BluetoothContext);

    if (IoControlCode == IOCTL_WIIMOTE_GET_STATE)
    {
        if (OutputBufferLength < sizeof(InputReport37))
        {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
            return;
        }

        // If disconnected, query hardware Bluetooth MAC connection state.
        // If MAC is reconnected, open L2CAP channels immediately!
        if (BluetoothContext->ConnectionState != WiimoteStateConnected)
        {
            BluetoothCheckAndConnect(DeviceContext);
        }

        PVOID Buffer = NULL;
        NTSTATUS Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(InputReport37), &Buffer, NULL);

        if (NT_SUCCESS(Status) && Buffer != NULL)
        {
            if (BluetoothContext->ConnectionState == WiimoteStateConnected)
            {
                RtlCopyMemory(Buffer, &(DeviceContext->WiimoteContext.CurrentState), sizeof(InputReport37));
            }
            else
            {
                RtlZeroMemory(Buffer, sizeof(InputReport37));
            }
            WdfRequestCompleteWithInformation(Request, Status, sizeof(InputReport37));
        }
        else
        {
            WdfRequestComplete(Request, Status);
        }
    }
    else if (IoControlCode == IOCTL_WIIMOTE_INITIALIZE)
    {
        Trace("Received IOCTL_WIIMOTE_INITIALIZE from Companion App - Checking connection state & initializing...");
        BluetoothCheckAndConnect(DeviceContext);
        WiimoteStart(DeviceContext);
        WdfRequestComplete(Request, STATUS_SUCCESS);
    }
    else
    {
        WDF_REQUEST_SEND_OPTIONS SendOptions;
        WDF_REQUEST_SEND_OPTIONS_INIT(&SendOptions, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);

        if (!WdfRequestSend(Request, WdfDeviceGetIoTarget(Device), &SendOptions))
        {
            NTSTATUS Status = WdfRequestGetStatus(Request);
            WdfRequestComplete(Request, Status);
        }
    }
}
