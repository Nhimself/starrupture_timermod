#pragma once

#include <cstdint>
#include "wave_packet.h"

namespace RuptureTimer
{
	// Maps EEnviroWaveStage to user-friendly names
	// None=Stable, PreWave=Warning, Moving=Burning, Fadeout=Cooling, Growback=Stabilizing
	enum class RupturePhase : uint8_t
	{
		Unknown      = 0,
		Stable       = 1,   // Waiting for next rupture (EEnviroWaveStage::None)
		Warning      = 2,   // 15-second pre-wave warning (EEnviroWaveStage::PreWave)
		Burning      = 3,   // Wave moving across map - DANGEROUS (EEnviroWaveStage::Moving)
		Cooling      = 4,   // Aftermath / fadeout (EEnviroWaveStage::Fadeout)
		Stabilizing  = 5,   // Safe gathering window (EEnviroWaveStage::Growback)
	};

	// Raw diagnostic data captured during ReadCurrentState().
	// Used by the diagnostic log to record exactly what the game reported.
	struct DiagnosticRawData
	{
		const char*  codePath;              // "subsystem", "repActor", "stateMachine", "none"
		int          rawStage;              // EEnviroWaveStage as int (-1 if not available)
		int          rawWaveType;           // EEnviroWave as int (-1 if not available)
		float        rawNextTime;           // ACrWaveTimerActor::NextTime (absolute server timestamp)
		double       rawServerTime;         // GetServerWorldTimeSeconds()
		float        rawNextTimeRemaining;  // NextTime - serverTime, unclamped (negative = stale)
		int32_t      rawNextPhase;          // ACrWaveTimerActor::NextPhase
		bool         rawPaused;             // ACrWaveTimerActor::bPause
		bool         hasRepActor;           // was ACrGatherableSpawnersRepActor found?
		bool         hasSubsystem;          // was UCrEnviroWaveSubsystem found?
		float        rawProgress;           // GetCurrentStageProgress() (-1 if not available)
		// Hex dump of repActor memory at offsets 0x02A8..0x02B0 (8 bytes)
		// Allows detecting offset misalignment without recompiling.
		uint8_t      repActorBytes[8];      // raw bytes at repActor+0x02A8, zeroed if no repActor
		bool         repActorBytesValid;    // true if repActorBytes was populated

		// Game-internal phase name — what the engine actually reports, not our
		// interpretation.  Populated by subsystem and repActor paths:
		//   subsystem/repActor: EEnviroWaveStage name ("None","PreWave","Moving","Fadeout","Growback")
		//   stateMachine:       NextPhase label       ("NP0=Stable","NP1=PreWave","NP2=Moving","NP3=PostWave")
		const char*  rawPhaseName;

		// Substage fields — populated by the subsystem path only.
		// Allow us to see what the game internally calls each sub-step of
		// Fadeout (FireWave/Burning/Fading), Growback (MoonPhase/RegrowthStart/Regrowth),
		// and PreWave (BeforeExplosion/AfterExplosion).
		// -1 means the stage is not active or the subsystem is absent.
		int          rawFadeoutSubstage;    // EEnviroWaveFadeoutSubstage as int (-1 if not applicable)
		int          rawGrowbackSubstage;   // EEnviroWaveGrowbackSubstage as int (-1 if not applicable)
		int          rawPreWaveSubstage;    // EEnviroWavePreWaveSubstage as int (-1 if not applicable)
		const char*  rawSubstageName;       // human-readable name of the active substage (or "None")
	};

	struct TimerState
	{
		bool         valid;                  // false if world/game state not ready
		RupturePhase phase;
		const char*  phaseName;             // "Stable", "Warning", "Burning", "Cooling", "Stabilizing"
		float        phaseRemainingSeconds; // time left in the current phase (-1.0f = unknown)
		float        nextRuptureInSeconds;  // seconds until next Burning phase starts (the primary display value)
		                                    // 0 = currently in rupture; -1 = unknown
		int32_t      waveNumber;            // how many ruptures have completed (NextPhase)
		bool         paused;                // timer paused on server
		uint8_t      waveType;             // 0=None, 1=Heat, 2=Cold
		const char*  waveTypeName;         // "None", "Heat", "Cold"

		// Per-phase timing breakdown.
		// -1.0f = not available (dedicated server client) or not yet scheduled (future phases).
		// 0.0f  = this phase has already passed in the current cycle.
		// Populated in full mode (local / listen-server) only.
		float        warningRemaining;      // time left in Warning phase
		float        burningRemaining;      // time left in Burning phase
		float        coolingRemaining;      // time left in Cooling phase
		float        stabilizingRemaining;  // time left in Stabilizing phase
		float        stableRemaining;       // duration of the next Stable period

		DiagnosticRawData diag;             // raw values for diagnostic logging
	};

	// Read current rupture timer state from the game. Call only from game thread.
	TimerState ReadCurrentState();

	// Store a network-replicated wave state received from the server-side plugin.
	// Must be called from the game thread. ReadCurrentState() will use this as a
	// fallback when UCrEnviroWaveSubsystem and ACrGatherableSpawnersRepActor are absent.
	void SetNetworkState(const WaveStatePacket& pkt);
}
