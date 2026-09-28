#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace dn {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
std::wstring g_path;
size_t g_maxBytes = 0;
std::unordered_map<std::string, ULONGLONG> g_lastByKey;

void rotateIfNeeded() {
    if (!g_file || g_maxBytes == 0) return;
    long size = ftell(g_file);
    if (size < 0 || static_cast<size_t>(size) < g_maxBytes) return;
    fclose(g_file);
    std::wstring old = g_path + L".1";
    MoveFileExW(g_path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = _wfopen(g_path.c_str(), L"ab");
}

void writeLine(const char* fmt, va_list ap) {
    if (!g_file) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_file, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
            t.wSecond, t.wMilliseconds);
    vfprintf(g_file, fmt, ap);
    fputc('\n', g_file);
    fflush(g_file);
    rotateIfNeeded();
}

}  // namespace

void logOpen(const std::wstring& path, size_t maxBytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_path = path;
    g_maxBytes = maxBytes;
    g_file = _wfopen(path.c_str(), L"ab");
}

void logClose() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) fclose(g_file);
    g_file = nullptr;
}

void logf(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    va_list ap;
    va_start(ap, fmt);
    writeLine(fmt, ap);
    va_end(ap);
}

void logRateLimited(const char* key, unsigned intervalMs, const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ULONGLONG now = GetTickCount64();
    auto it = g_lastByKey.find(key);
    if (it != g_lastByKey.end() && now - it->second < intervalMs) return;
    g_lastByKey[key] = now;
    va_list ap;
    va_start(ap, fmt);
    writeLine(fmt, ap);
    va_end(ap);
}

}  // namespace dn
