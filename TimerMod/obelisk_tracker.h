#pragma once

#include <cstdint>

namespace ObeliskTracker
{
	// Mirrors SDK::ECrAlienObeliskState (kept independent so callers don't need
	// to pull in the SDK headers).
	enum class State : uint8_t
	{
		Unknown                  = 255,
		Disabled                 = 0,
		Cooldown                 = 1,
		Enabled                  = 2,
		EnabledAndVisiblyCharged = 3,   // charging — attack imminent
		AttackInProgress         = 4,   // wave is active near this Monolith
	};

	struct ObeliskInfo
	{
		State  state;
		float  charge;            // 0.0 .. 1.0 (or whatever the game uses)
		float  posX;
		float  posY;
		float  posZ;
		float  distanceMeters;    // distance to local player; -1 if no pawn yet
		bool   hasPosition;       // false if the bubble item carried no position
	};

	struct Snapshot
	{
		bool   valid;             // false until we've successfully read at least once
		int    totalCount;        // total replicated obelisks
		int    chargingCount;     // EnabledAndVisiblyCharged
		int    attackingCount;    // AttackInProgress

		// Most relevant obelisk to display: prefer AttackInProgress, then
		// EnabledAndVisiblyCharged (closest of those), else Cooldown closest.
		// .state == Unknown when nothing of interest is around.
		ObeliskInfo highlighted;
	};

	// Read a fresh snapshot from the game. Game thread only.
	Snapshot ReadCurrentState();
}
