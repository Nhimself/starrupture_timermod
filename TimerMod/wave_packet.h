#pragma once
#include <cstdint>

// Type tag used by IPluginNetworkChannel — must match on server and client.
#define WAVE_STATE_TYPE_TAG "wave_state"

// Packed wire format broadcast by the server plugin to all clients.
// Little-endian; 20 bytes.
#pragma pack(push, 1)
struct WaveStatePacket
{
	uint8_t  phase;           // RupturePhase enum value (0=Unknown … 5=Stabilizing)
	uint8_t  waveType;        // 0=None, 1=Heat, 2=Cold
	int8_t   fadeoutSub;      // EEnviroWaveFadeoutSubstage as int, -1=N/A
	int8_t   growbackSub;     // EEnviroWaveGrowbackSubstage as int, -1=N/A
	int8_t   preWaveSub;      // EEnviroWavePreWaveSubstage as int, -1=N/A
	uint8_t  _pad[3];
	float    phaseRemaining;  // seconds left in current phase; -1.0f = unknown
	float    nextRuptureIn;   // seconds until next Burning; 0 = in rupture; -1 = unknown
	int32_t  waveNumber;      // ACrWaveTimerActor::NextPhase
};
#pragma pack(pop)
static_assert(sizeof(WaveStatePacket) == 20, "WaveStatePacket size mismatch");
