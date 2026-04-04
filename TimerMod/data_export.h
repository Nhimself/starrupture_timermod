#pragma once

#include "timer_tracker.h"

namespace DataExport
{
	// Write timer state to the configured JSON file.
	// Safe to call every tick — throttled internally by UpdateIntervalSeconds config.
	void Update(float deltaSeconds, const RuptureTimer::TimerState& state);

	// Append a diagnostic log line capturing raw game values.
	// Logs on value change + periodic heartbeat (every ~30s).
	// Safe to call every tick — throttled internally. No-op if config disabled.
	void UpdateDiagnosticLog(float deltaSeconds, const RuptureTimer::TimerState& state);

	// Ensure output directory exists. Call once on world begin play.
	void EnsureOutputDir();

	// Ensure diagnostic log directory exists and write CSV header if file is new.
	void EnsureDiagnosticLogDir();
}
