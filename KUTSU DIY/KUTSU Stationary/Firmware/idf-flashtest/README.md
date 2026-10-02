# KUTSU Flash Test (ESP-IDF)

[← Stationary firmware](../README.md) · [Main firmware](../KutsuSense/README.md)

Minimal ESP-IDF project to confirm **Espressif IDE can flash the ESP32-C5-DevKitC-1**
before porting the real firmware. It blinks the three status LEDs in sequence
(**GPIO5 green → GPIO26 yellow → GPIO0 red**), so a good flash also verifies the LED
wiring. No sensors/OLED needed.

## First: install the toolchain (fixes "No Toolchain found for Target Local")
That error means ESP-IDF + the RISC-V compiler toolchain aren't installed/registered yet.
Open the **ESP-IDF Manager** (toolbar icon, or *Help → ESP-IDF Manager*, or *Window →
Preferences → Espressif → ESP-IDF*), then:
1. **Download & configure ESP-IDF → v5.5 or newer** (required for the C5). Sets `IDF_PATH`.
2. **Install Tools** — downloads the **riscv32-esp-elf toolchain**, CMake, Ninja, OpenOCD,
   Python env. This is the piece the error is missing. Let it finish (a few hundred MB).
3. **Restart** the IDE, reopen this project, **Set Target → esp32c5** again, then Build.

## Steps in Espressif IDE
1. **ESP-IDF version:** the C5 needs **ESP-IDF v5.5 or newer** (installed above). An older
   IDF won't list `esp32c5` as a target.
2. **Open the project:** *File → Open Projects from File System…* → select this
   `idf-flashtest` folder → Finish.
3. **Set target:** bottom status bar target selector (or right-click project → *ESP-IDF:
   Set Espressif Device Target*) → **esp32c5**.
4. **Select port:** bottom status bar port selector → your board's COM port. Try both of
   the board's USB-C ports ("USB" native and "UART" bridge) if one doesn't enumerate.
5. **Flash settings:** UART flashing, **baud 115200** (Espressif IDE ships a newer esptool
   than Arduino's 5.1.0 — this is the point of the test).
6. **Build** (hammer / ✓), then **Flash** (⚡). If it doesn't auto-enter download mode:
   hold **BOOT**, tap **RST**, release **BOOT**, then Flash.
7. **Monitor** (📟) at 115200 — you should see `KUTSU flash-test running…` and `LED … on`
   lines, and watch the three LEDs cycle.

## Alternative: the command line (avoids Eclipse toolchain quirks)
The same install provides an **"ESP-IDF 5.5 PowerShell/CMD"** shortcut. Open it, `cd` into
this folder, and:
```
idf.py set-target esp32c5
idf.py build
idf.py -p COM23 flash monitor
```
(Swap `COM23` for your port; Ctrl-`]` exits the monitor.) `idf.py` bundles a current
esptool and can also flash over the native USB-JTAG — the most dependable route on the C5.

## If UART flashing still fails → use JTAG
The C5's **native "USB" port is a USB-Serial-JTAG**, so Espressif IDE can flash over
**JTAG/OpenOCD**, bypassing esptool entirely:
- Command palette → *ESP-IDF: Flash (with JTAG)* (or set the flash method to **JTAG** in
  the project's ESP-IDF settings), then Flash. This is the most reliable path on the C5
  and sidesteps the stub-flasher bug that blocked the Arduino toolchain.

## Result
- **The LEDs blink and the log lines appear:** the toolchain can flash this board. You can
  then flash the real firmware, either through ESP-IDF (Arduino as a component) or with the
  `esptool --no-stub` workaround in the [main firmware README](../KutsuSense/README.md).
- **It still fails:** keep the full build/flash console output. The stage where it fails
  tells you whether the problem is esptool, JTAG/driver, or the board itself.
