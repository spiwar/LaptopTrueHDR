# LaptopTrueHDR
Automatic HDR calibration tool for **laptop internal displays only**.

## Fork-Specific Features

- **Smart default (no args)**: Running without arguments applies the saved profile for the current brightness (respecting luminance bounds if enabled), or runs fast `--map` calibration if no profile exists yet.
- **`--calibrate-slow`**: Calibrates every system brightness percentage (`0–100%`) instead of interpolating between 5 anchor points, with tolerance for hardware drivers that snap to discrete brightness steps.
- **`--calibrate-slow`**: Calibrates every system brightness percentage (`0–100%`) instead of interpolating between 5 anchor points, with tolerance for hardware drivers that snap to discrete brightness steps.
- **`0.1`-nit slider precision & valid anchor filtering**: Calibration and profile application use `0.1`-nit granularity (`SDRWhiteLevel` tenths), and anchors that undershoot (`400.0` nits ceiling) or overshoot (`80.0` nits floor) are excluded so only valid calibration points are stored.
- **`--edid`**: Displays panel identity, `DisplayMonitor` luminance/chromaticity metrics, parsed EDID header fields, and a raw EDID hex dump.
- **EDID luminance overrides**: Added `--edid-override-luminance-max`, `--edid-override-luminance-min`, and `--edid-override-luminance-full-frame-avg` to override EDID-reported luminance values during calibration and `--edid` inspection.
- **`--ensure-luminance-bounds on | off`**: Stores a toggle in `profile.json` that makes `--watch` (and `--apply` / no-args) clamp system brightness back into the valid HDR calibration range if it drops below the minimum or exceeds the maximum calibrated level. Toggling this requires restarting the watcher if it is already running.

## Why this tool exists

On a desktop, or on an external monitor, with HDR enabled, Windows offer you the **SDR content
brightness** slider.  It sets how bright SDR content looks. Changing this slider doesn't change how HDR content is handled.

This isn't the case for laptops (or any Windows device with an internal display) in HDR mode. Two sliders govern how SDR and HDR content is handled:
- **System Brightness**: controls the brightness of both SDR and HDR content as a whole.
- **HDR Content Brightness**: controls how bright HDR content is *relative* to the SDR content.

Both of these values work together as a pair to affect how HDR content is handled. As a result:
- You can have 
30,000-nit content incorrectly tonemapped down into your display's range if you use a wrong value pair. 
- You have people running the Windows HDR Calibration app on a laptop and going *"my laptop clips at 4000
   nits."* This is not what the panel is capable of. This is just a fake peak that comes out of your incorrect value pair. 

To solve this and get the correct tonemapping from your laptop panel, you will need to find the correct value pair to get your adjusted peak brightness to match your base/true peak (the value from your panel's EDID). To find this pair, you previously had to:
- Run the [VESA DisplayHDRTest tool](https://github.com/vesa-org/DisplayHDRTest-v1.2)
- Check if adjusted peak = base peak yet
- If not, adjust your system brightness or HDR content brightness
- Run the tool again (you had to restart  the thing every time for it to work)
- Check if adjusted peak = base peak yet

Rinse and repeat until your base = adjusted. Doing this manually will take a lot of time and you will have to repeat the process again if you changed your system brightness. This tool exists to automate this process, allowing laptop users to get the best out of their panels.


## Requirements

- A laptop with an HDR-capable internal display
- **HDR enabled** in Windows Display settings

## Usage

1. Run the tool. The tool will calibrate your **whole brightness range**: your screen steps
   through several brightness levels and returns to the brightness you had.
2. Saves the result to a profile:
   `%LOCALAPPDATA%\LaptopTrueHDR\profile.json`
3. Run it once more with `--install` if you want the HDR content brightness to stay correct
   whenever you change system brightness

If it doesn't work, the tool will tell you why (HDR off, no brightness controls, profile missing...).
Use `--dry-run` to check without changing anything.

### All modes

| Command | What it does |
| --- | --- |
| *(no args)* | Applies the saved profile for the current brightness (respecting luminance bounds if enabled), or runs `--map` if no profile exists. |
| `--map` | Full calibration: bisects the HDR content brightness at several system-brightness anchors, saves the profile, applies the value for your current brightness. |
| `--calibrate-slow` | Full calibration across every system brightness level (`0–100%`) instead of interpolating between anchors. |
| `--quick` | Calibrate only for the current system brightness (no profile is written). |
| `--apply` | Re-apply the saved profile's value for the current system brightness. |
| `--watch` | Background watcher: re-applies the correct HDR content brightness ~2s after your system brightness changes (also after sleep/resume and dock/undock). |
| `--install` / `--uninstall` | Start/stop the watcher automatically at logon (per-user, no admin). `--install` also starts it immediately. |
| `--status` | Profile validity, panel identity, autostart state, last applied value. |
| `--edid` | Display EDID header/raw bytes and panel luminance/chromaticity data. |

Options (can be combined with `--map`, `--calibrate-slow`, `--quick`, or `--edid`):

- `--ensure-luminance-bounds <on|off>` — toggle bounds enforcement in `profile.json` (when `on`, `--watch` and `--apply` keep system brightness within the valid HDR calibration range; requires restarting the watcher if already running).
- `--edid-override-luminance-max <nits>` — override the EDID max peak luminance used as the calibration target.
- `--edid-override-luminance-min <nits>` — override the EDID min luminance.
- `--edid-override-luminance-full-frame-avg <nits>` — override the EDID max average full-frame luminance.


## How it works

1. Reads your current SDR/HDR content brightness setting.
2. Reads your **base peak**, the panel's true peak luminance from the EDID, via
   `DisplayMonitor.MaxLuminanceInNits()`.
3. Reads the **adjusted peak**, what Windows reports through
   `DXGI_OUTPUT_DESC1.MaxLuminance`. This one moves as the slider moves.
4. Searches for the slider value where `base peak / adjusted peak` is closest to 1

## Notes
- In games that let you set peak brightness, use the **FINAL ADJUSTED VALUE** the tool prints.
- If you manually drag the HDR content brightness slider afterwards, your change stays until the
  next system-brightness change (then the profile value wins, if the watcher is running).
- The watcher works without admin rights and writes a small log to
  `%LOCALAPPDATA%\LaptopTrueHDR\watcher.log`- In games that let you set peak brightness, use the **FINAL ADJUSTED VALUE** the tool
  prints.
- WON'T WORK IF YOU'VE USED "WINDOWS HDR CALIBRATION", DELETE ALL PROFILES CREATED BY THAT THING


## Known issues

- If your laptop's brightness WMI provider is broken (common on hybrid-GPU laptops), the tool falls
  back to the power-plan brightness value. The watcher then detects brightness changes by polling
  every ~5s instead of instantly.
