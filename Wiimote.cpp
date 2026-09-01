/*
Filename: Wiimote.cpp
Abstract: Handles Wiimote initialization. Uses a WDF Work Item to safely perform
          hardware stabilization delays and strictly enforces exact payload
          alignment matching the user-space companion app specification.
*/

#include "Wiimote.h"
#include "Device.h"
#include "Bluetooth.h"
#include "Trace.h"

EVT_WDF_WORKITEM WiimoteInitWorkItemCallback;

NTSTATUS SendBluetoothDataSynchronous(_In_ PDEVICE_CONTEXT DeviceContext, _In_ PUCHAR Data, _In_ SIZE_T Size)
{
    WDFMEMORY Memory;

    // Allocate Size + 1 to make room for the mandatory L2CAP HID Output Header (0xA2)
    NTSTATUS Status = WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES, NonPagedPool, BLUETOOTH_POOL_TAG, Size + 1, &Memory, NULL);
    if (!NT_SUCCESS(Status)) return Status;

    PUCHAR buffer = (PUCHAR)WdfMemoryGetBuffer(Memory, NULL);

    // Inject the HID Output Transaction Header
    buffer[0] = 0xA2;

    // Copy the actual Wiimote payload right after the header
    RtlCopyMemory(buffer + 1, Data, Size);

    Status = BluetoothTransferToDevice(DeviceContext, NULL, Memory, TRUE);

    WdfObjectDelete(Memory);
    return Status;
}

NTSTATUS WiimotePrepare(_In_ PDEVICE_CONTEXT DeviceContext)
{
    WiimoteReset(DeviceContext);

    WDF_WORKITEM_CONFIG workItemConfig;
    WDF_WORKITEM_CONFIG_INIT(&workItemConfig, WiimoteInitWorkItemCallback);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = DeviceContext->Device;

    NTSTATUS Status = WdfWorkItemCreate(&workItemConfig, &attributes, &DeviceContext->WiimoteContext.InitWorkItem);
    if (!NT_SUCCESS(Status))
    {
        TraceStatus("Failed to create Wiimote Init WorkItem", Status);
    }

    return Status;
}

NTSTATUS WiimoteStart(_In_ PDEVICE_CONTEXT DeviceContext)
{
    if (DeviceContext->WiimoteContext.InitWorkItem != NULL)
    {
        WdfWorkItemEnqueue(DeviceContext->WiimoteContext.InitWorkItem);
    }
    return STATUS_SUCCESS;
}

VOID WiimoteInitWorkItemCallback(_In_ WDFWORKITEM WorkItem)
{
    PDEVICE_CONTEXT DeviceContext = GetDeviceContext(WdfWorkItemGetParentObject(WorkItem));

    LARGE_INTEGER delay;

    // 1. Immediately enable continuous data reporting (Report Mode 0x37 with continuous flag 0x04)
    Trace("Enabling continuous reporting (Report Mode 0x37)...");
    uint8_t out_buf[3] = { 0x12, 0x04, 0x37 };
    SendBluetoothDataSynchronous(DeviceContext, out_buf, sizeof(out_buf));

    delay.QuadPart = -1000000LL; // 100ms
    KeDelayExecutionThread(KernelMode, FALSE, &delay);

    // 2. Request status report (Report Mode 0x15) to obtain battery level
    Trace("Requesting status report (Report 0x15 for battery status)...");
    uint8_t status_req[2] = { 0x15, 0x00 };
    SendBluetoothDataSynchronous(DeviceContext, status_req, sizeof(status_req));

    delay.QuadPart = -1000000LL; // 100ms
    KeDelayExecutionThread(KernelMode, FALSE, &delay);

    // 3. Initialize Motion Plus (Write 0x55 to register 0x04A600F0)
    Trace("Initializing Motion Plus (Write 0x55 to 0x04A600F0)...");
    uint8_t mplus_init[7] = { 0x16, 0x04, 0xA6, 0x00, 0xF0, 0x01, 0x55 };
    SendBluetoothDataSynchronous(DeviceContext, mplus_init, sizeof(mplus_init));

    delay.QuadPart = -1000000LL; // 100ms
    KeDelayExecutionThread(KernelMode, FALSE, &delay);

    // 4. Enable Motion Plus in Nunchuk Passthrough Mode (Write 0x05 to register 0x04A600FE)
    Trace("Enabling Motion Plus in Nunchuk Passthrough Mode (Write 0x05 to 0x04A600FE)...");
    uint8_t mplus_enable[7] = { 0x16, 0x04, 0xA6, 0x00, 0xFE, 0x01, 0x05 };
    SendBluetoothDataSynchronous(DeviceContext, mplus_enable, sizeof(mplus_enable));

    delay.QuadPart = -2000000LL; // 200ms
    KeDelayExecutionThread(KernelMode, FALSE, &delay);

    // 5. Re-confirm continuous reporting mode 0x37
    SendBluetoothDataSynchronous(DeviceContext, out_buf, sizeof(out_buf));
    Trace("Wiimote hardware initialization complete.");
}

NTSTATUS WiimoteStop(_In_ PDEVICE_CONTEXT DeviceContext)
{
    if (DeviceContext->WiimoteContext.InitWorkItem != NULL)
    {
        WdfWorkItemFlush(DeviceContext->WiimoteContext.InitWorkItem);
    }

    WiimoteReset(DeviceContext);

    return STATUS_SUCCESS;
}

VOID WiimoteReset(_In_ PDEVICE_CONTEXT DeviceContext)
{
    DeviceContext->WiimoteContext.IsRumbling = FALSE;
    RtlSecureZeroMemory(&(DeviceContext->WiimoteContext.CurrentState), sizeof(InputReport37));
}

NTSTATUS WiimoteProcessReport(_In_ PDEVICE_CONTEXT DeviceContext, _In_ PVOID Buffer, _In_ SIZE_T BufferSize)
{
    uint8_t* rawBuffer = static_cast<uint8_t*>(Buffer);

    // L2CAP HID reports prepend a transaction header (0xA1 for Input Report). 
    // We strip it to align with the InputReport37 struct layout.
    if (BufferSize > 0 && rawBuffer[0] == 0xA1)
    {
        rawBuffer++;
        BufferSize--;
    }

    if (BufferSize >= 7 && rawBuffer[0] == 0x20)
    {
        // Status Information Report (0x20): Byte 6 contains Battery Level (0x00 - 0xFF)
        DeviceContext->WiimoteContext.CurrentState.battery = rawBuffer[6];
    }
    else if (BufferSize >= 22 && rawBuffer[0] == 0x37)
    {
        // Copy Report 0x37 raw payload (22 bytes) without overwriting the battery field
        RtlCopyMemory(&(DeviceContext->WiimoteContext.CurrentState), rawBuffer, 22);
    }

    return STATUS_SUCCESS;
}