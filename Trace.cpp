/*
Filename: Trace.cpp
Abstract: Implementation for kernel-mode tracing.
*/

#include "Trace.h"

VOID Trace(_In_ PCSTR DebugMessage, ...)
{
#ifndef DBG
    UNREFERENCED_PARAMETER(DebugMessage);
#else
    NTSTATUS Status;
    va_list ParameterList;
    CHAR DebugMessageBuffer[512];

    va_start(ParameterList, DebugMessage);

    if (DebugMessage != NULL)
    {
        Status = RtlStringCbVPrintfA(DebugMessageBuffer, sizeof(DebugMessageBuffer), DebugMessage, ParameterList);
        if (NT_SUCCESS(Status))
        {
            DbgPrint("Trace Wiimote: %s\n", DebugMessageBuffer);
        }
    }

    va_end(ParameterList);
#endif
}

VOID TraceStatus(_In_ PCSTR DebugMessage, _In_ NTSTATUS Status)
{
#ifndef DBG
    UNREFERENCED_PARAMETER(DebugMessage);
    UNREFERENCED_PARAMETER(Status);
#else
    if (DebugMessage != NULL)
    {
        DbgPrint("Trace Wiimote: %s Result: 0x%x\n", DebugMessage, Status);
    }
#endif
}
