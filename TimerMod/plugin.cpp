#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "timer_tracker.h"
#include "wave_packet.h"
#include "data_export.h"
#include "hud_overlay.h"

#include <cstring>
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
	PLUGIN_INTERFACE_VERSION,
	PLUGIN_TARGET_CLIENT   // v33: this DLL is the client build
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool s_worldReady = false;
static RuptureTimer::TimerState s_lastState{};

// Hot-reload probe budget. When the plugin is reloaded via the mod menu into
// an already-running game, no World/Experience callback fires (BeginPlay
// already happened), so we detect a live world from the first engine ticks.
// ~10 s at 60 fps tolerates GameState/WaveTimerActor replication latency.
static int s_hotReloadProbeTicks = 600;

// The HUD PostRender callback must NOT be registered from PluginInit: that
// runs during UGameEngine::Init, before the engine/HUD exist, and hooking it
// there faults inside engine init (loader stuck at 85%). Register it lazily
// once the experience has loaded / a live world is detected, exactly once.
static bool s_hudInstalled = false;

static void MaybeInstallHud()
{
	if (s_hudInstalled)
		return;
	if (!RuptureTimerConfig::Config::ShouldShowOverlay())
		return;

	IPluginHooks* hooks = GetHooks();
	if (!hooks)
		return;

	if (HudOverlay::Install(hooks))
	{
		s_hudInstalled = true;
		LOG_INFO("HUD overlay installed");
	}
	else
	{
		LOG_WARN("HUD overlay could not be installed — in-game display will be unavailable");
	}
}

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

	// Register the HUD PostRender callback here (per modloader guidance):
	// PluginInit is too early — the engine/HUD do not exist yet.
	MaybeInstallHud();

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

static void OnNetworkWaveState(const char* /*pluginName*/, const char* /*typeTag*/,
                               const uint8_t* data, size_t size)
{
	if (size < sizeof(WaveStatePacket)) return;
	WaveStatePacket pkt;
	memcpy(&pkt, data, sizeof(pkt));
	RuptureTimer::SetNetworkState(pkt);
}

static void OnEngineTick(float deltaSeconds)
{
	// Hot-reload path: plugin reloaded into a running game. Engine ticks only
	// fire after UGameEngine::Init has completed, so reading game state here is
	// safe (unlike PluginInit). Bounded so we stop probing in menus.
	if (!s_worldReady && s_hotReloadProbeTicks > 0)
	{
		--s_hotReloadProbeTicks;
		RuptureTimer::TimerState probe = RuptureTimer::ReadCurrentState();
		if (probe.valid)
		{
			LOG_INFO("Active game world detected on tick — starting tracking (hot-reload)");
			s_worldReady = true;
			DataExport::EnsureOutputDir();
			DataExport::EnsureDiagnosticLogDir();
			MaybeInstallHud();
		}
	}

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

	LOG_INFO("Initializing...");

	RuptureTimerConfig::Config::Initialize(self);

	if (!RuptureTimerConfig::Config::IsEnabled())
	{
		LOG_WARN("Disabled in config");
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

	if (hooks->Network && !hooks->Network->IsServer())
	{
		hooks->Network->RegisterMessageHandler(self, WAVE_STATE_TYPE_TAG, OnNetworkWaveState);
		LOG_DEBUG("Registered network wave state handler");
	}

	// Do NOT register the HUD PostRender callback or read game state here.
	// PluginInit runs during UGameEngine::Init, before the engine/HUD/world
	// exist; hooking PostRender here faults inside engine init and leaves the
	// loader stuck at 85%. The overlay is installed from OnExperienceLoadComplete
	// (cold start) or the OnEngineTick probe (hot-reload). See MaybeInstallHud().

	LOG_INFO("Initialized — JSON output: %s | HUD overlay: %s",
		RuptureTimerConfig::Config::GetJsonFilePath(),
		RuptureTimerConfig::Config::ShouldShowOverlay() ? "enabled" : "disabled");

	return true;
}

__declspec(dllexport) void PluginShutdown()
{
	LOG_INFO("Shutting down...");

	s_worldReady = false;

	if (g_self && g_self->hooks)
	{
		auto* hooks = g_self->hooks;

		if (s_hudInstalled)
			HudOverlay::Remove(hooks);

		if (hooks->Network && !hooks->Network->IsServer())
			hooks->Network->UnregisterMessageHandler(g_self, WAVE_STATE_TYPE_TAG, OnNetworkWaveState);

		if (hooks->World)
		{
			hooks->World->UnregisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
			hooks->World->UnregisterOnBeforeWorldEndPlay(OnBeforeWorldEndPlay);
			hooks->World->UnregisterOnExperienceLoadComplete(OnExperienceLoadComplete);
		}
		if (hooks->Engine)
			hooks->Engine->UnregisterOnTick(OnEngineTick);
	}

	// Reset lazy-init state so a reload starts clean.
	s_hudInstalled        = false;
	s_hotReloadProbeTicks = 600;

	// All game-thread callbacks are unregistered and the render thread is
	// drained, so the cached config self pointer can no longer be used.
	RuptureTimerConfig::Config::Shutdown();

	g_self = nullptr;
}

} // extern "C"
