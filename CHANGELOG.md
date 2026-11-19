# Changelog (Fork)

Developer-oriented summary of changes in this fork relative to upstream `LaptopTrueHDR`.

## Calibration & Precision

- **`0.1`-nit slider granularity (`main.cpp`)**
  - Replaced integer nit conversion (`LevelToNits` / `NitsToLevel`) with tenths-of-a-nit (`LevelToTenths` / `TenthsToLevel`) on `DISPLAYCONFIG_SDR_WHITE_LEVEL` (raw unit = `0.08` nits, so `0.1`-nit steps are injective and round-trip cleanly).
  - Updated `SelfCheck()` to verify round-trip fidelity across `80.0–400.0` nits (`800–4000` tenths).
  - Updated `Anchor::nits` and `InterpolateHdrNits` to `double` rounded to `0.1` nits; `profile.json` writes `%.1f` while remaining backward-compatible with integer profiles.
  - Updated `BisectAtCurrentBrightness` to binary-search in tenths (`800..4000`), selecting the endpoint (`lo` or `hi`) with the smallest `|measured - base|` delta.
- **`--calibrate-slow` mode (`RunMap(true)`)**
  - Steps through all `0–100%` system brightness levels instead of only `kAnchorPcts` (`10, 30, 50, 70, 90`).
  - Added `allowSnap` stabilization in `WaitBrightness` and duplicate-level skipping so drivers that quantize brightness steps do not abort or re-bisect identical hardware levels.
- **Valid-anchor filtering (`BisectResult::outOfBounds`)**
  - Points where `loVal > base` (overshoots at `80.0` nits) or `hiVal < base` (undershoots at `400.0` nits) are marked `outOfBounds` and excluded from `profile.anchors`.
  - `RunMap` aborts without saving if zero valid anchors are found.
- **DXGI settling & calibration coordination**
  - Increased post-write settle delay (`200ms`) and required 3 consecutive identical non-zero reads in `MeasureStableMaxLuminance` (`100ms` interval, `3000ms` timeout).
  - Added a monotonicity guard in `BisectAtCurrentBrightness` (re-reads up to 3 times if `measured` falls outside `[loVal, hiVal]`).
  - Fixed `GetFreshDXGIDesc` device-name comparison (`wcscmp(preferGdiName, desc.DeviceName) == 0` instead of pointer equality) for multi-display setups.
  - Added `Local\LaptopTrueHDR_Calibrating` mutex held by `RunMap` and checked by `ApplyFromProfile` so `--watch` does not overwrite the slider mid-bisection, plus a startup warning if an older watcher is running.

## Watcher, Apply & Luminance Bounds

- **`--ensure-luminance-bounds <on|off>`**
  - Persisted as `ensureLuminanceBounds` (`bool`) in `Profile` / `profile.json` (with `JsonCursor::ParseBool`).
  - `ComputeLuminanceBoundsPct` derives the valid system-brightness range (`[anchors.front().pct, anchors.back().pct]`).
  - When enabled, both `--watch` (`ApplyFromProfile`) and `--apply` (`RunApply`) clamp out-of-range system brightness back into `[minPct, maxPct]` before applying the HDR content brightness.
  - Toggling requires restarting `--watch` if a watcher instance is already running.
- **Smart no-args behavior (`wmain`)**
  - Running with no arguments checks `LoadProfile`: if a valid profile exists, it runs `RunApply()` (respecting `ensureLuminanceBounds`) and exits without pausing; otherwise it runs fast `--map` calibration.

## EDID Inspection & Overrides

- **`--edid` mode (`RunEdid`)**
  - Prints DXGI output name, FNV-1a EDID hash, WinRT `DisplayMonitor` properties (native resolution, physical size, min/max/maxAvgFullFrame luminance, chromaticity), parsed EDID 1.x header fields, and a full hex dump of raw EDID blocks from the registry.
- **EDID luminance overrides**
  - Added `--edid-override-luminance-max <nits>`, `--edid-override-luminance-min <nits>`, and `--edid-override-luminance-full-frame-avg <nits>` (supporting `--flag value` and `--flag=value`).
  - `--edid-override-luminance-max` overrides `ctx.baseLevel` in `InitDisplayContext`; all three are reflected in `RunEdid()` with `[overridden]`.

## CLI & Output

- Aligned colons, labels, and fixed-width numeric columns across `PrintHeader`, `BisectAtCurrentBrightness`, `RunQuick`, `RunMap`, `RunApply`, `RunStatus`, `RunEdid`, and `RunHelp`.
- Updated `RunMap` final summary, `RunStatus`, `RunApply`, and `RunWatch` logs to surface valid HDR bounds and `ensureLuminanceBounds` state.
