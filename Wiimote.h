/*
Filename: Wiimote.h
Abstract: Declares the Wiimote state container utilizing the C++ struct.
          Includes WDFWORKITEM for safe PASSIVE_LEVEL initialization.
*/

#pragma once

#include <ntddk.h>
#include <wdf.h>

#ifndef _CSTDINT_
#define _CSTDINT_
#define _CSTDINT
#define _STDINT
#define _STDINT_H
#define _INC_STDINT
#define _VCRUNTIME_H_
#define _VCRUNTIME_H
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned __int64 uint64_t;
typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef __int64 int64_t;
#endif

#include "WiimoteData.h"

typedef struct _WIIMOTE_DEVICE_CONTEXT {
    InputReport37 CurrentState;
    BOOLEAN IsRumbling;
    WDFWORKITEM InitWorkItem; // Used to safely delay execution at PASSIVE_LEVEL
} WIIMOTE_DEVICE_CONTEXT, * PWIIMOTE_DEVICE_CONTEXT;

struct _DEVICE_CONTEXT;

NTSTATUS WiimotePrepare(_In_ struct _DEVICE_CONTEXT* DeviceContext);
NTSTATUS WiimoteStart(_In_ struct _DEVICE_CONTEXT* DeviceContext);
NTSTATUS WiimoteStop(_In_ struct _DEVICE_CONTEXT* DeviceContext);
VOID WiimoteReset(_In_ struct _DEVICE_CONTEXT* DeviceContext);
NTSTATUS WiimoteProcessReport(_In_ struct _DEVICE_CONTEXT* DeviceContext, _In_ PVOID Buffer, _In_ SIZE_T BufferSize);
NTSTATUS SendBluetoothDataSynchronous(_In_ struct _DEVICE_CONTEXT* DeviceContext, _In_ PUCHAR Data, _In_ SIZE_T Size);
