# Kutsu Flasher

A browser **web flasher** for Kutsu Sense firmware — installs/updates the board over
USB with [ESP Web Tools](https://esphome.github.io/esp-web-tools/) (esptool-js). No
app install; one firmware serves every XIAO module (C3 / C5 / C6 / S3) and the chip is
auto-detected.

This is the **USB / web-flash** channel (Task 2, step 1). Wireless OTA (stationary
dashboard + mobile, no cable) is a later step — see [PROVISIONING.md] roadmap.

## Deploy it (GitHub Pages)

Web Serial needs **HTTPS** and the flasher fetches `manifest.json` + the binaries
cross-origin, so **host this folder on HTTPS** — `file://` won't work reliably.

1. Push `KutsuFlasher/` to a repo and enable **GitHub Pages** (Settings → Pages →
   deploy from branch). The flasher is then at
   `https://OWNER.github.io/REPO/KutsuFlasher/`.
2. The **Kutsu Setup** app's "Update firmware" button should point at that URL — set it
   in `KutsuSetupApp/index.html` (`FLASHER_URL`).

## Publish the firmware binaries (GitHub Releases)

The flasher installs a **merged** image per chip at offset `0`.

1. In the Arduino IDE, select the board (e.g. **XIAO_ESP32C6**) with the same Tools
   settings you flash with (Partition Scheme = **Huge APP**, USB CDC On Boot = Enabled).
2. **Sketch → Export Compiled Binary.** In the sketch's `build/…/` output, take the
   **`KutsuSense.ino.merged.bin`** (the full image — bootloader + partitions + app).
   Rename it per chip: `kutsu-c6.merged.bin`.
   - If your core doesn't emit a `.merged.bin`, merge manually:
     ```bash
     esptool.py --chip esp32c6 merge_bin -o kutsu-c6.merged.bin \
       0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 KutsuSense.ino.bin
     ```
     (Offsets come from the IDE's verbose upload log; the C-series bootloader is at `0x0`.)
3. Repeat for each module you ship (C3 / C5 / C6 / S3).
4. Create a **GitHub Release** (e.g. `v1.0.0`) and attach the `kutsu-*.merged.bin` files.
5. Edit **`manifest.json`**: replace `OWNER/REPO` and the `v1.0.0` tag with yours, and
   bump `version`. Keep one `builds` entry per `chipFamily` you provide.

## Notes

- **Credentials & settings survive an update.** Wi-Fi, room, device name, thresholds and
  toggles live in the `kutsu` NVS namespace, untouched by a normal flash. The installer's
  **Erase** option wipes them (fresh device).
- **ESP32-C5:** support depends on the esptool-js version ESP Web Tools bundles. If a C5
  isn't recognised in the browser, flash it from the Arduino IDE for now, or host a build
  of ESP Web Tools pinned to an esptool-js with C5 support.
- **Versioning:** the firmware reports `hw` and `v` (schema) in its records; consider
  adding a firmware-version string to the boot log / STATUS so the app can show "update
  available" later (ties into the wireless-OTA step).
