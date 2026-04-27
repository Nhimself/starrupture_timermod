#pragma once

#include "timer_tracker.h"
#include "obelisk_tracker.h"

struct IPluginHooks;

namespace HudOverlay
{
	// Register the PostRender callback via hooks->HUD (v16, client only).
	// Returns false if hooks->HUD is null (server build) or registration fails.
	bool Install(IPluginHooks* hooks);

	// Unregister the PostRender callback. Safe to call if Install was never called or failed.
	void Remove(IPluginHooks* hooks);

	// Push a fresh timer state for the overlay to display.
	// Call from the engine tick callback after ReadCurrentState().
	void SetState(const RuptureTimer::TimerState& state);

	// Push a fresh obelisk snapshot for the overlay to display.
	// Pass an invalid (default-constructed) snapshot to clear.
	void SetObeliskState(const ObeliskTracker::Snapshot& snap);
}
