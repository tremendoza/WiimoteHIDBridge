/*
Filename: Device.cpp
Abstract: Contains system callbacks regarding a device's pnp and power states.
*/

#include "Device.h"
#include "Trace.h"
#include "AppBridge.h"
#include "Shared.h"
#include <initguid.h>

NTSTATUS DeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS Status = STATUS_SUCCESS;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
    WDFDEVICE Device;
    PDEVICE_CONTEXT DevContext;

    UNREFERENCED_PARAMETER(Driver);
    Trace("Device Added");

    WdfFdoInitSetFilter(DeviceInit);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDevicePrepareHardware = PrepareHardware;
    PnpPowerCallbacks.EvtDeviceReleaseHardware = ReleaseHardware;
    PnpPowerCallbacks.EvtDeviceD0Entry = DeviceD0Entry;
    PnpPowerCallbacks.EvtDeviceD0Exit = DeviceD0Exit;

    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPowerCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, DEVICE_CONTEXT);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) return Status;

    DevContext = GetDeviceContext(Device);
    DevContext->Device = Device;

    Status = AppBridgeCreateQueues(Device, DevContext);
    if (!NT_SUCCESS(Status))
    {
        TraceStatus("Error Creating App Bridge Queues", Status);
        return Status;
    }

    // Expose the Device Interface so the companion app can connect
    Status = WdfDeviceCreateDeviceInterface(Device, &GUID_DEVINTERFACE_WIIMOTE_BRIDGE, NULL);
    if (!NT_SUCCESS(Status))
    {
        TraceStatus("Error Creating Device Interface", Status);
    }

    TraceStatus("Device Added Result", Status);
    return Status;
}

NTSTATUS PrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesRaw, _In_ WDFCMRESLIST ResourcesTranslated)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PDEVICE_CONTEXT DeviceContext;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    Trace("PrepareHardware");

    DeviceContext = GetDeviceContext(Device);
    DeviceContext->IoTarget = WdfDeviceGetIoTarget(Device);

    Status = BluetoothPrepare(DeviceContext);
    if (!NT_SUCCESS(Status)) return Status;

    Status = WiimotePrepare(DeviceContext);

    TraceStatus("PrepareHardware Result", Status);
    return Status;
}

NTSTATUS DeviceD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PDEVICE_CONTEXT DeviceContext = GetDeviceContext(Device);
    UNREFERENCED_PARAMETER(PreviousState);

    Trace("Device D0 Entry - Starting active Bluetooth reconnection scanner");

    // Initiate the Bluetooth L2CAP reconnection state machine loop
    Status = BluetoothStartReconnectionLoop(DeviceContext);

    TraceStatus("Device D0 Entry Result", Status);
    return Status;
}

NTSTATUS DeviceD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PDEVICE_CONTEXT DeviceContext = GetDeviceContext(Device);
    UNREFERENCED_PARAMETER(TargetState);

    Trace("Exit D0");

    // Stop active scanning / reconnection timer
    BluetoothStopReconnectionLoop(DeviceContext);

    Status = WiimoteStop(DeviceContext);
    if (!NT_SUCCESS(Status)) TraceStatus("Error Stopping Wiimote", Status);

    Status = BluetoothCloseChannels(DeviceContext);
    if (!NT_SUCCESS(Status)) TraceStatus("Error Closing Bluetooth Connections", Status);

    TraceStatus("Exit D0 Result", Status);
    return Status;
}

NTSTATUS ReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    Trace("Release Hardware");
    return STATUS_SUCCESS;
}

NTSTATUS SignalDeviceIsGone(_In_ PDEVICE_CONTEXT DeviceContext)
{
    UNREFERENCED_PARAMETER(DeviceContext);
    // HID Minidriver is gone, so we acknowledge the disconnect gracefully without crashing or failing WDF
    Trace("SignalDeviceIsGone: Acknowledged remote disconnection gracefully");
    return STATUS_SUCCESS;
}
