#include "hud_overlay.h"
#include "plugin_config.h"
#include "plugin_helpers.h"

// Win32 headers define DrawText as a macro (DrawTextW/DrawTextA depending on
// the character-set setting). Undefine it BEFORE including Engine_classes.hpp
// so the preprocessor does not rename AHUD::DrawText to AHUD::DrawTextW inside
// the class definition itself.
#ifdef DrawText
#undef DrawText
#endif

#include "Engine_classes.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace HudOverlay
{

// ---------------------------------------------------------------------------
// Threading model — IMPORTANT
//
// OnPostRender() is invoked by the engine HUD render path, which the modloader
// documents as potentially running on a DIFFERENT thread than the game tick.
// SetState() is called from the game thread (engine tick / experience-load).
//
// Consequences this code must defend against:
//   1. SetState() (game thread) and OnPostRender() (render thread) touch shared
//      state concurrently → guarded by s_mutex.
//   2. On disable / reload / shutdown, PluginShutdown() runs on the game thread,
//      calls Remove(), and the modloader then FreeLibrary()s this DLL. If the
//      render thread is inside OnPostRender() at that moment, the code/data it
//      uses is unmapped → EXCEPTION_ACCESS_VIOLATION. Remove() therefore drains
//      any in-flight render before returning (see Remove()).
//   3. The render thread must never dereference a pointer that lives in this
//      DLL (string literals) or call back into the modloader (s_self->config),
//      because those are gone after unload. OnPostRender() therefore consumes
//      ONLY a value-type snapshot whose strings are inline char buffers and
//      whose config values were captured on the game thread.
// ---------------------------------------------------------------------------

// Value-only snapshot consumed by the render thread. Contains no pointers into
// this DLL and no live config handles — safe to read after disposal has begun.
struct HudSnapshot
{
	bool                       valid = false;
	RuptureTimer::RupturePhase phase = RuptureTimer::RupturePhase::Unknown;

	float nextRuptureInSeconds  = -1.0f;
	float phaseRemainingSeconds = -1.0f;
	float stableRemaining       = -1.0f;
	float warningRemaining      = -1.0f;
	float burningRemaining      = -1.0f;
	float coolingRemaining      = -1.0f;
	float stabilizingRemaining  = -1.0f;

	int32_t waveNumber = 0;
	bool    paused     = false;
	uint8_t waveType   = 0;

	char phaseName[16]    = "Unknown";
	char waveTypeName[8]  = "None";

	// Diagnostic fields used by the ShowDebugInfo lines only.
	int  rawStage            = -1;
	int  rawFadeoutSubstage  = -1;
	int  rawGrowbackSubstage = -1;
	int  rawPreWaveSubstage  = -1;
	char codePath[16]        = "none";
	char rawSubstageName[24] = "None";

	// Config values captured on the game thread (never read from s_self here).
	bool  cfgShowOverlay = false;
	float cfgScale       = 1.0f;
	bool  cfgExtended    = false;
	bool  cfgDebugInfo   = false;
	char  cfgPosition[64] = "LowerLeft";
};

// Shared snapshot — written by SetState() (game thread), read by
// OnPostRender() (render thread). s_mutex guards ONLY the snapshot copy and is
// never held across an engine DrawText call (doing so can deadlock against
// UE's render/game-thread fences). s_installed is the fast-path gate.
// s_renderActive counts OnPostRender bodies currently in flight so Remove()
// can drain them before the DLL is unloaded, without holding a lock across
// the engine.
static std::mutex          s_mutex;
static HudSnapshot         s_snapshot;
static std::atomic<bool>   s_installed{false};
static std::atomic<int>    s_renderActive{0};

// Copy a C string into a fixed buffer with guaranteed null-termination.
template <size_t N>
static void CopyStr(char (&dst)[N], const char* src)
{
	if (!src) { dst[0] = '\0'; return; }
	_snprintf_s(dst, N, _TRUNCATE, "%s", src);
}

// ---------------------------------------------------------------------------
// Smooth display values — interpolated at wall-clock rate in OnPostRender.
//
// Problem: GetServerWorldTimeSeconds() is periodically corrected by the server.
// Each correction causes nextRuptureInSeconds (= NextTime - serverTime) to
// jump by the correction delta, which looks like a visible snap on the HUD.
//
// Fix: maintain locally-interpolated display values that count down at real
// wall-clock rate between server updates.  Only snap to the server value when
// the discrepancy is larger than SNAP_THRESHOLD (catches genuine phase
// transitions like Stable→Warning which change remaining by hundreds of
// seconds) or when the phase itself changes.
//
// SNAP_THRESHOLD: server-clock corrections are typically < 2 s in UE5 net
// play.  10 s absorbs those and any QPC drift over long stable periods (the
// stable phase runs ~45 min) while still catching legitimate phase-change
// jumps, which are always >= 30 s.
//
// These statics are touched ONLY by OnPostRender (render thread, single
// threaded for HUD), so they need no lock. They are reset in Install() so a
// re-enable starts from a clean slate.
// ---------------------------------------------------------------------------
static constexpr float SNAP_THRESHOLD = 10.0f;

static float s_dispNextRup   = -1.0f;
static float s_dispPhaseRem  = -1.0f;
static float s_dispStableRem = -1.0f;

static RuptureTimer::RupturePhase s_prevPhase = RuptureTimer::RupturePhase::Unknown;

// Last world name seen by OnPostRender (render-thread only). Used to gate the
// overlay to the actual game world and to log world changes once.
static std::string s_lastWorldName;

static LARGE_INTEGER s_qpcFreq      = {};
static LARGE_INTEGER s_lastQpcTime  = {};
static bool          s_qpcReady     = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Format seconds as "M:SS" (e.g. 2550 → "42:30"). Returns "--:--" for
// values < 0 (unknown) and "NOW" when seconds == 0 and isNow is true.
static void FormatTime(char* buf, int bufSize, float seconds, bool nowOnZero = false)
{
	if (seconds < 0.0f)
	{
		_snprintf_s(buf, bufSize, _TRUNCATE, "--:--");
		return;
	}
	if (nowOnZero && seconds < 0.5f)
	{
		_snprintf_s(buf, bufSize, _TRUNCATE, "NOW");
		return;
	}
	int total = static_cast<int>(seconds);
	int m     = total / 60;
	int s     = total % 60;
	_snprintf_s(buf, bufSize, _TRUNCATE, "%d:%02d", m, s);
}

// Draw a single line of text with a drop shadow for readability over any
// background. Uses AHUD::DrawText with default engine font (Font = nullptr).
static void DrawLine(SDK::AHUD* hud, float x, float y, float scale, const char* text)
{
	// Convert ASCII to wide string — our text is always plain ASCII digits/letters.
	wchar_t wbuf[128] = {};
	for (int i = 0; text[i] && i < 126; ++i)
		wbuf[i] = static_cast<wchar_t>(text[i]);

	SDK::FString fs(wbuf);
	SDK::FLinearColor shadow{0.0f, 0.0f, 0.0f, 0.75f};
	SDK::FLinearColor white{1.0f, 1.0f, 1.0f, 1.0f};

	// Shadow pass (offset 1 pixel)
	hud->DrawText(fs, shadow, x + 1.0f, y + 1.0f, nullptr, scale, false);
	// Main pass
	hud->DrawText(fs, white,  x,         y,         nullptr, scale, false);
}

// ---------------------------------------------------------------------------
// Position calculation
//
// Anchor names map to 7 points around the screen edge. Text is always
// rendered left-to-right from the calculated origin x/y.
//
// estimatedBlockW — approximate pixel width of the widest text line at
//                   scale 1.0 (default font ~8px/char, longest line ~24 chars).
// lineH           — approximate line height including spacing.
// ---------------------------------------------------------------------------
static constexpr float MARGIN             = 20.0f;
static constexpr float ESTIMATED_CHAR_W  = 12.0f; // pixels per char at scale 1.0 (UE default HUD font)
static constexpr int   LONGEST_LINE_CHARS = 28;   // "Planet: Stabilizing (Heat)" = 26 + buffer
static constexpr float LINE_H_BASE        = 20.0f; // px per line at scale 1.0

static void CalcPosition(const char* posName, float scale,
                         float screenW, float screenH,
                         int lineCount,
                         float& outX, float& outY)
{
	const float margin    = MARGIN * scale;
	const float blockW    = ESTIMATED_CHAR_W * LONGEST_LINE_CHARS * scale;
	const float blockH    = LINE_H_BASE * static_cast<float>(lineCount) * scale;

	// Default: lower-left
	outX = margin;
	outY = screenH - margin - blockH;

	if (_stricmp(posName, "LowerLeft") == 0)
	{
		outX = margin;
		outY = screenH - margin - blockH;
	}
	else if (_stricmp(posName, "MidLeft") == 0)
	{
		outX = margin;
		outY = screenH * 0.5f - blockH * 0.5f;
	}
	else if (_stricmp(posName, "TopLeft") == 0)
	{
		outX = margin;
		outY = margin;
	}
	else if (_stricmp(posName, "TopMid") == 0)
	{
		outX = screenW * 0.5f - blockW * 0.5f;
		outY = margin;
	}
	else if (_stricmp(posName, "TopRight") == 0)
	{
		outX = screenW - margin - blockW;
		outY = margin;
	}
	else if (_stricmp(posName, "MidRight") == 0)
	{
		outX = screenW - margin - blockW;
		outY = screenH * 0.5f - blockH * 0.5f;
	}
	else if (_stricmp(posName, "LowerRight") == 0)
	{
		outX = screenW - margin - blockW;
		outY = screenH - margin - blockH;
	}
}

// ---------------------------------------------------------------------------
// PostRender callback — registered via hooks->HUD->RegisterOnPostRender (v16).
// Runs on the render thread. See the threading-model note at the top.
// ---------------------------------------------------------------------------

static void OnPostRender(void* hudPtr)
{
	// Fast-path gate: if disposal has started, bail before touching anything.
	if (!s_installed.load(std::memory_order_acquire))
		return;

	auto* self = static_cast<SDK::AHUD*>(hudPtr);
	if (!self || !self->Canvas)
		return;

	// Mark this render in flight so Remove() can drain it. RAII so every
	// early-return path below decrements. Increment FIRST, then re-check the
	// gate: this orders against Remove() (clears gate, then waits for the
	// count to reach zero).
	s_renderActive.fetch_add(1, std::memory_order_acq_rel);
	struct ActiveGuard
	{
		~ActiveGuard() { s_renderActive.fetch_sub(1, std::memory_order_acq_rel); }
	} activeGuard;

	if (!s_installed.load(std::memory_order_acquire))
		return;

	// Only draw in the actual game world. GetWorld()/GetName() touch engine
	// globals from the render thread, so wrap them (per modloader guidance).
	SDK::UWorld* world = nullptr;
	try
	{
		world = SDK::UWorld::GetWorld();
	}
	catch (...)
	{
		LOG_ERROR("[HudOverlay] exception in GetWorld — skipping draw");
		return;
	}
	if (!world)
		return;

	std::string worldName;
	try
	{
		worldName = world->GetName();
	}
	catch (...)
	{
		LOG_ERROR("[HudOverlay] exception in GetName — skipping draw");
		return;
	}
	if (worldName != s_lastWorldName)
	{
		LOG_INFO("[HudOverlay] world: '%s'", worldName.c_str());
		s_lastWorldName = worldName;
	}
	if (worldName != "ChimeraMain")
		return;

	// Copy the shared snapshot under the lock, then release it. The lock is
	// NOT held across any DrawText call — only across this trivial copy.
	HudSnapshot snap;
	{
		std::lock_guard<std::mutex> lk(s_mutex);
		snap = s_snapshot;
	}

	if (!snap.valid)          return;
	if (!snap.cfgShowOverlay) return;

	// -----------------------------------------------------------------------
	// Step 1 — compute wall-clock frame delta via QPC.
	// -----------------------------------------------------------------------
	LARGE_INTEGER qpcNow;
	QueryPerformanceCounter(&qpcNow);

	float dt = 0.0f;
	if (!s_qpcReady)
	{
		QueryPerformanceFrequency(&s_qpcFreq);
		s_qpcReady    = true;
		s_lastQpcTime = qpcNow;
	}
	else if (s_qpcFreq.QuadPart > 0)
	{
		dt = static_cast<float>(qpcNow.QuadPart - s_lastQpcTime.QuadPart)
		     / static_cast<float>(s_qpcFreq.QuadPart);
		// Guard against stalls / debugger pauses warping the display.
		if (dt > 2.0f) dt = 0.0f;
	}
	s_lastQpcTime = qpcNow;

	// -----------------------------------------------------------------------
	// Step 2 — tick display values down at wall-clock rate.
	// -----------------------------------------------------------------------
	if (s_dispNextRup   >= 0.0f) s_dispNextRup   -= dt;
	if (s_dispPhaseRem  >= 0.0f) s_dispPhaseRem  -= dt;
	if (s_dispStableRem >= 0.0f) s_dispStableRem -= dt;

	// -----------------------------------------------------------------------
	// Step 3 — sync from server when needed.
	//   • Phase changed            → always snap (new cycle, values are stale).
	//   • Display uninitialised    → snap.
	//   • Discrepancy > threshold  → snap (genuine phase-transition jump).
	//   • Small discrepancy        → ignore; local interpolation is smoother.
	// -----------------------------------------------------------------------
	bool phaseChanged = (snap.phase != s_prevPhase);
	s_prevPhase = snap.phase;

	auto Sync = [phaseChanged](float& disp, float srv)
	{
		if (srv < 0.0f)          { disp = srv; return; } // unknown → pass through
		if (disp < 0.0f)         { disp = srv; return; } // uninitialised
		if (phaseChanged)        { disp = srv; return; } // phase flip
		if (disp - srv > SNAP_THRESHOLD ||
		    srv - disp > SNAP_THRESHOLD) { disp = srv; } // large jump
		// else: keep locally interpolated value
	};

	Sync(s_dispNextRup,   snap.nextRuptureInSeconds);
	Sync(s_dispPhaseRem,  snap.phaseRemainingSeconds);
	Sync(s_dispStableRem, snap.stableRemaining);

	if (s_dispNextRup   < 0.0f) s_dispNextRup   = 0.0f;
	if (s_dispPhaseRem  < 0.0f) s_dispPhaseRem  = 0.0f;
	if (s_dispStableRem < 0.0f) s_dispStableRem = 0.0f;

	SDK::UCanvas* canvas = self->Canvas;
	const float screenW = static_cast<float>(canvas->SizeX);
	const float screenH = static_cast<float>(canvas->SizeY);
	if (screenW <= 0.0f || screenH <= 0.0f)
		return;

	const float scale = snap.cfgScale;
	const float lineH = LINE_H_BASE * scale;

	const bool extended  = snap.cfgExtended;
	const bool debugInfo = snap.cfgDebugInfo;

	// Count active extended lines (only for non-Stable phases)
	int extendedLines = 0;
	if (extended && snap.phase != RuptureTimer::RupturePhase::Stable)
	{
		if (snap.warningRemaining     >= 0.0f) extendedLines++;
		if (snap.burningRemaining     >= 0.0f) extendedLines++;
		if (snap.coolingRemaining     >= 0.0f) extendedLines++;
		if (snap.stabilizingRemaining >= 0.0f) extendedLines++;
		if (snap.stableRemaining      >= 0.0f) extendedLines++;
	}

	const int debugLines = debugInfo ? 3 : 0;
	const int totalLines = 3 + extendedLines + debugLines;

	float x, y;
	CalcPosition(snap.cfgPosition, scale, screenW, screenH, totalLines, x, y);

	float curY = y;

	// --- Line 1: Next Rupture countdown ---
	char nextBuf[16];
	FormatTime(nextBuf, sizeof(nextBuf), snap.nextRuptureInSeconds >= 0.0f ? s_dispNextRup : -1.0f, /*nowOnZero=*/true);

	char line1[48];
	_snprintf_s(line1, sizeof(line1), _TRUNCATE, "Next Rupture: %s", nextBuf);
	DrawLine(self, x, curY, scale, line1);
	curY += lineH;

	// --- Line 2: Planet status (phase + wave type if active) ---
	char line2[48];
	const bool waveActive = (snap.waveType != 0); // 0 = None
	if (waveActive)
		_snprintf_s(line2, sizeof(line2), _TRUNCATE, "Planet: %s (%s)", snap.phaseName, snap.waveTypeName);
	else
		_snprintf_s(line2, sizeof(line2), _TRUNCATE, "Planet: %s", snap.phaseName);
	DrawLine(self, x, curY, scale, line2);
	curY += lineH;

	// --- Line 3: Current phase timer ---
	char phaseBuf[16];
	FormatTime(phaseBuf, sizeof(phaseBuf), snap.phaseRemainingSeconds >= 0.0f ? s_dispPhaseRem : -1.0f);

	char line3[48];
	_snprintf_s(line3, sizeof(line3), _TRUNCATE, "Wave Timer: %s", phaseBuf);
	DrawLine(self, x, curY, scale, line3);
	curY += lineH;

	// --- Extended phase breakdown (ExtendedPhaseTimers=true, non-Stable phases only) ---
	if (extended && snap.phase != RuptureTimer::RupturePhase::Stable)
	{
		char buf[48];
		char tbuf[16];

		if (snap.warningRemaining >= 0.0f)
		{
			FormatTime(tbuf, sizeof(tbuf), snap.warningRemaining);
			_snprintf_s(buf, sizeof(buf), _TRUNCATE, "  Warning:     %s", tbuf);
			DrawLine(self, x, curY, scale, buf);
			curY += lineH;
		}
		if (snap.burningRemaining >= 0.0f)
		{
			FormatTime(tbuf, sizeof(tbuf), snap.burningRemaining);
			_snprintf_s(buf, sizeof(buf), _TRUNCATE, "  Burning:     %s", tbuf);
			DrawLine(self, x, curY, scale, buf);
			curY += lineH;
		}
		if (snap.coolingRemaining >= 0.0f)
		{
			FormatTime(tbuf, sizeof(tbuf), snap.coolingRemaining);
			_snprintf_s(buf, sizeof(buf), _TRUNCATE, "  Cooling:     %s", tbuf);
			DrawLine(self, x, curY, scale, buf);
			curY += lineH;
		}
		if (snap.stabilizingRemaining >= 0.0f)
		{
			FormatTime(tbuf, sizeof(tbuf), snap.stabilizingRemaining);
			_snprintf_s(buf, sizeof(buf), _TRUNCATE, "  Stabilizing: %s", tbuf);
			DrawLine(self, x, curY, scale, buf);
			curY += lineH;
		}
		if (snap.stableRemaining >= 0.0f)
		{
			FormatTime(tbuf, sizeof(tbuf), s_dispStableRem);
			_snprintf_s(buf, sizeof(buf), _TRUNCATE, "  Stable:      %s", tbuf);
			DrawLine(self, x, curY, scale, buf);
			curY += lineH;
		}
	}

	// --- Debug info lines (ShowDebugInfo=true) ---
	if (debugInfo)
	{
		char dbg1[80];
		_snprintf_s(dbg1, sizeof(dbg1), _TRUNCATE, "[Wave:%d RawStage:%d Path:%s]",
			snap.waveNumber,
			snap.rawStage,
			snap.codePath);
		DrawLine(self, x, curY, scale, dbg1);
		curY += lineH;

		char dbg2[64];
		_snprintf_s(dbg2, sizeof(dbg2), _TRUNCATE, "[PhRem:%.1f Rup:%.1f %s]",
			snap.phaseRemainingSeconds,
			snap.nextRuptureInSeconds,
			snap.paused ? "PAUSED" : "");
		DrawLine(self, x, curY, scale, dbg2);
		curY += lineH;

		// Substage line — populated only in subsystem path; shows "None" otherwise.
		// Compare against what the in-game UI displays to find name mismatches.
		char dbg3[80];
		_snprintf_s(dbg3, sizeof(dbg3), _TRUNCATE, "[Substage:%s FO:%d GB:%d PW:%d]",
			snap.rawSubstageName,
			snap.rawFadeoutSubstage,
			snap.rawGrowbackSubstage,
			snap.rawPreWaveSubstage);
		DrawLine(self, x, curY, scale, dbg3);
	}
}

// ---------------------------------------------------------------------------
// Install / Remove
// ---------------------------------------------------------------------------

bool Install(IPluginHooks* hooks)
{
	if (!hooks || !hooks->HUD)
	{
		LOG_DEBUG("[HudOverlay] hooks->HUD not available — overlay disabled (server build or pre-v16 loader)");
		return false;
	}

	// Reset render-thread display state and shared snapshot so a re-enable
	// within the same DLL load starts from a clean slate.
	{
		std::lock_guard<std::mutex> lk(s_mutex);
		s_snapshot = HudSnapshot{};
	}
	s_dispNextRup   = -1.0f;
	s_dispPhaseRem  = -1.0f;
	s_dispStableRem = -1.0f;
	s_prevPhase     = RuptureTimer::RupturePhase::Unknown;
	s_qpcReady      = false;
	s_lastWorldName.clear();

	s_installed.store(true, std::memory_order_release);
	hooks->HUD->RegisterOnPostRender(OnPostRender);
	LOG_INFO("[HudOverlay] PostRender callback registered via v16 HUD interface");
	return true;
}

void Remove(IPluginHooks* hooks)
{
	if (!hooks || !hooks->HUD)
		return;

	// Order matters for disposal safety:
	//   1. Clear the gate so a render that has not yet entered bails fast.
	//   2. Unregister so the modloader dispatches no new renders.
	//   3. Drain: wait until no OnPostRender body is in flight. We never hold
	//      s_mutex across an engine DrawText call, so this spin cannot deadlock
	//      against the engine's render/game-thread fences. Once the count is
	//      zero and the gate is cleared + unregistered, it is safe for the
	//      modloader to FreeLibrary() this DLL.
	//
	// Residual: a render the modloader dispatched but has not yet entered
	// (before its first instruction) cannot be fenced from the plugin side;
	// closing that fully requires the loader to quiesce the render thread in
	// UnregisterOnPostRender. This drains every realistic in-flight case.
	s_installed.store(false, std::memory_order_release);
	hooks->HUD->UnregisterOnPostRender(OnPostRender);
	for (int i = 0; s_renderActive.load(std::memory_order_acquire) != 0; ++i)
	{
		if (i >= 5000) // ~5 s; far beyond one HUD frame — give up rather than hang.
		{
			LOG_ERROR("[HudOverlay] render drain timed out — proceeding (rare unload race possible)");
			break;
		}
		::Sleep(1);
	}
	LOG_INFO("[HudOverlay] PostRender callback unregistered and render thread drained");
}

void SetState(const RuptureTimer::TimerState& state)
{
	// Called on the game thread. Build a value-only snapshot (copying strings
	// into inline buffers) and capture config here so the render thread never
	// dereferences a DLL pointer or calls back into the modloader.
	HudSnapshot s;
	s.valid                 = state.valid;
	s.phase                 = state.phase;
	s.nextRuptureInSeconds  = state.nextRuptureInSeconds;
	s.phaseRemainingSeconds = state.phaseRemainingSeconds;
	s.stableRemaining       = state.stableRemaining;
	s.warningRemaining      = state.warningRemaining;
	s.burningRemaining      = state.burningRemaining;
	s.coolingRemaining      = state.coolingRemaining;
	s.stabilizingRemaining  = state.stabilizingRemaining;
	s.waveNumber            = state.waveNumber;
	s.paused                = state.paused;
	s.waveType              = state.waveType;
	CopyStr(s.phaseName,    state.phaseName);
	CopyStr(s.waveTypeName, state.waveTypeName);
	s.rawStage              = state.diag.rawStage;
	s.rawFadeoutSubstage    = state.diag.rawFadeoutSubstage;
	s.rawGrowbackSubstage   = state.diag.rawGrowbackSubstage;
	s.rawPreWaveSubstage    = state.diag.rawPreWaveSubstage;
	CopyStr(s.codePath,        state.diag.codePath);
	CopyStr(s.rawSubstageName, state.diag.rawSubstageName);

	s.cfgShowOverlay = RuptureTimerConfig::Config::ShouldShowOverlay();
	s.cfgScale       = RuptureTimerConfig::Config::GetOverlayScale();
	s.cfgExtended    = RuptureTimerConfig::Config::ShouldWriteExtendedPhaseTimers();
	s.cfgDebugInfo   = RuptureTimerConfig::Config::ShouldShowDebugInfo();
	CopyStr(s.cfgPosition, RuptureTimerConfig::Config::GetOverlayPosition());

	std::lock_guard<std::mutex> lk(s_mutex);
	s_snapshot = s;
}

} // namespace HudOverlay
