/*
Filename: Bluetooth.cpp
Abstract: Manages Bluetooth L2CAP connection handling, asynchronous packet reads,
          and robust active scanning/reconnection logic for paired Wiimotes.
*/

#include <initguid.h>
#include "Bluetooth.h"
#include "Device.h"
#include "Trace.h"

EVT_WDF_REQUEST_COMPLETION_ROUTINE ControlChannelCompletion;
EVT_WDF_REQUEST_COMPLETION_ROUTINE InterruptChannelCompletion;
VOID L2CAPCallback(_In_ PVOID Context, _In_ INDICATION_CODE Indication, _In_ PINDICATION_PARAMETERS Parameters);
EVT_WDF_REQUEST_COMPLETION_ROUTINE TransferToDeviceCompletion;
EVT_WDF_REQUEST_COMPLETION_ROUTINE ReadFromDeviceCompletion;

NTSTATUS GetVendorAndProductID(_In_ WDFIOTARGET IoTarget, _Out_ USHORT* VendorID, _Out_ USHORT* ProductID)
{
    NTSTATUS Status = STATUS_SUCCESS;
    WDF_MEMORY_DESCRIPTOR EnumInfoMemDescriptor;
    BTH_ENUMERATOR_INFO EnumInfo;

    RtlZeroMemory(&EnumInfo, sizeof(EnumInfo));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&EnumInfoMemDescriptor, &EnumInfo, sizeof(EnumInfo));

    Status = WdfIoTargetSendInternalIoctlSynchronously(
        IoTarget, NULL, IOCTL_INTERNAL_BTHENUM_GET_ENUMINFO, NULL, &EnumInfoMemDescriptor, NULL, NULL);

    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    (*ProductID) = EnumInfo.Pid;
    (*VendorID) = EnumInfo.Vid;

    return Status;
}

NTSTATUS BluetoothPrepare(_In_ PDEVICE_CONTEXT DeviceContext)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PBLUETOOTH_DEVICE_CONTEXT BluetoothContext = &(DeviceContext->BluetoothContext);
    WDF_MEMORY_DESCRIPTOR DeviceInfoMemDescriptor;
    BTH_DEVICE_INFO DeviceInfo;

    BluetoothContext->ControlChannelHandle = NULL;
    BluetoothContext->InterruptChannelHandle = NULL;
    BluetoothContext->ConnectionState = WiimoteStateDisconnected;
    BluetoothContext->LastConnectAttemptTime.QuadPart = 0;

    Status = WdfFdoQueryForInterface(
        DeviceContext->Device, &GUID_BTHDDI_PROFILE_DRIVER_INTERFACE,
        (PINTERFACE)(&(BluetoothContext->ProfileDriverInterface)),
        sizeof(BluetoothContext->ProfileDriverInterface),
        BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI, NULL);

    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    RtlZeroMemory(&DeviceInfo, sizeof(DeviceInfo));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&DeviceInfoMemDescriptor, &DeviceInfo, sizeof(DeviceInfo));

    Status = WdfIoTargetSendInternalIoctlSynchronously(
        DeviceContext->IoTarget, NULL, IOCTL_INTERNAL_BTHENUM_GET_DEVINFO, NULL, &DeviceInfoMemDescriptor, NULL, NULL);

    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    BluetoothContext->DeviceAddress = DeviceInfo.address;
    Status = RtlStringCchPrintfW(BluetoothContext->DeviceAddressStringBuffer, BLUETOOTH_ADDRESS_STRING_SIZE, L"%012I64x", DeviceInfo.address);
    if (!NT_SUCCESS(Status)) return Status;
    Status = RtlUnicodeStringInit(&BluetoothContext->DeviceAddressString, BluetoothContext->DeviceAddressStringBuffer);
    if (!NT_SUCCESS(Status)) return Status;

    size_t NameLength;
    Status = RtlStringCbLengthA(DeviceInfo.name, BTH_MAX_NAME_SIZE, &NameLength);
    if (!NT_SUCCESS(Status)) return Status;

    ULONG bytesConverted = 0;
    Status = RtlUTF8ToUnicodeN(BluetoothContext->DeviceNameStringBuffer, BTH_MAX_NAME_SIZE * sizeof(WCHAR), &bytesConverted, DeviceInfo.name, (ULONG)NameLength);
    if (!NT_SUCCESS(Status)) return Status;

    Status = RtlUnicodeStringInit(&BluetoothContext->DeviceNameString, BluetoothContext->DeviceNameStringBuffer);
    if (!NT_SUCCESS(Status)) return Status;

    return STATUS_SUCCESS;
}

BOOLEAN BluetoothIsDeviceConnected(_In_ PDEVICE_CONTEXT DeviceContext)
{
    BTH_DEVICE_INFO DeviceInfo;
    WDF_MEMORY_DESCRIPTOR DeviceInfoMemDescriptor;

    RtlZeroMemory(&DeviceInfo, sizeof(DeviceInfo));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&DeviceInfoMemDescriptor, &DeviceInfo, sizeof(DeviceInfo));

    NTSTATUS Status = WdfIoTargetSendInternalIoctlSynchronously(
        DeviceContext->IoTarget, NULL, IOCTL_INTERNAL_BTHENUM_GET_DEVINFO, NULL, &DeviceInfoMemDescriptor, NULL, NULL);

    if (NT_SUCCESS(Status))
    {
        // Query Bluetooth hardware connection state for specific target MAC address
        if ((DeviceInfo.flags & BTH_DEVICE_CONNECTED) || (DeviceInfo.flags & BDIF_CONNECTED))
        {
            Trace("Bluetooth Hardware Status: MAC %012I64x IS CONNECTED (flags=0x%x)", DeviceInfo.address, DeviceInfo.flags);
            return TRUE;
        }
        else
        {
            Trace("Bluetooth Hardware Status: MAC %012I64x is DISCONNECTED (flags=0x%x)", DeviceInfo.address, DeviceInfo.flags);
        }
    }
    else
    {
        TraceStatus("IOCTL_INTERNAL_BTHENUM_GET_DEVINFO query failed", Status);
    }

    return FALSE;
}

NTSTATUS BluetoothCheckAndConnect(_In_ PDEVICE_CONTEXT DeviceContext)
{
    PBLUETOOTH_DEVICE_CONTEXT BluetoothContext = &(DeviceContext->BluetoothContext);

    // 1. If already connected or actively opening channels, return immediately!
    if (BluetoothContext->ConnectionState == WiimoteStateConnected ||
        BluetoothContext->ConnectionState == WiimoteStateConnecting)
    {
        return STATUS_SUCCESS;
    }

    // 2. Throttle connection checks to at most once every 2 seconds (20,000,000 100-ns units)
    LARGE_INTEGER CurrentTime;
    KeQuerySystemTime(&CurrentTime);

    if ((CurrentTime.QuadPart - BluetoothContext->LastConnectAttemptTime.QuadPart) < 20000000LL)
    {
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    BluetoothContext->LastConnectAttemptTime = CurrentTime;

    // 3. Query Bluetooth hardware connection state for target MAC address
    if (BluetoothIsDeviceConnected(DeviceContext))
    {
        Trace("Bluetooth state IS CONNECTED. Opening L2CAP channels for Wiimote %wZ...", &BluetoothContext->DeviceAddressString);
        BluetoothContext->ConnectionState = WiimoteStateConnecting;

        NTSTATUS Status = BluetoothOpenChannels(DeviceContext);
        if (!NT_SUCCESS(Status))
        {
            BluetoothContext->ConnectionState = WiimoteStateDisconnected;
            WiimoteReset(DeviceContext);
        }
        return Status;
    }
    else
    {
        Trace("Bluetooth state is NOT connected. Clearing queues and resting...");
        BluetoothContext->ConnectionState = WiimoteStateDisconnected;
        WiimoteReset(DeviceContext);
        return STATUS_DEVICE_NOT_CONNECTED;
    }
}

NTSTATUS BluetoothStartReconnectionLoop(_In_ PDEVICE_CONTEXT DeviceContext)
{
    // State/event-driven connection trigger - NO TIMER
    return BluetoothCheckAndConnect(DeviceContext);
}

VOID BluetoothStopReconnectionLoop(_In_ PDEVICE_CONTEXT DeviceContext)
{
    PBLUETOOTH_DEVICE_CONTEXT BluetoothContext = &(DeviceContext->BluetoothContext);
    BluetoothCloseChannels(DeviceContext);
    BluetoothContext->ConnectionState = WiimoteStateDisconnected;
    WiimoteReset(DeviceContext);
}

NTSTATUS CreateRequest(_In_ WDFDEVICE Device, _In_ WDFIOTARGET IoTarget, _Outptr_ WDFREQUEST* Request)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
    return WdfRequestCreate(&Attributes, IoTarget, Request);
}

NTSTATUS CreateBuffer(_In_ WDFREQUEST Request, _In_ SIZE_T BufferSize, _Outptr_ WDFMEMORY* Memory, _Outptr_opt_result_buffer_(BufferSize) PVOID* Buffer)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Request;
    return WdfMemoryCreate(&Attributes, NonPagedPool, BLUETOOTH_POOL_TAG, BufferSize, Memory, Buffer);
}

NTSTATUS BluetoothCreateRequestAndBuffer(_In_ WDFDEVICE Device, _In_ WDFIOTARGET IoTarget, _In_ SIZE_T BufferSize, _Outptr_ WDFREQUEST* Request, _Outptr_ WDFMEMORY* Memory, _Outptr_opt_result_buffer_(BufferSize) PVOID* Buffer)
{
    NTSTATUS Status = CreateRequest(Device, IoTarget, Request);
    if (!NT_SUCCESS(Status)) return Status;

    Status = CreateBuffer((*Request), BufferSize, Memory, Buffer);
    if (!NT_SUCCESS(Status))
    {
        WdfObjectDelete(*Request);
        (*Request) = NULL;
    }
    return Status;
}

NTSTATUS PrepareRequest(_In_ WDFIOTARGET IoTarget, _In_ struct _BRB* BRB, _In_ WDFREQUEST Request)
{
    WDF_OBJECT_ATTRIBUTES MemoryAttributes;
    WDFMEMORY Memory = NULL;

    WDF_OBJECT_ATTRIBUTES_INIT(&MemoryAttributes);
    MemoryAttributes.ParentObject = Request;

    NTSTATUS Status = WdfMemoryCreatePreallocated(&MemoryAttributes, BRB, sizeof(*BRB), &Memory);
    if (!NT_SUCCESS(Status)) return Status;

    return WdfIoTargetFormatRequestForInternalIoctlOthers(IoTarget, Request, IOCTL_INTERNAL_BTH_SUBMIT_BRB, Memory, NULL, NULL, NULL, NULL, NULL);
}

NTSTATUS SendBRB(_In_ PDEVICE_CONTEXT DeviceContext, _In_opt_ WDFREQUEST OptRequest, _In_ struct _BRB* BRB, _In_ PFN_WDF_REQUEST_COMPLETION_ROUTINE CompletionRoutine)
{
    NTSTATUS Status = STATUS_SUCCESS;
    WDFREQUEST Request;

    if (OptRequest == NULL)
    {
        Status = CreateRequest(DeviceContext->Device, DeviceContext->IoTarget, &Request);
        if (!NT_SUCCESS(Status)) return Status;
    }
    else
    {
        Request = OptRequest;
    }

    Status = PrepareRequest(DeviceContext->IoTarget, BRB, Request);
    if (!NT_SUCCESS(Status))
    {
        WdfObjectDelete(Request);
        return Status;
    }

    WdfRequestSetCompletionRoutine(Request, CompletionRoutine, BRB);

    if (!WdfRequestSend(Request, DeviceContext->IoTarget, WDF_NO_SEND_OPTIONS))
    {
        Status = WdfRequestGetStatus(Request);
        WdfObjectDelete(Request);
    }
    return Status;
}

NTSTATUS SendBRBSynchronous(_In_ PDEVICE_CONTEXT DeviceContext, _In_opt_ WDFREQUEST OptRequest, _In_ struct _BRB* BRB)
{
    NTSTATUS Status = STATUS_SUCCESS;
    WDF_REQUEST_SEND_OPTIONS SendOptions;
    WDFREQUEST Request;

    if (OptRequest == NULL)
    {
        Status = CreateRequest(DeviceContext->Device, DeviceContext->IoTarget, &Request);
        if (!NT_SUCCESS(Status)) return Status;
    }
    else
    {
        Request = OptRequest;
    }

    Status = PrepareRequest(DeviceContext->IoTarget, BRB, Request);
    if (!NT_SUCCESS(Status))
    {
        WdfObjectDelete(Request);
        return Status;
    }

    Status = WdfRequestAllocateTimer(Request);
    if (!NT_SUCCESS(Status))
    {
        WdfObjectDelete(Request);
        return Status;
    }

    WDF_REQUEST_SEND_OPTIONS_INIT(&SendOptions, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS | WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&SendOptions, SYNCHRONOUS_CALL_TIMEOUT);

    WdfRequestSend(Request, DeviceContext->IoTarget, &SendOptions);
    Status = WdfRequestGetStatus(Request);

    if (!NT_SUCCESS(Status))
    {
        WdfObjectDelete(Request);
    }
    return Status;
}

VOID CleanUpCompletedRequest(_In_ WDFREQUEST Request, _In_ WDFIOTARGET IoTarget, _In_ WDFCONTEXT Context)
{
    PDEVICE_CONTEXT DeviceContext = GetDeviceContext(WdfIoTargetGetDevice(IoTarget));
    PBLUETOOTH_DEVICE_CONTEXT BluetoothContext = &(DeviceContext->BluetoothContext);
    struct _BRB* UsedBRB = (struct _BRB*)Context;

    WdfObjectDelete(Request);
    BluetoothContext->ProfileDriverInterface.BthFreeBrb(UsedBRB);
}

VOID L2CAPCallback(_In_ PVOID Context, _In_ INDICATION_CODE Indication, _In_ PINDICATION_PARAMETERS Parameters)
{
    PDEVICE_CONTEXT DeviceContext = (PDEVICE_CONTEXT)Context;
    PBLUETOOTH_DEVICE_CONTEXT BluetoothContext = &(DeviceContext->BluetoothContext);
    UNREFERENCED_PARAMETER(Parameters);

    Trace("L2CAP Channel Callback - Indication: 0x%x", Indication);

    if (Indication == IndicationRemoteDisconnect)
    {
        Trace("Wiimote Remote Disconnect detected! Resetting state & scheduling reconnection scan...");
        
        // CRITICAL BSOD FIX (Bluetooth Verifier):
        // 1. Do NOT call BRB_L2CA_CLOSE_CHANNEL after remote disconnect indication!
        //    The Bluetooth stack (BthPort) has already unmapped/closed the L2CAP channel.
        // 2. L2CAPCallback runs at DISPATCH_LEVEL; never invoke synchronous BRB calls here.
        BluetoothContext->InterruptChannelHandle = NULL;
        BluetoothContext->ControlChannelHandle = NULL;
        BluetoothContext->ConnectionState = WiimoteStateDisconnected;

        WiimoteReset(DeviceContext);
        SignalDeviceIsGone(DeviceContext);
    }
}

NTSTATUS OpenChannel(_In_ PDEVICE_CONTEXT DeviceContext, _In_opt_ struct _BRB* PreAllocatedBRB, _In_ BYTE PSM, _In_opt_ PFNBTHPORT_INDICATION_CALLBACK ChannelCallback, _In_ PFN_WDF_REQUEST_COMPLETION_ROUTINE ChannelCompletion)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PBLUETOOTH_DEVICE_CONTEXT BluetoothContext = &(DeviceContext->BluetoothContext);
    struct _BRB_L2CA_OPEN_CHANNEL* BRBOpenChannel;

    if (PreAllocatedBRB == NULL)
    {
        BRBOpenChannel = (struct _BRB_L2CA_OPEN_CHANNEL*)BluetoothContext->ProfileDriverInterface.BthAllocateBrb(BRB_L2CA_OPEN_CHANNEL, BLUETOOTH_POOL_TAG);
        if (BRBOpenChannel == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    }
    else
    {
        BluetoothContext->ProfileDriverInterface.BthReuseBrb(PreAllocatedBRB, BRB_L2CA_OPEN_CHANNEL);
        BRBOpenChannel = (struct _BRB_L2CA_OPEN_CHANNEL*)PreAllocatedBRB;
    }

    BRBOpenChannel->BtAddress = BluetoothContext->DeviceAddress;
    BRBOpenChannel->Psm = PSM;
    BRBOpenChannel->ChannelFlags = 0;
    BRBOpenChannel->ConfigOut.Flags = 0;
    BRBOpenChannel->ConfigOut.Mtu.Max = L2CAP_DEFAULT_MTU;
    BRBOpenChannel->ConfigOut.Mtu.Min = L2CAP_MIN_MTU;
    BRBOpenChannel->ConfigOut.Mtu.Preferred = L2CAP_DEFAULT_MTU;
    BRBOpenChannel->ConfigOut.FlushTO.Max = L2CAP_DEFAULT_FLUSHTO;
    BRBOpenChannel->ConfigOut.FlushTO.Min = L2CAP_MIN_FLUSHTO;
    BRBOpenChannel->ConfigOut.FlushTO.Preferred = L2CAP_DEFAULT_FLUSHTO;
    BRBOpenChannel->ConfigOut.ExtraOptions = 0;
    BRBOpenChannel->ConfigOut.NumExtraOptions = 0;
    BRBOpenChannel->ConfigOut.LinkTO = 1; // 1s Link Timeout for fast reconnection failure recovery
    BRBOpenChannel->IncomingQueueDepth = 50;
    BRBOpenChannel->ReferenceObject = (PVOID)WdfDeviceWdmGetDeviceObject(DeviceContext->Device);

    if (ChannelCallback != NULL)
    {
        BRBOpenChannel->CallbackFlags = CALLBACK_DISCONNECT;
        BRBOpenChannel->Callback = ChannelCallback;
        BRBOpenChannel->CallbackContext = (PVOID)DeviceContext;
    }

    Status = SendBRB(DeviceContext, NULL, (struct _BRB*)BRBOpenChannel, ChannelCompletion);
    if (!NT_SUCCESS(Status))
    {
        BluetoothContext->ProfileDriverInterface.BthFreeBrb((struct _BRB*)BRBOpenChannel);
    }
    return Status;
}

VOID ControlChannelCompletion(_In_ WDFREQUEST Request, _In_ WDFIOTARGET IoTarget, _In_ PWDF_REQUEST_COMPLETION_PARAMS Params, _In_ WDFCONTEXT Context)
{
    NTSTATUS Status = Params->IoStatus.Status;
    PDEVICE_CONTEXT DeviceContext = GetDeviceContext(WdfIoTargetGetDevice(IoTarget));
    PBLUETOOTH_DEVICE_CONTEXT BluetoothContext = &(DeviceContext->BluetoothContext);
    struct _BRB_L2CA_OPEN_CHANNEL* UsedBRBOpenChannel = (struct _BRB_L2CA_OPEN_CHANNEL*)Context;

    TraceStatus("Control Channel Open Result", Status);

    if (!NT_SUCCESS(Status))
    {
        CleanUpCompletedRequest(Request, IoTarget, Context);
        
        BluetoothCloseChannels(DeviceContext);
        BluetoothContext->ConnectionState = WiimoteStateDisconnected;
        WiimoteReset(DeviceContext);
        return;
    }

    BluetoothContext->ControlChannelHandle = UsedBRBOpenChannel->ChannelHandle;
    CleanUpCompletedRequest(Request, IoTarget, Context);

    // Open Interrupt Channel (PSM 0x13)
    OpenChannel(DeviceContext, NULL, 0x13, L2CAPCallback, InterruptChannelCompletion);
}

VOID InterruptChannelCompletion(_In_ WDFREQUEST Request, _In_ WDFIOTARGET IoTarget, _In_ PWDF_REQUEST_COMPLETION_PARAMS Params, _In_ WDFCONTEXT Context)
{
    NTSTATUS Status = Params->IoStatus.Status;
    PDEVICE_CONTEXT DeviceContext = GetDeviceContext(WdfIoTargetGetDevice(IoTarget));
    PBLUETOOTH_DEVICE_CONTEXT BluetoothContext = &(DeviceContext->BluetoothContext);
    struct _BRB_L2CA_OPEN_CHANNEL* UsedBRBOpenChannel = (struct _BRB_L2CA_OPEN_CHANNEL*)Context;

    TraceStatus("Interrupt Channel Open Result", Status);

    if (!NT_SUCCESS(Status))
    {
        CleanUpCompletedRequest(Request, IoTarget, Context);

        BluetoothCloseChannels(DeviceContext);
        BluetoothContext->ConnectionState = WiimoteStateDisconnected;
        WiimoteReset(DeviceContext);
        return;
    }

    BluetoothContext->InterruptChannelHandle = UsedBRBOpenChannel->ChannelHandle;
    CleanUpCompletedRequest(Request, IoTarget, Context);

    // --- SUCCESSFULLY CONNECTED! ---
    BluetoothContext->ConnectionState = WiimoteStateConnected;

    Trace("L2CAP Channels successfully established! Starting reader & Wiimote initialization...");

    // Start continuous L2CAP packet reader immediately so no incoming data is missed
    BluetoothStartContiniousReader(DeviceContext);

    // Start Wiimote hardware report mode initialization
    WiimoteStart(DeviceContext);
}

NTSTATUS BluetoothOpenChannels(_In_ PDEVICE_CONTEXT DeviceContext)
{
    return OpenChannel(DeviceContext, NULL, 0x11, NULL, ControlChannelCompletion);
}

NTSTATUS CloseChannel(_In_ PDEVICE_CONTEXT DeviceContext, _In_ L2CAP_CHANNEL_HANDLE ChannelHandle)
{
    PBLUETOOTH_DEVICE_CONTEXT BluetoothContext = &(DeviceContext->BluetoothContext);
    if (ChannelHandle == NULL) return STATUS_SUCCESS;

    // Safety Check: Bluetooth Verifier BSOD Prevention
    // Never invoke SendBRBSynchronous at DISPATCH_LEVEL
    if (KeGetCurrentIrql() > PASSIVE_LEVEL)
    {
        Trace("CloseChannel called at DISPATCH_LEVEL. Skipping synchronous BRB.");
        return STATUS_SUCCESS;
    }

    struct _BRB_L2CA_CLOSE_CHANNEL* BRBCloseChannel = (struct _BRB_L2CA_CLOSE_CHANNEL*)BluetoothContext->ProfileDriverInterface.BthAllocateBrb(BRB_L2CA_CLOSE_CHANNEL, BLUETOOTH_POOL_TAG);
    if (BRBCloseChannel == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    BRBCloseChannel->BtAddress = BluetoothContext->DeviceAddress;
    BRBCloseChannel->ChannelHandle = ChannelHandle;

    NTSTATUS Status = SendBRBSynchronous(DeviceContext, NULL, (struct _BRB*)BRBCloseChannel);
    BluetoothContext->ProfileDriverInterface.BthFreeBrb((struct _BRB*)BRBCloseChannel);
    return Status;
}

NTSTATUS BluetoothCloseChannels(_In_ PDEVICE_CONTEXT DeviceContext)
{
    PBLUETOOTH_DEVICE_CONTEXT BluetoothContext = &(DeviceContext->BluetoothContext);

    if (BluetoothContext->InterruptChannelHandle != NULL) {
        CloseChannel(DeviceContext, BluetoothContext->InterruptChannelHandle);
        BluetoothContext->InterruptChannelHandle = NULL;
    }

    if (BluetoothContext->ControlChannelHandle != NULL) {
        CloseChannel(DeviceContext, BluetoothContext->ControlChannelHandle);
        BluetoothContext->ControlChannelHandle = NULL;
    }

    return STATUS_SUCCESS;
}

NTSTATUS BluetoothTransferToDevice(_In_ PDEVICE_CONTEXT DeviceContext, _In_ WDFREQUEST Request, _In_ WDFMEMORY Memory, _In_ BOOLEAN Synchronous)
{
    PBLUETOOTH_DEVICE_CONTEXT BluetoothContext = &(DeviceContext->BluetoothContext);
    size_t BufferSize;

    L2CAP_CHANNEL_HANDLE targetChannel = NULL;
    if (BluetoothContext->ControlChannelHandle != NULL)
    {
        targetChannel = BluetoothContext->ControlChannelHandle;
    }
    else if (BluetoothContext->InterruptChannelHandle != NULL)
    {
        targetChannel = BluetoothContext->InterruptChannelHandle;
    }
    else
    {
        return STATUS_INVALID_HANDLE;
    }

    struct _BRB_L2CA_ACL_TRANSFER* BRBTransfer = (struct _BRB_L2CA_ACL_TRANSFER*)BluetoothContext->ProfileDriverInterface.BthAllocateBrb(BRB_L2CA_ACL_TRANSFER, BLUETOOTH_POOL_TAG);
    if (BRBTransfer == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    BRBTransfer->BtAddress = BluetoothContext->DeviceAddress;
    BRBTransfer->ChannelHandle = targetChannel;
    BRBTransfer->TransferFlags = ACL_TRANSFER_DIRECTION_OUT;
    BRBTransfer->BufferMDL = NULL;
    BRBTransfer->Buffer = WdfMemoryGetBuffer(Memory, &BufferSize);
    BRBTransfer->BufferSize = (ULONG)BufferSize;

    NTSTATUS Status;
    if (Synchronous)
    {
        Status = SendBRBSynchronous(DeviceContext, Request, (struct _BRB*)BRBTransfer);
        BluetoothContext->ProfileDriverInterface.BthFreeBrb((struct _BRB*)BRBTransfer);
    }
    else
    {
        Status = SendBRB(DeviceContext, Request, (struct _BRB*)BRBTransfer, TransferToDeviceCompletion);
        if (!NT_SUCCESS(Status)) BluetoothContext->ProfileDriverInterface.BthFreeBrb((struct _BRB*)BRBTransfer);
    }
    return Status;
}

VOID TransferToDeviceCompletion(_In_ WDFREQUEST Request, _In_ WDFIOTARGET IoTarget, _In_ PWDF_REQUEST_COMPLETION_PARAMS Params, _In_ WDFCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Params);
    CleanUpCompletedRequest(Request, IoTarget, Context);
}

NTSTATUS ReadFromDevice(_In_ PDEVICE_CONTEXT DeviceContext, _In_ WDFREQUEST Request, _In_ struct _BRB_L2CA_ACL_TRANSFER* BRB, _In_reads_(ReadBufferSize) PVOID ReadBuffer, _In_ SIZE_T ReadBufferSize)
{
    PBLUETOOTH_DEVICE_CONTEXT BluetoothContext = &(DeviceContext->BluetoothContext);

    if (BluetoothContext->InterruptChannelHandle == NULL) return STATUS_INVALID_HANDLE;

    BRB->BtAddress = BluetoothContext->DeviceAddress;
    BRB->ChannelHandle = BluetoothContext->InterruptChannelHandle;
    BRB->TransferFlags = ACL_TRANSFER_DIRECTION_IN | ACL_SHORT_TRANSFER_OK;
    BRB->BufferMDL = NULL;
    BRB->Buffer = ReadBuffer;
    BRB->BufferSize = (ULONG)ReadBufferSize;

    NTSTATUS Status = SendBRB(DeviceContext, Request, (struct _BRB*)BRB, ReadFromDeviceCompletion);
    if (!NT_SUCCESS(Status)) TraceStatus("SendBRB Read Failed", Status);
    return Status;
}

VOID ReadFromDeviceCompletion(_In_ WDFREQUEST Request, _In_ WDFIOTARGET IoTarget, _In_ PWDF_REQUEST_COMPLETION_PARAMS Params, _In_ WDFCONTEXT Context)
{
    NTSTATUS Status = Params->IoStatus.Status;
    PDEVICE_CONTEXT DeviceContext = GetDeviceContext(WdfIoTargetGetDevice(IoTarget));
    PBLUETOOTH_DEVICE_CONTEXT BluetoothContext = &(DeviceContext->BluetoothContext);
    struct _BRB_L2CA_ACL_TRANSFER* BRB = (struct _BRB_L2CA_ACL_TRANSFER*)Context;
    WDF_REQUEST_REUSE_PARAMS RequestReuseParams;

    if (!NT_SUCCESS(Status))
    {
        TraceStatus("Continuous reader received error completion status", Status);
        BluetoothContext->ProfileDriverInterface.BthFreeBrb((struct _BRB*)BRB);
        WdfObjectDelete(Request);

        // CRITICAL BSOD FIX: Continuous reader completion routine executes at DISPATCH_LEVEL.
        // Safely clear channel handles without calling synchronous BRB functions at DISPATCH_LEVEL.
        BluetoothContext->InterruptChannelHandle = NULL;
        BluetoothContext->ControlChannelHandle = NULL;
        BluetoothContext->ConnectionState = WiimoteStateDisconnected;

        WiimoteReset(DeviceContext);
        SignalDeviceIsGone(DeviceContext);
        return;
    }

    PVOID ReadBuffer = BRB->Buffer;
    size_t ReadBufferSize = BRB->BufferSize;

    Status = WiimoteProcessReport(DeviceContext, ReadBuffer, (ReadBufferSize - BRB->RemainingBufferSize));
    if (!NT_SUCCESS(Status))
    {
        WdfObjectDelete(Request);
        return;
    }

    BluetoothContext->ProfileDriverInterface.BthReuseBrb((struct _BRB*)BRB, BRB_L2CA_ACL_TRANSFER);

    WDF_REQUEST_REUSE_PARAMS_INIT(&RequestReuseParams, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    if (!NT_SUCCESS(WdfRequestReuse(Request, &RequestReuseParams)))
    {
        WdfObjectDelete(Request);
        return;
    }

    RtlSecureZeroMemory(ReadBuffer, ReadBufferSize);
    ReadFromDevice(DeviceContext, Request, BRB, ReadBuffer, BluetoothContext->ReadBufferSize);
}

NTSTATUS BluetoothStartContiniousReader(_In_ PDEVICE_CONTEXT DeviceContext)
{
    CONST size_t ReadBufferSize = 50;
    WDFREQUEST Request;
    WDFMEMORY Memory;
    PVOID ReadBuffer = NULL;
    PBLUETOOTH_DEVICE_CONTEXT BluetoothContext = &(DeviceContext->BluetoothContext);

    Trace("StartContiniousReader");

    NTSTATUS Status = BluetoothCreateRequestAndBuffer(DeviceContext->Device, DeviceContext->IoTarget, ReadBufferSize, &Request, &Memory, &ReadBuffer);
    if (!NT_SUCCESS(Status)) return Status;

    BluetoothContext->ReadBufferSize = ReadBufferSize;

    struct _BRB* BRB = BluetoothContext->ProfileDriverInterface.BthAllocateBrb(BRB_L2CA_ACL_TRANSFER, BLUETOOTH_POOL_TAG);
    if (BRB == NULL)
    {
        WdfObjectDelete(Request);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    return ReadFromDevice(DeviceContext, Request, (struct _BRB_L2CA_ACL_TRANSFER*)BRB, ReadBuffer, ReadBufferSize);
}
