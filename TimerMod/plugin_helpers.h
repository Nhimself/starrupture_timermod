#pragma once

#include "plugin_interface.h"

// Forward declaration to access the single global plugin self pointer
IPluginSelf* GetSelf();

// Convenience accessors
inline IPluginHooks*   GetHooks()   { auto* s = GetSelf(); return s ? s->hooks   : nullptr; }
inline IPluginConfig*  GetConfig()  { auto* s = GetSelf(); return s ? s->config  : nullptr; }
inline IPluginScanner* GetScanner() { auto* s = GetSelf(); return s ? s->scanner : nullptr; }

// Convenience macros for logging (v19: pass IPluginSelf* instead of plugin name string)
#define LOG_TRACE(format, ...) do { if (auto* _s = GetSelf()) _s->logger->Trace(_s, format, ##__VA_ARGS__); } while(0)
#define LOG_DEBUG(format, ...) do { if (auto* _s = GetSelf()) _s->logger->Debug(_s, format, ##__VA_ARGS__); } while(0)
#define LOG_INFO(format, ...)  do { if (auto* _s = GetSelf()) _s->logger->Info (_s, format, ##__VA_ARGS__); } while(0)
#define LOG_WARN(format, ...)  do { if (auto* _s = GetSelf()) _s->logger->Warn (_s, format, ##__VA_ARGS__); } while(0)
#define LOG_ERROR(format, ...) do { if (auto* _s = GetSelf()) _s->logger->Error(_s, format, ##__VA_ARGS__); } while(0)

// Log once per process lifetime (uses a local static bool)
#define LOG_INFO_ONCE(format, ...) do { static bool _logged = false; if (!_logged) { _logged = true; LOG_INFO(format, ##__VA_ARGS__); } } while(0)
#define LOG_WARN_ONCE(format, ...) do { static bool _logged = false; if (!_logged) { _logged = true; LOG_WARN(format, ##__VA_ARGS__); } } while(0)
