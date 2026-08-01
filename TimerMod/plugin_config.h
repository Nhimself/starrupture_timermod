#pragma once

#include "plugin_interface.h"
#include <cstdio>

namespace RuptureTimerConfig
{
	static const ConfigEntry CONFIG_ENTRIES[] = {
		// --- General ---
		{
			"General",
			"Enabled",
			ConfigValueType::Boolean,
			"true",
			"Enable or disable the RuptureTimer plugin",
			0.0f, 0.0f
		},
		// --- Export (StreamDeck / external tools) ---
		{
			"Export",
			"WriteJsonFile",
			ConfigValueType::Boolean,
			"true",
			"Write rupture timer state to a JSON file for external tools (e.g. StreamDeck)",
			0.0f, 0.0f
		},
		{
			"Export",
			"JsonFilePath",
			ConfigValueType::String,
			"Plugins/data/rupture_timer.json",
			"Path to the JSON output file (relative to the ModLoader directory; absolute paths are used as-is)",
			0.0f, 0.0f
		},
		{
			"Export",
			"UpdateIntervalSeconds",
			ConfigValueType::Float,
			"1.0",
			"How often (in seconds) to update the JSON file",
			0.1f, 30.0f
		},
		{
			"Export",
			"ExtendedPhaseTimers",
			ConfigValueType::Boolean,
			"false",
			"Add per-phase timing breakdown to JSON: warning_remaining_sec, burning_remaining_sec, cooling_remaining_sec, stabilizing_remaining_sec, stable_remaining_sec",
			0.0f, 0.0f
		},
		{
			"Export",
			"WriteDiagnosticLog",
			ConfigValueType::Boolean,
			"false",
			"Append raw game values to a diagnostic log file for debugging phase/timer issues over a full game session",
			0.0f, 0.0f
		},
		{
			"Export",
			"DiagnosticLogPath",
			ConfigValueType::String,
			"Plugins/data/timer_diagnostic.log",
			"Path to the diagnostic log file (relative to the ModLoader directory; absolute paths are used as-is)",
			0.0f, 0.0f
		},
		// --- HUD (in-game overlay) ---
		{
			"HUD",
			"ShowOverlay",
			ConfigValueType::Boolean,
			"false",
			"Show rupture timer as an in-game text overlay",
			0.0f, 0.0f
		},
		{
			"HUD",
			"Position",
			ConfigValueType::String,
			"LowerLeft",
			"Overlay anchor position: LowerLeft, MidLeft, TopLeft, TopMid, TopRight, MidRight, LowerRight",
			0.0f, 0.0f
		},
		{
			"HUD",
			"Scale",
			ConfigValueType::Float,
			"1.0",
			"Text scale multiplier (1.0 = default engine font size)",
			0.25f, 5.0f
		},
		{
			"HUD",
			"ShowDebugInfo",
			ConfigValueType::Boolean,
			"false",
			"Append raw diagnostic lines to the overlay: stage int, wave number, raw timer values. Use to diagnose phase-reading issues.",
			0.0f, 0.0f
		},
	};

	static const ConfigSchema SCHEMA = {
		CONFIG_ENTRIES,
		sizeof(CONFIG_ENTRIES) / sizeof(ConfigEntry)
	};

	class Config
	{
	public:
		static void Initialize(IPluginSelf* self)
		{
			s_self = self;
			if (s_self && s_self->config)
				s_self->config->InitializeFromSchema(s_self, &SCHEMA);
		}

		// Clear the cached self pointer on disposal so no post-shutdown caller
		// dereferences a stale modloader handle.
		static void Shutdown()
		{
			s_self = nullptr;
		}

		// --- General ---
		static bool IsEnabled()
		{
			return (s_self && s_self->config) ? s_self->config->ReadBool(s_self, "General", "Enabled", true) : true;
		}

		// --- Export ---
		static bool ShouldWriteJsonFile()
		{
			return (s_self && s_self->config) ? s_self->config->ReadBool(s_self, "Export", "WriteJsonFile", true) : true;
		}

		static const char* GetJsonFilePath()
		{
			static char buffer[512];
			static char resolved[512];
			const char* configured = "Plugins/data/rupture_timer.json";
			if (s_self && s_self->config && s_self->config->ReadString(s_self, "Export", "JsonFilePath", buffer, sizeof(buffer), "Plugins/data/rupture_timer.json"))
				configured = buffer;
			return ResolveOutputPath(configured, resolved, sizeof(resolved));
		}

		static float GetUpdateIntervalSeconds()
		{
			return (s_self && s_self->config) ? s_self->config->ReadFloat(s_self, "Export", "UpdateIntervalSeconds", 1.0f) : 1.0f;
		}

		static bool ShouldWriteExtendedPhaseTimers()
		{
			return (s_self && s_self->config) ? s_self->config->ReadBool(s_self, "Export", "ExtendedPhaseTimers", false) : false;
		}

		static bool ShouldWriteDiagnosticLog()
		{
			return (s_self && s_self->config) ? s_self->config->ReadBool(s_self, "Export", "WriteDiagnosticLog", false) : false;
		}

		static const char* GetDiagnosticLogPath()
		{
			static char buffer[512];
			static char resolved[512];
			const char* configured = "Plugins/data/timer_diagnostic.log";
			if (s_self && s_self->config && s_self->config->ReadString(s_self, "Export", "DiagnosticLogPath", buffer, sizeof(buffer), "Plugins/data/timer_diagnostic.log"))
				configured = buffer;
			return ResolveOutputPath(configured, resolved, sizeof(resolved));
		}

		// --- HUD ---
		static bool ShouldShowOverlay()
		{
			return (s_self && s_self->config) ? s_self->config->ReadBool(s_self, "HUD", "ShowOverlay", false) : false;
		}

		static const char* GetOverlayPosition()
		{
			static char buffer[64];
			if (s_self && s_self->config && s_self->config->ReadString(s_self, "HUD", "Position", buffer, sizeof(buffer), "LowerLeft"))
				return buffer;
			return "LowerLeft";
		}

		static float GetOverlayScale()
		{
			float scale = (s_self && s_self->config) ? s_self->config->ReadFloat(s_self, "HUD", "Scale", 1.0f) : 1.0f;
			if (scale < 0.25f) scale = 0.25f;
			if (scale > 5.0f)  scale = 5.0f;
			return scale;
		}

		static bool ShouldShowDebugInfo()
		{
			return (s_self && s_self->config) ? s_self->config->ReadBool(s_self, "HUD", "ShowDebugInfo", false) : false;
		}

	private:
		// Resolve a possibly-relative output path against the parent of the
		// directory this DLL lives in. The modloader loads plugins from
		// <exe_dir>\ModLoader\Plugins\, so the anchor is the ModLoader folder
		// and the "Plugins/data/..." defaults land in ModLoader\Plugins\data\.
		// Resolving against the CWD (as before) broke when the modloader moved
		// plugins out of Binaries\Win64\Plugins\ — the relative defaults then
		// pointed at a folder that no longer exists.
		static const char* ResolveOutputPath(const char* configured, char* out, size_t cap)
		{
			// Absolute paths (drive letter, rooted, or UNC) are used as-is.
			if (configured[0] == '\\' || configured[0] == '/' ||
				(configured[0] != '\0' && configured[1] == ':'))
				return configured;

			HMODULE hm = nullptr;
			if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                        reinterpret_cast<LPCSTR>(&s_self), &hm))
				return configured;

			char dllPath[MAX_PATH];
			DWORD n = GetModuleFileNameA(hm, dllPath, MAX_PATH);
			if (n == 0 || n >= MAX_PATH)
				return configured;

			// Strip the DLL filename, then the Plugins directory itself, to
			// land on the ModLoader folder the config paths are relative to.
			for (int strip = 0; strip < 2; strip++)
			{
				char* lastSep = nullptr;
				for (char* p = dllPath; *p; p++)
					if (*p == '\\' || *p == '/') lastSep = p;
				if (!lastSep) return configured;
				*lastSep = '\0';
			}

			int written = snprintf(out, cap, "%s\\%s", dllPath, configured);
			if (written <= 0 || static_cast<size_t>(written) >= cap)
				return configured;
			return out;
		}

		static IPluginSelf* s_self;
	};
}
