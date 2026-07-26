#pragma once
#include <ntddk.h>

typedef enum _AV_LOG_LEVEL {
    kLogpLevelDebug = 0,
    kLogpLevelInfo,
    kLogpLevelWarn,
    kLogpLevelError
} AV_LOG_LEVEL;

extern "C" void LogpPrint(
    _In_ AV_LOG_LEVEL Level,
    _In_ const char* FunctionName,
    _In_ _Printf_format_string_ const char* Format,
    ...
);

#define ARGUSVISOR_LOG_DEBUG(format, ...) \
    LogpPrint(kLogpLevelDebug, __FUNCTION__, (format), __VA_ARGS__)

#define ARGUSVISOR_LOG_INFO(format, ...) \
    LogpPrint(kLogpLevelInfo, __FUNCTION__, (format), __VA_ARGS__)

#define ARGUSVISOR_LOG_WARN(format, ...) \
    LogpPrint(kLogpLevelWarn, __FUNCTION__, (format), __VA_ARGS__)

#define ARGUSVISOR_LOG_ERROR(format, ...) \
    LogpPrint(kLogpLevelError, __FUNCTION__, (format), __VA_ARGS__)

