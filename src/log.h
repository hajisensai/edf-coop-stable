#pragma once
#include <string>

namespace dn {

// Opens the log file (UTF-8). When it grows past maxBytes the file is rotated to "<path>.1".
void logOpen(const std::wstring& path, size_t maxBytes);
void logClose();
void logf(const char* fmt, ...);

// Sends every line to `sink` (one line, no timestamp, no line end) instead of a file of its own: in EDF6Coop
// the room part's crash-safe log is the one log. nullptr goes back to the file.
using LogSink = void (*)(const char* line);
void logToSink(LogSink sink);

// Same as logf, but at most once per `intervalMs` for the same key (for per-packet paths).
void logRateLimited(const char* key, unsigned intervalMs, const char* fmt, ...);

}  // namespace dn
