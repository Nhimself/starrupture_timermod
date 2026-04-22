#ifdef MODLOADER_SERVER_BUILD

#include "wave_packet.h"
#include "plugin_helpers.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cmath>
#include <cstring>
#include <string_view>
#include "Basic.hpp"
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"
#include "Chimera_structs.hpp"

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

static IPluginSelf* g_self = nullptr;
IPluginSelf* GetSelf() { return g_self; }

// Plugin name must match the client plugin so the modloader routes messages by
// (pluginName + typeTag) to the correct RegisterMessageHandler subscription.
static PluginInfo s_pluginInfo = {
	"RuptureTimer",
	MODLOADER_BUILD_TAG,
	"Nhimself",
	"Server-side companion: reads UCrEnviroWaveSubsystem and broadcasts WaveStatePacket to all clients.",
	PLUGIN_INTERFACE_VERSION
};

// ---------------------------------------------------------------------------
// Object cache — find UCrEnviroWaveSubsystem on the server.
// Retries every scan until found (subsystem may not be in GObjects yet at
// world-begin), then caches permanently for the lifetime of the world.
// ---------------------------------------------------------------------------
static SDK::UWorld*                 s_cachedWorld  = nullptr;
static SDK::UCrEnviroWaveSubsystem* s_cachedSub    = nullptr;
static int                          s_scanAttempts = 0;

static SDK::UCrEnviroWaveSubsystem* FindSubsystem(SDK::UWorld* world)
{
	if (world != s_cachedWorld)
	{
		s_cachedWorld  = world;
		s_cachedSub    = nullptr;
		s_scanAttempts = 0;
	}
	if (s_cachedSub) return s_cachedSub;

	s_scanAttempts++;
	SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
	if (!arr) return nullptr;
	const SDK::UObject* cdo = SDK::UCrEnviroWaveSubsystem::GetDefaultObj();
	for (int i = 0; i < arr->Num(); i++)
	{
		SDK::UObject* obj = arr->GetByIndex(i);
		if (!obj || obj == cdo || !obj->Class) continue;
		if (obj->Class->GetName() == "CrEnviroWaveSubsystem")
		{
			s_cachedSub = static_cast<SDK::UCrEnviroWaveSubsystem*>(obj);
			LOG_INFO("UCrEnviroWaveSubsystem found after %d scan(s)", s_scanAttempts);
			return s_cachedSub;
		}
	}
	// Log periodically so we can tell if the name or CDO skip is the problem.
	if (s_scanAttempts == 1 || s_scanAttempts == 30 || s_scanAttempts == 300)
		LOG_WARN("UCrEnviroWaveSubsystem not found after %d scan(s) — still searching", s_scanAttempts);
	return nullptr;
}

// ---------------------------------------------------------------------------
// Phase duration from game settings (subsystem path only).
// ---------------------------------------------------------------------------
static float StageDuration(SDK::EEnviroWaveStage stage, const SDK::FCrEnviroWaveSettings& s)
{
	switch (stage)
	{
		case SDK::EEnviroWaveStage::PreWave:
			return s.PreWaveDuration + s.WavePreWaveExplosionDuration;
		case SDK::EEnviroWaveStage::Moving:
			return (s.WaveSpeed > 0.0f)
				? fabsf(s.WaveEndPosition - s.WaveStartPosition) / s.WaveSpeed : 0.0f;
		case SDK::EEnviroWaveStage::Fadeout:
			return s.WaveFadeoutFireWaveDuration + s.WaveFadeoutBurningDuration + s.WaveFadeoutFadingDuration;
		case SDK::EEnviroWaveStage::Growback:
			return s.WaveGrowbackMoonPhaseDuration + s.WaveGrowbackRegrowthStartDuration + s.WaveGrowbackRegrowthDuration;
		default:
			return 0.0f;
	}
}

// ---------------------------------------------------------------------------
// Change detection — broadcast on state delta + 5 s heartbeat.
// ---------------------------------------------------------------------------
struct BroadcastSnapshot
{
	uint8_t  phase       = 0xFF;
	uint8_t  waveType    = 0xFF;
	int8_t   fadeoutSub  = -99;
	int8_t   growbackSub = -99;
	int8_t   preWaveSub  = -99;
	int32_t  waveNumber  = -1;
};
static BroadcastSnapshot s_last;
static float s_heartbeatAccum    = 0.0f;
static bool  s_worldReady        = false;
static constexpr float HEARTBEAT = 5.0f;

// Canonical post-wave split: first 60s = Cooling, remaining 600s = Stabilizing.
// Matches the client-side stateMachine constants used when no anchor is observed.
static constexpr float STABILIZING_DURATION = 600.0f;

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------
static void OnBeforeWorldEndPlay(SDK::UWorld*, const char* name)
{
	if (!s_worldReady) return;
	LOG_INFO("World ending: %s — stopping broadcasts", name ? name : "?");
	s_worldReady  = false;
	s_cachedWorld = nullptr;
	s_cachedSub   = nullptr;
	s_last        = BroadcastSnapshot{};
}

static void OnAnyWorldBeginPlay(SDK::UWorld* world, const char* name)
{
	if (!world || !name) return;
	if (std::string_view(name).find("ChimeraMain") == std::string_view::npos) return;
	LOG_INFO("World ready: %s — server wave broadcasts active", name);
	s_worldReady = true;
}

static void OnEngineTick(float deltaSeconds)
{
	if (!s_worldReady) return;

	SDK::UWorld* world = SDK::UWorld::GetWorld();
	if (!world) return;
	auto* gs = static_cast<SDK::ACrGameStateBase*>(world->GameState);
	if (!gs || !gs->WaveTimerActor) return;

	SDK::ACrWaveTimerActor* timer      = gs->WaveTimerActor;
	double                  serverTime = gs->GetServerWorldTimeSeconds();

	// Primary source: WaveTimerActor — always valid on server, loaded from save.
	// NextTime is an absolute server timestamp; the server's own clock is exact.
	int32_t nextPhase    = timer->NextPhase;
	float   ntRaw        = timer->NextTime - static_cast<float>(serverTime);
	float   ntRemaining  = (ntRaw < 0.0f) ? 0.0f : ntRaw;

	// Derive RupturePhase from NextPhase (same mapping as client stateMachine):
	//   0=Stable  1=Warning  2=Burning  3=PostWave(Cooling+Stabilizing combined)
	// For nextPhase==3, split on time remaining vs the Stabilizing boundary.
	uint8_t phase;
	switch (nextPhase) {
		case 0:  phase = 1; break;
		case 1:  phase = 2; break;
		case 2:  phase = 3; break;
		case 3:  phase = (ntRemaining > STABILIZING_DURATION) ? 4 : 5; break;
		default: phase = 0; break;
	}

	float nextRuptureIn  = (phase == 3) ? 0.0f : ntRemaining;
	float phaseRemaining = (phase == 1 || phase == 2) ? ntRemaining : -1.0f;

	// Optional enrichment: subsystem provides wave type, substages, and precise
	// phaseRemaining. Subsystem absence no longer blocks broadcasts.
	uint8_t wt         = 0;
	int8_t  fadeoutSub = -1, growbackSub = -1, preWaveSub = -1;

	SDK::UCrEnviroWaveSubsystem* sub = FindSubsystem(world);
	if (sub)
	{
		SDK::EEnviroWave      waveType = sub->GetCurrentType();
		SDK::EEnviroWaveStage stage    = sub->GetCurrentStage();
		wt = static_cast<uint8_t>(waveType);

		fadeoutSub  = (stage == SDK::EEnviroWaveStage::Fadeout)
			? static_cast<int8_t>(sub->CurrentFadeoutSubstage)  : int8_t(-1);
		growbackSub = (stage == SDK::EEnviroWaveStage::Growback)
			? static_cast<int8_t>(sub->CurrentGrowbackSubstage) : int8_t(-1);
		preWaveSub  = (stage == SDK::EEnviroWaveStage::PreWave)
			? static_cast<int8_t>(sub->CurrentPreWaveSubstage)  : int8_t(-1);

		if (stage != SDK::EEnviroWaveStage::None)
		{
			float progress = sub->GetCurrentStageProgress();
			if (progress >= 0.0f && progress <= 1.0f)
			{
				SDK::FCrEnviroWaveSettings settings = sub->GetCurrentStageSettings();
				float dur = StageDuration(stage, settings);
				phaseRemaining = dur * (1.0f - progress);
				if (phaseRemaining < 0.0f) phaseRemaining = 0.0f;
			}
		}
		else
		{
			phaseRemaining = ntRemaining;
		}
	}

	bool changed = (phase != s_last.phase || wt != s_last.waveType ||
	                fadeoutSub != s_last.fadeoutSub || growbackSub != s_last.growbackSub ||
	                preWaveSub != s_last.preWaveSub || nextPhase != s_last.waveNumber);

	s_heartbeatAccum += deltaSeconds;
	bool heartbeat = (s_heartbeatAccum >= HEARTBEAT);
	if (heartbeat) s_heartbeatAccum = 0.0f;

	if (changed || heartbeat)
	{
		auto* net = (g_self && g_self->hooks) ? g_self->hooks->Network : nullptr;
		if (net)
		{
			WaveStatePacket pkt{};
			pkt.phase          = phase;
			pkt.waveType       = wt;
			pkt.fadeoutSub     = fadeoutSub;
			pkt.growbackSub    = growbackSub;
			pkt.preWaveSub     = preWaveSub;
			pkt.phaseRemaining = phaseRemaining;
			pkt.nextRuptureIn  = nextRuptureIn;
			pkt.waveNumber     = nextPhase;

			net->SendPacketToAllClients(g_self, WAVE_STATE_TYPE_TAG,
				reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));

			LOG_DEBUG("Broadcast: phase=%u type=%u wave#=%d ntRem=%.1f sub=%s",
				phase, wt, nextPhase, ntRemaining, sub ? "yes" : "no");
		}

		s_last.phase       = phase;
		s_last.waveType    = wt;
		s_last.fadeoutSub  = fadeoutSub;
		s_last.growbackSub = growbackSub;
		s_last.preWaveSub  = preWaveSub;
		s_last.waveNumber  = nextPhase;
	}
}

// ---------------------------------------------------------------------------
// Plugin exports
// ---------------------------------------------------------------------------
extern "C" {

__declspec(dllexport) PluginInfo* GetPluginInfo() { return &s_pluginInfo; }

__declspec(dllexport) bool PluginInit(IPluginSelf* self)
{
	g_self = self;
	LOG_INFO("RuptureTimerServer initializing...");

	if (!self || !self->hooks)
	{
		LOG_ERROR("hooks interface is null");
		return false;
	}

	auto* hooks = self->hooks;
	if (hooks->World)
	{
		hooks->World->RegisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
		hooks->World->RegisterOnBeforeWorldEndPlay(OnBeforeWorldEndPlay);
	}
	if (hooks->Engine)
		hooks->Engine->RegisterOnTick(OnEngineTick);

	{
		SDK::UWorld* world = SDK::UWorld::GetWorld();
		if (world)
		{
			auto* gs = static_cast<SDK::ACrGameStateBase*>(world->GameState);
			if (gs && gs->WaveTimerActor && FindSubsystem(world))
			{
				LOG_INFO("Active world detected on init — broadcasts enabled immediately");
				s_worldReady = true;
			}
		}
	}

	LOG_INFO("RuptureTimerServer initialized — broadcasting every %.0fs + on phase change", HEARTBEAT);
	return true;
}

__declspec(dllexport) void PluginShutdown()
{
	LOG_INFO("RuptureTimerServer shutting down...");
	s_worldReady = false;
	if (g_self && g_self->hooks)
	{
		auto* hooks = g_self->hooks;
		if (hooks->World)
		{
			hooks->World->UnregisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
			hooks->World->UnregisterOnBeforeWorldEndPlay(OnBeforeWorldEndPlay);
		}
		if (hooks->Engine)
			hooks->Engine->UnregisterOnTick(OnEngineTick);
	}
	g_self = nullptr;
}

} // extern "C"

#endif // MODLOADER_SERVER_BUILD
