#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>

static FILE* g_log;

void LogOpen(const std::string& path)
{
    if (!g_log)
        g_log = _fsopen(path.c_str(), "w", _SH_DENYWR);
}

void Log(const char* fmt, ...)
{
    if (!g_log)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}
