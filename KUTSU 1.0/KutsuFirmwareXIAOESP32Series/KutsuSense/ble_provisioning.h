#pragma once

/* ===========================================================================
 * KUTSU Sense — BLE provisioning service (GATT server)
 * ---------------------------------------------------------------------------
 * Lets the Kutsu desktop app (and any BLE central, e.g. nRF Connect) set up
 * the device locally over Bluetooth LE: push WiFi credentials + a room label,
 * read live status, and receive live sample notifications — no USB cable.
 *
 * This is the SAME "Kutsu Environmental Service" (UUIDs in config.h) the KUTSU
 * Stationary firmware exposes, so one app speaks to every build. The companion
 * app's provisioning contract is documented in ../../PROVISIONING.md.
 *
 * IMPORTANT — layering: the sensing core already calls BLEDevice::init() for the
 * passive BLE *scanner* (the Networks/Devices screens). This module must NOT call
 * BLEDevice::init() again — it assumes BLE is already up and only adds the GATT
 * server + advertising on top. Call bleProvisioningStart() from setup() right after
 * BLEDevice::init(""). NOTE: init MUST be called with an EMPTY name — passing a
 * device name to BLEDevice::init() asserts in NimBLE's ble_svc_gap_init (boot loop)
 * on the C5/C6 when a GATT server is present. This module sets the advertised name
 * ("Kutsu-XXXXXX") on the advertising object instead, so the device is identifiable.
 *
 * Coexistence caveat: a GATT server advertising while the scanner runs, all on
 * the single 2.4 GHz radio shared with WiFi, is heavier than the Stationary case
 * (which does not scan). Verify on hardware; PROVISION_BLE in config.h disables
 * this module if coexistence misbehaves on a given module (notably the C3).
 * =========================================================================== */

// Create the GATT server + characteristics and start advertising. Safe to call
// once, from setup(), after identityInit() and the scanner's BLEDevice::init().
void bleProvisioningStart();

// Push a fresh STATUS value to a connected central (called after a successful
// WiFi/time write, and whenever the net role/IP changes). No-op if nothing is
// connected or the service isn't up.
void bleProvisioningNotifyStatus();
