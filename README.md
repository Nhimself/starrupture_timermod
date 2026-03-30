# RuptureTimer

A [StarRupture Mod Loader](https://github.com/AlienXAXS/StarRupture-Plugin-SDK) plugin that tracks the rupture wave timer and makes the data available in two ways:

- **JSON export** -- writes a file every second that external tools (StreamDeck, overlays, scripts) can read.
- **In-game HUD overlay** -- optional plain-text overlay drawn directly on screen showing the three key values at a glance.

Built using the [StarRupture Plugin SDK](https://github.com/AlienXAXS/StarRupture-Plugin-SDK).

---

## What it tracks

| Value | Description |
|-------|-------------|
| **Next Rupture** | Countdown until the next wave hits. Shows `NOW` while a wave is active. |
| **Planet Status** | Current phase: Stable, Warning, Burning, Cooling, or Stabilizing. Includes wave type (Heat / Cold) when active. |
| **Wave Timer** | Time remaining in the current phase. |

Works across all connection types:

- **Local / listen server** -- full accuracy, reads phase progress directly from the game.
- **Dedicated server client** -- phase name from replicated actor; timing from server timestamp.
- **Fallback** -- phase inferred from observable jumps in the server timestamp.

---

## Installation

### Prerequisites

You need the [StarRupture Mod Loader](https://github.com/AlienXAXS/StarRupture-Plugin-SDK) installed. Download `dwmapi.dll` from the mod loader's releases and place it in:

```
steamapps/common/StarRupture/StarRupture/Binaries/Win64/
```

### Installing the plugin

1. Download `TimerMod.dll` from the [latest release](../../releases/latest).
2. Place it in:

```
steamapps/common/StarRupture/StarRupture/Binaries/Win64/Plugins/
```

3. Launch the game. On first run the mod loader generates a config file.

---

## Configuration

Config file location:

```
Plugins/config/RuptureTimer.ini
```

### [General]

```ini
[General]
; Enable or disable the plugin entirely
Enabled=1
```

### [Export] -- JSON file for external tools

```ini
[Export]
; Write timer state to a JSON file (set to 0 to disable)
WriteJsonFile=1

; Path to the output file, relative to the game directory
JsonFilePath=Plugins/data/rupture_timer.json

; How often the file is updated (seconds, minimum 0.1)
UpdateIntervalSeconds=1.0

; Include per-phase breakdown timers in the JSON output.
; Only populated in full mode (local / listen server).
ExtendedPhaseTimers=0
```

#### JSON output

Standard (`ExtendedPhaseTimers=0`):

```json
{
  "valid": true,
  "phase": "Stable",
  "phase_remaining_sec": 2550.0,
  "next_rupture_in_sec": 2550.0,
  "wave_number": 3,
  "wave_type": "None",
  "paused": false
}
```

Extended (`ExtendedPhaseTimers=1`) adds: `warning_remaining_sec`, `burning_remaining_sec`, `cooling_remaining_sec`, `stabilizing_remaining_sec`, `stable_remaining_sec`. Values are `null` when unknown.

**Phase values:** `Stable` | `Warning` | `Burning` | `Cooling` | `Stabilizing` | `Unknown`

**Wave type values:** `None` | `Heat` | `Cold`

### [HUD] -- In-game text overlay

```ini
[HUD]
; Show the rupture timer as a text overlay (0 = off, 1 = on)
ShowOverlay=0

; Anchor position: TopLeft, TopMid, TopRight, MidLeft, MidRight, LowerLeft, LowerRight
Position=LowerLeft

; Text scale multiplier (1.0 = default)
Scale=1.0
```

The overlay displays:

```
Next Rupture: 42:30
Planet: Stable
Wave Timer: 42:30
```

---

## Building from source

### Requirements

- Visual Studio 2022 (v143 toolset)
- Windows 10 SDK

### Steps

```bash
git clone --recurse-submodules <this-repo-url>
```

Open `StarRupture-TimerMod.sln` in Visual Studio and build the **Client Release | x64** configuration.

The compiled DLL is output to `bin/x64/Client Release/plugins/TimerMod.dll`.

---

## Credits

- **Nhimself** -- RuptureTimer plugin
- **AlienXAXS** -- [StarRupture Mod Loader & Plugin SDK](https://github.com/AlienXAXS/StarRupture-Plugin-SDK)
