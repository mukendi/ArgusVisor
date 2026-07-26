#include "log.h"
#include <stdarg.h>
#include <ntstrsafe.h>


#if DBG
static AV_LOG_LEVEL g_GlobalLogLevel = kLogpLevelDebug;
#else
static AV_LOG_LEVEL g_GlobalLogLevel = kLogpLevelInfo;
#endif

extern "C" void LogpPrint(
    _In_ AV_LOG_LEVEL Level,
    _In_ const char* FunctionName,
    _In_ _Printf_format_string_ const char* Format,
    ...
)
{
    
    if (Level < g_GlobalLogLevel) {
        return;
    }

    const char* levelString = "UNKNOWN";
    switch (Level) {
    case kLogpLevelDebug: levelString = "DBG"; break;
    case kLogpLevelInfo:  levelString = "INF"; break;
    case kLogpLevelWarn:  levelString = "WRN"; break;
    case kLogpLevelError: levelString = "ERR"; break;
    }

    // Local message buffers on the stack (Reasonable size for the kernel)
    char messageBuffer[512];
    char finalBuffer[600];

    // Handling variable arguments (va_list)
    va_list args;
    va_start(args, Format);

    // Secure formatting of the user's message
    NTSTATUS status = RtlStringCchVPrintfA(messageBuffer, sizeof(messageBuffer), Format, args);
    va_end(args);

    if (!NT_SUCCESS(status)) {
        // If the formatting fails (e.g., message too long), it is cut short.
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[ArgusVisor] [ERR] Log formatting failed.\n");
        return;
    }

    
    status = RtlStringCchPrintfA(
        finalBuffer,
        sizeof(finalBuffer),
        "[ArgusVisor] [%s] [%s] %s\n",
        levelString,
        FunctionName,
        messageBuffer
    );

    if (NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "%s", finalBuffer);
    }
}