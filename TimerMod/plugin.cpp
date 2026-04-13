#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "timer_tracker.h"
#include "data_export.h"
#include "hud_overlay.h"
#include "plugin_network_helpers.h"

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

// Server-side: how often to broadcast the timer state to clients.
// Active phases (Warning/Burning/Cooling/Stabilizing) use the shorter interval
// because every second matters there; Stable can afford a longer heartbeat.
static constexpr float NET_SYNC_INTERVAL_STABLE = 10.0f;
static constexpr float NET_SYNC_INTERVAL_ACTIVE =  2.0f;
static float s_netSyncAccum = 0.0f;

// Track which tick function was registered so PluginShutdown unregisters the right one.
static void (*s_registeredTick)(float) = nullptr;

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

static void TickHeartbeat(float deltaSeconds)
{
	static float s_logAccum = 0.0f;
	s_logAccum += deltaSeconds;
	if (s_logAccum >= 5.0f)
	{
		s_logAccum = 0.0f;
		LOG_DEBUG("Tick alive — deltaSeconds=%.4f", deltaSeconds);
	}
}

// ---------------------------------------------------------------------------
// Server tick — reads game state from local subsystem, broadcasts to clients.
// Registered only when IsServer() == true at init time.
// ---------------------------------------------------------------------------
static void OnServerTick(float deltaSeconds)
{
	if (!s_worldReady) return;

	TickHeartbeat(deltaSeconds);

	s_lastState = RuptureTimer::ReadCurrentState();

	auto* hooks = g_self ? g_self->hooks : nullptr;
	if (hooks && hooks->Network && s_lastState.valid)
	{
		bool activePhase = (s_lastState.phase != RuptureTimer::RupturePhase::Stable &&
		                    s_lastState.phase != RuptureTimer::RupturePhase::Unknown);
		float interval = activePhase ? NET_SYNC_INTERVAL_ACTIVE : NET_SYNC_INTERVAL_STABLE;

		s_netSyncAccum += deltaSeconds;
		if (s_netSyncAccum >= interval)
		{
			s_netSyncAccum = 0.0f;

			RuptureTimer::TimerSyncPacket pkt{};
			pkt.phaseRemainingSeconds = s_lastState.phaseRemainingSeconds;
			pkt.nextRuptureInSeconds  = s_lastState.nextRuptureInSeconds;
			pkt.stableRemaining       = s_lastState.stableRemaining;
			pkt.waveNumber            = s_lastState.waveNumber;
			pkt.serverId              = RuptureTimer::GetServerId();
			pkt.phase                 = static_cast<uint8_t>(s_lastState.phase);
			pkt.waveType              = s_lastState.waveType;
			pkt.paused                = s_lastState.paused ? 1 : 0;
			// rawStage carries the server's EEnviroWaveStage so the client gets the
			// authoritative stage enum (Fadeout vs Growback) instead of guessing.
			pkt.rawStage              = (s_lastState.diag.rawStage >= 0)
			                              ? static_cast<uint8_t>(s_lastState.diag.rawStage)
			                              : 0;

			Network::SendPacketToAllClients(hooks, g_self, pkt);
			LOG_DEBUG("NetSync sent: phase=%d rem=%.1f nextRup=%.1f serverId=0x%08X",
				(int)pkt.phase, pkt.phaseRemainingSeconds, pkt.nextRuptureInSeconds, pkt.serverId);
		}
	}

	DataExport::Update(deltaSeconds, s_lastState);
	DataExport::UpdateDiagnosticLog(deltaSeconds, s_lastState);
	HudOverlay::SetState(s_lastState);  // no-op on dedicated server (hooks->HUD is null)
}

// ---------------------------------------------------------------------------
// Client tick — reads state via netSync (or local fallback if never paired),
// updates HUD. Registered only when IsServer() == false at init time.
// ---------------------------------------------------------------------------
static void OnClientTick(float deltaSeconds)
{
	if (!s_worldReady) return;

	TickHeartbeat(deltaSeconds);

	s_lastState = RuptureTimer::ReadCurrentState();

	DataExport::Update(deltaSeconds, s_lastState);
	DataExport::UpdateDiagnosticLog(deltaSeconds, s_lastState);
	HudOverlay::SetState(s_lastState);
}

// ---------------------------------------------------------------------------
// Local tick — no network channel available (solo play, offline, etc.).
// Reads game state from local subsystem / repActor only.
// ---------------------------------------------------------------------------
static void OnLocalTick(float deltaSeconds)
{
	if (!s_worldReady) return;

	TickHeartbeat(deltaSeconds);

	s_lastState = RuptureTimer::ReadCurrentState();

	DataExport::Update(deltaSeconds, s_lastState);
	DataExport::UpdateDiagnosticLog(deltaSeconds, s_lastState);
	HudOverlay::SetState(s_lastState);
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void OnAnyWorldBeginPlay(SDK::UWorld* world, const char* worldName)
{
	if (!world || !worldName) return;

	// Only track the main gameplay world
	if (std::string_view(worldName).find("ChimeraMain") == std::string_view::npos)
	{
		if (s_worldReady)
		{
			LOG_INFO("World changed to non-gameplay world (%s) — pausing rupture timer tracking", worldName);
			s_worldReady = false;
		}
		return;
	}

	LOG_INFO("World ready: %s — starting rupture timer tracking", worldName);
	s_worldReady = true;
	DataExport::EnsureOutputDir();
	DataExport::EnsureDiagnosticLogDir();
}

static void OnSaveLoaded()
{
	LOG_INFO("Save loaded — reading initial rupture timer state");
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
		LOG_WARN("  Timer state not available yet (game state not fully loaded?)");
	}
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
		hooks->World->RegisterOnSaveLoaded(OnSaveLoaded);
		LOG_DEBUG("Registered world/save callbacks");
	}

	// Boot into server or client mode based on the network channel.
	// IsServer() is reliable at PluginInit time per the SDK contract (plugin_network_helpers.h).
	// The same DLL handles both modes: server broadcasts, client receives and displays.
	if (hooks->Network)
	{
		if (hooks->Network->IsServer())
		{
			// ---- SERVER MODE ----
			// Generate a unique session ID embedded in every broadcast packet so
			// clients can lock onto one server and ignore competing broadcasts.
			LOG_INFO("Mode: SERVER — will broadcast TimerSyncPacket to clients");
			RuptureTimer::InitServerMode();

			if (hooks->Engine)
			{
				hooks->Engine->RegisterOnTick(OnServerTick);
				s_registeredTick = OnServerTick;
				LOG_DEBUG("Registered OnServerTick");
			}
			// Server does not register an OnReceive handler — it only sends.
		}
		else
		{
			// ---- CLIENT MODE ----
			// Register a receive handler that pairs to the first server heard from
			// and ignores all subsequent competing servers (race condition guard).
			LOG_INFO("Mode: CLIENT — will receive TimerSyncPacket from server");
			Network::OnReceive<RuptureTimer::TimerSyncPacket>(
				hooks, self,
				[](const RuptureTimer::TimerSyncPacket& pkt)
				{
					RuptureTimer::ApplyNetworkSync(pkt);
				});
			LOG_DEBUG("Registered TimerSyncPacket receive handler");

			if (hooks->Engine)
			{
				hooks->Engine->RegisterOnTick(OnClientTick);
				s_registeredTick = OnClientTick;
				LOG_DEBUG("Registered OnClientTick");
			}
		}
	}
	else
	{
		// ---- LOCAL MODE ----
		// No network channel (solo play, offline, or old modloader build).
		// Fall back to reading game state directly from local actors.
		LOG_DEBUG("hooks->Network not available — local-only mode");
		if (hooks->Engine)
		{
			hooks->Engine->RegisterOnTick(OnLocalTick);
			s_registeredTick = OnLocalTick;
			LOG_DEBUG("Registered OnLocalTick");
		}
	}

	// HUD overlay — only register if the overlay is enabled in config.
	// hooks->HUD is null on dedicated server builds; Install() handles that gracefully.
	if (RuptureTimerConfig::Config::ShouldShowOverlay())
	{
		if (!HudOverlay::Install(hooks))
			LOG_WARN("HUD overlay could not be installed — in-game display will be unavailable");
	}
	else
	{
		LOG_DEBUG("HUD overlay disabled in config (HUD.ShowOverlay=false)");
	}

	// If we are being loaded/reloaded while a game world is already active
	// (e.g. via the mod loader UI), OnAnyWorldBeginPlay will not fire again —
	// it only fires on world transitions. Detect this by attempting a direct
	// state read: if it succeeds the game is already running and we can start
	// tracking immediately without waiting for the next world load.
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
			hooks->World->UnregisterOnSaveLoaded(OnSaveLoaded);
		}
		if (hooks->Engine && s_registeredTick)
			hooks->Engine->UnregisterOnTick(s_registeredTick);
	}

	s_registeredTick = nullptr;
	g_self = nullptr;
}

} // extern "C"
