#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "timer_tracker.h"
#include "data_export.h"
#include "hud_overlay.h"

#include "Engine_classes.hpp"

// ---------------------------------------------------------------------------
// Global plugin self pointer (v19: single struct instead of 4 separate ptrs)
// ---------------------------------------------------------------------------
static IPluginSelf* g_self = nullptr;

IPluginSelf* GetSelf() { return g_self; }

// ---------------------------------------------------------------------------
// Plugin metadata
// ---------------------------------------------------------------------------
#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

static PluginInfo s_pluginInfo = {
	"RuptureTimer",
	MODLOADER_BUILD_TAG,
	"Nhimself",
	"Tracks the rupture wave timer. Exports phase/countdown data to JSON for StreamDeck integration and optionally renders an in-game HUD overlay.",
	PLUGIN_INTERFACE_VERSION
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool s_worldReady = false;
static RuptureTimer::TimerState s_lastState{};

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void OnBeforeWorldEndPlay(SDK::UWorld* /*world*/, const char* worldName)
{
	if (!s_worldReady) return;
	LOG_INFO("World ending: %s — pausing rupture timer tracking", worldName ? worldName : "?");
	s_worldReady = false;
	s_lastState = {};
}

static void OnAnyWorldBeginPlay(SDK::UWorld* world, const char* worldName)
{
	if (!world || !worldName) return;

	if (std::string_view(worldName).find("ChimeraMain") == std::string_view::npos)
		return;

	LOG_INFO("World ready: %s — starting rupture timer tracking", worldName);
	s_worldReady = true;
	DataExport::EnsureOutputDir();
	DataExport::EnsureDiagnosticLogDir();
}

static void OnExperienceLoadComplete()
{
	LOG_INFO("Experience load complete — reading initial rupture timer state");
	s_lastState = RuptureTimer::ReadCurrentState();
	if (s_lastState.valid)
	{
		LOG_INFO("  Phase: %s | Remaining: %.1fs | Wave #%d | Type: %s",
			s_lastState.phaseName,
			s_lastState.phaseRemainingSeconds,
			s_lastState.waveNumber,
			s_lastState.waveTypeName);
		HudOverlay::SetState(s_lastState);
	}
	else
	{
		LOG_WARN("  Timer state not available yet after experience load — will retry on next tick");
	}
}

static void OnEngineTick(float deltaSeconds)
{
	if (!s_worldReady) return;

	static float s_logAccum = 0.0f;
	s_logAccum += deltaSeconds;
	if (s_logAccum >= 5.0f)
	{
		s_logAccum = 0.0f;
		LOG_DEBUG("Tick alive — deltaSeconds=%.4f", deltaSeconds);
	}

	s_lastState = RuptureTimer::ReadCurrentState();
	DataExport::Update(deltaSeconds, s_lastState);
	DataExport::UpdateDiagnosticLog(deltaSeconds, s_lastState);
	HudOverlay::SetState(s_lastState);
}

// ---------------------------------------------------------------------------
// Plugin exports
// ---------------------------------------------------------------------------
extern "C" {

__declspec(dllexport) PluginInfo* GetPluginInfo()
{
	return &s_pluginInfo;
}

__declspec(dllexport) bool PluginInit(IPluginSelf* self)
{
	g_self = self;

	LOG_INFO("RuptureTimer initializing...");

	RuptureTimerConfig::Config::Initialize(self);

	if (!RuptureTimerConfig::Config::IsEnabled())
	{
		LOG_WARN("RuptureTimer is disabled in config");
		return true;
	}

	if (!self || !self->hooks)
	{
		LOG_ERROR("hooks interface is null — cannot register callbacks");
		return false;
	}

	auto* hooks = self->hooks;

	if (hooks->World)
	{
		hooks->World->RegisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
		hooks->World->RegisterOnBeforeWorldEndPlay(OnBeforeWorldEndPlay);
		hooks->World->RegisterOnExperienceLoadComplete(OnExperienceLoadComplete);
		LOG_DEBUG("Registered world callbacks");
	}

	if (hooks->Engine)
	{
		hooks->Engine->RegisterOnTick(OnEngineTick);
		LOG_DEBUG("Registered OnEngineTick");
	}

	if (RuptureTimerConfig::Config::ShouldShowOverlay())
	{
		if (!HudOverlay::Install(hooks))
			LOG_WARN("HUD overlay could not be installed — in-game display will be unavailable");
	}
	else
	{
		LOG_DEBUG("HUD overlay disabled in config (HUD.ShowOverlay=false)");
	}

	// Detect hot-reload into an already-running world.
	{
		s_lastState = RuptureTimer::ReadCurrentState();
		if (s_lastState.valid)
		{
			LOG_INFO("Active game world detected on init — starting tracking immediately");
			s_worldReady = true;
			DataExport::EnsureOutputDir();
			DataExport::EnsureDiagnosticLogDir();
			HudOverlay::SetState(s_lastState);
		}
	}

	LOG_INFO("RuptureTimer initialized — JSON output: %s | HUD overlay: %s",
		RuptureTimerConfig::Config::GetJsonFilePath(),
		RuptureTimerConfig::Config::ShouldShowOverlay() ? "enabled" : "disabled");

	return true;
}

__declspec(dllexport) void PluginShutdown()
{
	LOG_INFO("RuptureTimer shutting down...");

	s_worldReady = false;

	if (g_self && g_self->hooks)
	{
		auto* hooks = g_self->hooks;

		HudOverlay::Remove(hooks);

		if (hooks->World)
		{
			hooks->World->UnregisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
			hooks->World->UnregisterOnBeforeWorldEndPlay(OnBeforeWorldEndPlay);
			hooks->World->UnregisterOnExperienceLoadComplete(OnExperienceLoadComplete);
		}
		if (hooks->Engine)
			hooks->Engine->UnregisterOnTick(OnEngineTick);
	}

	g_self = nullptr;
}

} // extern "C"
