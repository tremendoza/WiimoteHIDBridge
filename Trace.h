/*
Filename: Trace.h
Abstract: Declarations and configuration for Tracing.
*/

#pragma once

#include <ntddk.h>
#include <ntstrsafe.h>

VOID Trace(_In_ PCSTR DebugMessage, ...);
VOID TraceStatus(_In_ PCSTR DebugMessage, _In_ NTSTATUS Status);
