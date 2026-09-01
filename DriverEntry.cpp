/*
Filename: DriverEntry.cpp
Abstract: The true kernel-mode entry point. Initializes the KMDF framework.
*/

#include <ntddk.h>
#include <wdf.h>
#include "Device.h"

extern "C" NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    WDF_DRIVER_CONFIG Config;
    NTSTATUS Status;

    // Initialize the driver configuration and point it to our DeviceAdd callback
    WDF_DRIVER_CONFIG_INIT(&Config, DeviceAdd);

    // Create the KMDF Driver Object
    Status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &Config,
        WDF_NO_HANDLE
    );

    return Status;
}
