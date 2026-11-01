# MKS TS24-R UI port

This is an opt-in first port slice for the MKS DLC32 V2.2 / TS24-R V2.1. It uses the LVGL 6.1.1 copy from the MKS DLC32 source tree and ESP-IDF SPI instead of Arduino `TFT_eSPI`.

The official MKS source tree contains the TS35 UI (`MKS_TS35.*`, 480x320) but no TS24 UI source; Makerbase has an open issue requesting the TS24 source. The TS24-R is 320x240 and uses an ST7789V controller, while MKS's TS35 TFT_eSPI setup selects ST7796. This port reuses the TS35 UI approach and LVGL, with TS24 geometry and ST7789 initialization. The ST7789 command sequence is based on the ST7789 driver bundled in MKS's own TFT_eSPI source.

## Implemented TS24 screens

- Ready screen with live MPos/WPos and controller state.
- Control screen with compact live MPos/WPos DRO, a centered target button for jogging to XY work zero, X/Y jog buttons around it, Z jog, selectable 0.1/1/10 mm increments, selectable 500/1000/2000 mm/min jog feed, alarm-only red `UNLOCK` control, hard homing, controller reboot, and `X0Y0` / `Z0` work-coordinate zeroing.
- SD file browser with directory navigation and G-code extension filtering. It shows five entries per page.
- Job screen with explicit Start, Hold, Resume, and Stop actions. Selecting a file does not start it; Start is a separate action.
- Setup screen for switching between CNC and Laser mode, with a note that directs steps/mm, homing, limits, and machine calibration to grblHAL configuration.
- MKS-like dark panels, blue accents, and green pressed buttons, adapted to the 320 x 240 display.

The TS24 pages are adapted from the stock TS35 workflow. Wi-Fi provisioning, language selection, frame preview, probe macros, power calibration, and the full MKS job telemetry view are not ported yet. The SD browser uses grblHAL VFS and the board SD slot; the current build configuration enables `SDCARD_ENABLE` for that purpose.

The user has confirmed display orientation, touch alignment, live DRO operation, and screen operation on the board. The screen flow builds with ESP-IDF v4.3 and generates `build/grbl.bin`. The board is not connected to a machine, so motion and job execution have not been validated.

## Enable

1. Select `BOARD_MKS_DLC32_V2P0` in `main/my_machine.h`.
2. Use the ESP-IDF v4.3 environment described in the grblHAL ESP32 README. Open its ESP-IDF terminal and go to the `grblhal-esp32` project folder.
3. Set the target once with `idf.py set-target esp32`.
4. Run `idf.py menuconfig`; under **Serial flasher config**, set **Flash size** to **8 MB** for the DLC32 V2.2, then save and exit.
5. Initialize the submodules and apply the project patch for the two grblHAL core changes:

   ```text
   git submodule update --init --recursive
   git -C main/grbl apply ../..\patches\grblhal-core-ts24.patch
   ```

6. Build the experimental UI with the normal (non-safe-bring-up) profile:

   ```text
   idf.py -D MKS_TS24_UI=ON -D MKS_TS24_UI_SAFE_BRINGUP=OFF build
   ```

7. With the controller on USB and its COM port known, flash and open the serial monitor:

   ```text
   idf.py -p COM7 -b 115200 flash monitor
   ```

The two CMake options are kept in the build cache. The Web Builder cannot include this local UI component.

The option defaults to OFF, leaving the upstream grblHAL board map and build unchanged. With it enabled, the TS24 uses:

| Signal | DLC32 GPIO |
| --- | ---: |
| SPI clock / MISO / MOSI | 18 / 19 / 23 |
| LCD chip select | 25 |
| Touch chip select | 26 |
| LCD reset | 27 |
| Backlight | 5 |
| LCD data/command | 33 |

Those signals overlap existing auxiliary pin assignments. The TS24 configuration therefore reserves them and rejects spindle enable/direction, flood/mist coolant, spindle 1, PWM servo, and MPG mode configurations that claim the same pins. `MKS_TS24_UI_SAFE_BRINGUP` disables spindle, coolant, and MPG functions so the display can be brought up without those conflicts; it must not be used as the final machining profile. A production profile needs a deliberate spindle and pin plan.

The touch conversion uses the raw range already adjusted on the user's TS24-R; the user confirmed that touch and display now align. Recheck alignment if the panel, flex cable, or orientation setting changes.

## Bench bring-up notes

- The user's current bench setup has no machine attached. Do not use jog, home, or local-file Start as a motion validation step.
- After rebuilding and flashing, verify the page navigation, touch targets, MPos/WPos updates, and that selecting a file only opens the job page. Start is a separate button.
- The touch UI does not contain axis calibration controls. Those remain in grblHAL settings/configuration.
- Before connecting a machine, configure its steps/mm, homing, limits, and physical output mapping in grblHAL. The safe bring-up profile disables spindle, coolant, and MPG functions that conflict with TS24 display pins; it is not a finished production pin profile.
