# Kutsu Setup

A small **local app** for setting up a flashed Kutsu Sense device — WiFi network,
room label, and clock — over **Bluetooth** or **USB**, with a friendly form instead
of raw bytes in nRF Connect.

It is a single self-contained web page (`index.html`): no install, no server, no
internet. Everything runs on your computer and talks straight to the device.

## Run it

1. Open **`index.html`** in desktop **Google Chrome** or **Microsoft Edge** (v89+).
   Double-clicking the file works. *(Web Bluetooth / Web Serial are not available in
   Firefox or Safari — the app says so if you land there.)*
2. Power the Kutsu on and keep it nearby (Bluetooth) or plug in the USB-C cable.
3. Click **Connect over Bluetooth** (pick `Kutsu-XXXXXX` in the chooser) or
   **Connect via USB** (pick the device's serial port).
4. Set **mode → stationary**, then enter your **network name + password** (and an optional
   **room** label) → **Save & connect**.
5. Make sure the [KUTSU Hub](../KutsuHub/README.md) is running on a PC/Pi on that network.
   Watch the **Hub** row: once the unit has found the hub, click its **dashboard** link.

> Tip: prefer a **2.4 GHz** network — every Kutsu module has 2.4 GHz; only the C5
> also joins 5 GHz.

## What it does

- Reads live **status** (name, ID, role, WiFi/IP, hub, room, clock) and shows the KUTSU Hub
  dashboard link once the device is feeding a hub.
- Writes **WiFi credentials** + room, and syncs the device clock from your computer.
- **Device settings** — toggle buzzer, warning LED, display, and WiFi/BLE scanning, and
  switch **handheld ⇄ stationary** mode (stationary joins WiFi and feeds the KUTSU Hub;
  handheld stays off WiFi). Saves to the device immediately.
- Shows **live readings** (CO₂ / VOC / NOx / temp / humidity) while connected.
- **Forget WiFi** returns the device to standalone handheld-local operation.

## How it connects

Two transports behind one interface (see [`../PROVISIONING.md`](../PROVISIONING.md)
for the exact contract):

- **Bluetooth LE** → the device's "Kutsu Environmental Service" GATT server
  (firmware `ble_provisioning.cpp`).
- **USB serial** → the firmware's console commands (`ssid` / `pass` / `room` /
  `connect` / `forget` / `status`) at 115200 baud.

The UUIDs/commands in `index.html` must match the firmware; both are specified in
`PROVISIONING.md`.

## Requirements on the device

BLE setup needs firmware with `PROVISION_BLE` enabled (the current
`KutsuFirmwareXIAOESP32Series` and the KUTSU Stationary build). USB setup works on
any build with the serial console. If the Bluetooth picker shows no Kutsu, the unit
is likely on older firmware without the provisioning service — use USB, or reflash.

## Roadmap

This page is the seed for a packaged **Tauri/Electron** desktop build and more
features (firmware info/OTA, SCD41 fresh-air calibration, threshold tuning, node
naming). Features extend both transports behind the shared interface.
