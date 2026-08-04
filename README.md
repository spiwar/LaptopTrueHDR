# HDR Laptop Calibration

Automatic HDR calibration tool for **laptop internal displays only**.


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
- **External monitors unplugged** (see [Known issues](#known-issues))

## Usage

1. Set your system brightness to maximum.
2. Run the tool.
3. It just works.

If it doesn't work, then your system brightness is too low, the program will tell you that

## How it works

1. Reads your current SDR/HDR content brightness setting.
2. Reads your **base peak**, the panel's true peak luminance from the EDID, via
   `DisplayMonitor.MaxLuminanceInNits()`.
3. Reads the **adjusted peak**, what Windows reports through
   `DXGI_OUTPUT_DESC1.MaxLuminance`. This one moves as the slider moves.
4. Searches for the slider value where `base peak / adjusted peak` is closest to 1.


## Notes

- Afterwards, check your Windows "HDR content brightness" value. It now shows the best
  setting for your current system brightness. **Remember that system brightness + HDR
  content brightness combo.** That's where your screen is most accurate.
- Want a brighter or dimmer desktop? Use whatever system brightness you like and just
  run the tool again.
- In games that let you set peak brightness, use the **FINAL ADJUSTED VALUE** the tool
  prints.
- WON'T WORK IF YOU'VE USED "WINDOWS HDR CALIBRATION", DELETE ALL PROFILES CREATED BY THAT THING


## Known issues

- **Multi-monitor is not handled.** Measurements come from DXGI adapter 0 / output 0,
  but the slider write goes to display path 0.
- The result of `DisplayConfigSetDeviceInfo` is not
  checked, so if HDR is disabled on the target display the tool will print
  confident-looking numbers while changing nothing. Confirm HDR is on first.

