#include "obelisk_tracker.h"
#include "plugin_helpers.h"

#include <cmath>
#include <cstdint>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "Basic.hpp"
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"
#include "Chimera_structs.hpp"

namespace ObeliskTracker
{

// ---------------------------------------------------------------------------
// The replicated obelisk data lives on ACrMassAlienObeliskClientBubbleInfo —
// one actor per client bubble, holding an array of FCrAlienObeliskFastArrayItem
// entries. Each entry carries position, state, and charge. We resolve the
// bubble actor lazily by scanning GObjects, then re-resolve when the world
// changes.
// ---------------------------------------------------------------------------
struct Cache
{
	SDK::UWorld*                              world  = nullptr;
	SDK::ACrMassAlienObeliskClientBubbleInfo* bubble = nullptr;
};
static Cache s_cache;

static SDK::ACrMassAlienObeliskClientBubbleInfo* FindBubble()
{
	SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
	if (!arr) return nullptr;

	const SDK::UObject* cdo = SDK::ACrMassAlienObeliskClientBubbleInfo::GetDefaultObj();
	for (int i = 0; i < arr->Num(); i++)
	{
		SDK::UObject* obj = arr->GetByIndex(i);
		if (!obj || obj == cdo || !obj->Class) continue;
		if (obj->Class->GetName() == "CrMassAlienObeliskClientBubbleInfo")
			return static_cast<SDK::ACrMassAlienObeliskClientBubbleInfo*>(obj);
	}
	return nullptr;
}

static State MapState(SDK::ECrAlienObeliskState s)
{
	switch (s)
	{
		case SDK::ECrAlienObeliskState::Disabled:                  return State::Disabled;
		case SDK::ECrAlienObeliskState::Cooldown:                  return State::Cooldown;
		case SDK::ECrAlienObeliskState::Enabled:                   return State::Enabled;
		case SDK::ECrAlienObeliskState::EnabledAndVisiblyCharged:  return State::EnabledAndVisiblyCharged;
		case SDK::ECrAlienObeliskState::AttackInProgress:          return State::AttackInProgress;
		default:                                                   return State::Unknown;
	}
}

// Priority for picking which obelisk to highlight. Higher = more important.
static int Priority(State s)
{
	switch (s)
	{
		case State::AttackInProgress:         return 4;
		case State::EnabledAndVisiblyCharged: return 3;
		case State::Cooldown:                 return 2;
		case State::Enabled:                  return 1;
		default:                              return 0;
	}
}

static bool GetLocalPawnLocation(SDK::UWorld* world, SDK::FVector& outLoc)
{
	if (!world || !world->OwningGameInstance) return false;
	auto& players = world->OwningGameInstance->LocalPlayers;
	if (players.Num() == 0) return false;
	SDK::ULocalPlayer* lp = players[0];
	if (!lp) return false;
	SDK::APlayerController* pc = lp->PlayerController;
	if (!pc) return false;
	SDK::APawn* pawn = pc->Pawn;
	if (!pawn) return false;
	outLoc = pawn->K2_GetActorLocation();
	return true;
}

Snapshot ReadCurrentState()
{
	Snapshot snap{};
	snap.highlighted.state = State::Unknown;
	snap.highlighted.distanceMeters = -1.0f;

	SDK::UWorld* world = SDK::UWorld::GetWorld();
	if (!world) return snap;

	if (world != s_cache.world)
	{
		s_cache = Cache{};
		s_cache.world = world;
	}

	if (!s_cache.bubble)
	{
		s_cache.bubble = FindBubble();
		if (!s_cache.bubble) return snap;   // not yet replicated
		LOG_INFO_ONCE("[Obelisk] Found CrMassAlienObeliskClientBubbleInfo");
	}

	auto& items = s_cache.bubble->AlienObeliskSerializer.AlienObelisk;
	const int count = items.Num();
	if (count <= 0) { snap.valid = true; return snap; }

	SDK::FVector pawnLoc{};
	const bool havePawn = GetLocalPawnLocation(world, pawnLoc);

	int  bestIdx       = -1;
	int  bestPriority  = 0;       // 0 = nothing interesting (Disabled / Unknown)
	float bestDistance = 1e30f;

	for (int i = 0; i < count; i++)
	{
		const SDK::FCrAlienObeliskFastArrayItem& it = items[i];
		const State st = MapState(it.Agent.AlienObeliskData.State);

		if (st == State::EnabledAndVisiblyCharged) snap.chargingCount++;
		if (st == State::AttackInProgress)         snap.attackingCount++;

		const int prio = Priority(st);
		if (prio == 0) continue;

		float dist = -1.0f;
		if (havePawn)
		{
			const float dx = (float)(it.Agent.PositionYaw.Position.X - pawnLoc.X);
			const float dy = (float)(it.Agent.PositionYaw.Position.Y - pawnLoc.Y);
			const float dz = (float)(it.Agent.PositionYaw.Position.Z - pawnLoc.Z);
			dist = std::sqrt(dx*dx + dy*dy + dz*dz) * 0.01f; // UE units → meters
		}

		// Pick by priority first, distance as tiebreaker.
		const bool better = (prio > bestPriority) ||
		                    (prio == bestPriority && dist >= 0.0f && dist < bestDistance);
		if (better)
		{
			bestIdx       = i;
			bestPriority  = prio;
			bestDistance  = (dist >= 0.0f) ? dist : bestDistance;
		}
	}

	snap.valid      = true;
	snap.totalCount = count;

	if (bestIdx >= 0)
	{
		const SDK::FCrAlienObeliskFastArrayItem& it = items[bestIdx];
		ObeliskInfo& o = snap.highlighted;
		o.state          = MapState(it.Agent.AlienObeliskData.State);
		o.charge         = it.Agent.AlienObeliskData.ChargeValue;
		o.posX           = (float)it.Agent.PositionYaw.Position.X;
		o.posY           = (float)it.Agent.PositionYaw.Position.Y;
		o.posZ           = (float)it.Agent.PositionYaw.Position.Z;
		o.hasPosition    = true;
		o.distanceMeters = (bestDistance < 1e29f) ? bestDistance : -1.0f;
	}

	return snap;
}

} // namespace ObeliskTracker
