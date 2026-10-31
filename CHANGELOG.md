# Changelog

## [Unreleased]

### Added

- Grove-port sensor support on the Solar Node P1: SHT4x, SHTC3, AHT10/20, BME280, BME680, BMP180, M5 ENV I/II/III, and INA219/INA226/INA260/INA3221 power monitors, plus the nRF52840's own die temperature — all on the Telemetry page and `/api/env`.
- 7-day telemetry history (battery, sensors, mesh) with a History page and `/api/hist` endpoint.
- LXMF alerts to admins (restart recovery, low battery) and a `status` command, from the node's own announced LXMF address.
- Optional flag and footer line on the NomadNet home page, set per node on the Name & announce page.
- Cached neighbour names shown next to hashes on the NomadNet Routes/Neighbours pages, with short hash for named rows and short-by-default.
- Opt-in power-saving build flags for ESP32 boards (160 MHz underclock + loop-yield idle gating), enabled for Heltec V4.
- "Your link" line on the NomadNet home page: how many hops the visitor's request took, plus RSSI/SNR of the last hop when it arrived over LoRa. Read from the raw packet header in the receive hook, no library change. On by default, can be hidden on the Home page layout page; admins see it on the Admin page instead, and the admin home view shortens the verified identity to 8 characters (full hash on the Admin page) so Radio and Airtime fit again. For 2+ hop links the Admin page also names the last hop: from the path table towards the admin's own LXMF/NomadNet address, else guessed from the signal of recently heard repeaters (marked with "?").
- `/api/links`: the open Reticulum links, longest silent first, with state, age, seconds since anything was received, hops/RSSI of the last packet, the visitor's identity and announced name if it identified itself, and for 2+ hop links the repeater it comes through. Shows who is holding links open when the count climbs.
- Home page battery line shows a green ● when the pack is full (charged), next to ▲ charging and ▼ on battery; full used to show the same ▲ as charging.
- Battery calibration on the Power page: type the voltage a multimeter reads on the pack into Meter and press Calibrate, and the node stores the ratio to its own reading and applies it from then on, so voltage, percentage, charge detection, alerts and the Power thresholds use the real pack voltage. Per node, without a rebuild; no calibration = the board's built-in scale. The Solar Node P1 read about 1.7 % low (a full pack measured 4.16 V, the page showed 4.09 V / 94 %), so its "radio off below 3.3 V" really cut in near 3.36 V. Works on every board that reads its battery through an ADC (P1, Heltec V3/V4, RAK4631, T-Echo, T114, T3S3, ...). Kept in its own file across updates and a full reset.
- Home page settings: two new checkboxes, "ScotMesh credit" and "Firmware & board", hide the footer's credit line and firmware/board line from visitors (admins always see them). Both on by default, so existing nodes look the same; stored in the config's one spare byte, so saved settings keep loading. Your own flag/footer line stays as before: leave its fields empty to hide it.
- Visitors page (`/page/v.mu`, admin only): who has a link open to the node, longest silent first, with time since anything was received, hops, the visitor's name (or short identity, or "anon") and the repeater a 2+ hop link comes through. Admin → Device gets a "Routes  Neighbours  Visitors" row; Routes and Neighbours link to Visitors for admins.

### Fixed

- Low-battery protection now fires at night. The Power page actions (lower TX power, radio off until charged, radio off below 3.3 V) and the LXMF low-battery alert only acted when the node thought it was not charging, and boards without a charge-status pin guess that from the voltage trend: a big pack that barely moves at night read "charging" in the dark, so none of them ever triggered. They now go by battery level only. On the Solar Node P1 and the Heltec V4 (both run solar 4x18650 packs), "charging" itself now needs a clear rise (+15 mV against an hour ago) instead of just "not dropping", so the Telemetry page, `/api/bat`, the history and alerts stop showing charging in the dark. It ends as soon as the voltage falls 8 mV within the last 10-20 minutes, so sunset shows within about 20 minutes. The Heltec V4 battery voltage now uses MeshCore's ADC scale (the old one read about 4.5 % low, a full 4.20 V pack showed about 4.02 V), and its percentage the same 3.15 V = 0 % .. 4.15 V = 100 % line as the P1 (was 3.05 .. 4.0 V, which compensated for the low reading), so a Power-page threshold means the same real voltage on both boards.
- Heltec V3 and V4: the Lights setting (off / off after 10 min) now switches off the onboard LED too. It was only honoured on the Solar Node P1, so on a Heltec the LED kept blinking whatever was chosen.
- API paging (`o`, `n`, `s` on `/api/paths`, `/api/nbrs`, `/api/links`, `/api/hist`) now also works from a NomadNet browser: link variables (`` `/api/links`s=1|n=4 ``, sent as `var_s`/`var_n`) were ignored, only raw request-data keys from scripts counted. The docs no longer suggest a `?s=1` query string, which NomadNet treats as part of the path, so the request timed out.
- Home page reads less cramped: the operator line sits under the node name instead of under "Status" (so a visitor who can only see the operator no longer gets a "Status" header above it), a blank line separates the name from the status block, "Up" is padded to line up with "Radio", and in the admin view public rows get a space where hidden ones get a `*`, so the rows stay aligned. "Not identified" is no longer sent to visitors who didn't identify: it told them nothing they could act on and cost 15 bytes of the one-packet page.
- `/api/links` no longer tells everyone who is browsing the node: `who`, `name` and `via` are now `null` unless the request comes from an identified admin, as on the admin-only Visitors page. The identity and name come from an identify the visitor sent only to this node, and `via` ties a live session to the area around one repeater. Link id, state, times, hops and RSSI stay public.
- nRF52 boards: NaN checks work again. The Adafruit core appends `-Ofast` after our `-Os`, and its fast-math folded every `isnan()` to false, so missing readings showed as "nan" (e.g. `nanhPa` / `nan%` on the Telemetry page per-chip rows with an ENV III). Built with `-fno-finite-math-only` now.
- Solar Node P1: the radio is reset before it is probed at boot. The P1 bootloader (0.9.2-OTAFIX2.2-BP1.3) can leave the SX1262 in reset, giving "No radio module found", `hw_ready 0` and a TNC config that never loads (the node fell back to host-controlled after a restart). A failed probe now logs the sync-word bytes read on nRF52 too.
- microReticulum is pinned by its full commit hash: PlatformIO cannot fetch the short `fd76f15` on a clean build, it only worked from an existing `.pio` cache.
- Solar Node P1 image is back under the 798,720 B its bootloader accepts: the "Your link" pages took it 2.4 KB over, so the bootloader silently refused both the UF2 and the DFU zip. The P1 build now compiles out TRACE logging (`RNS_LOG_LEVEL_DEBUG`), leaving ~28 KB headroom. The build now fails when the image exceeds that limit (`custom_max_image_size`), and the P1 UF2 is packaged with the Seeed family ID `0x28860044` its bootloader requires (the generic `0xADA52840` was silently ignored).
- Stale Reticulum links are closed: the library's `Link::start_watchdog()` is an unimplemented stub upstream, so a client that never sent `LINKCLOSE` (closed app, out of range, crash) left its link and its memory allocated forever. `sm_links_watchdog()` runs every 30 s and follows Python RNS's rules: an established link is closed after 725 s with nothing received (`STALE_TIME` + `STALE_GRACE`; client keepalives every 360 s keep live sessions open), and a handshake that never finishes after its `establishment_timeout`. On boards whose Reticulum memory lives in the TLSF pool (nRF52, e.g. the Solar Node P1) it also keeps the pool above a floor (`SM_MIN_POOL_FREE`, 10 KB default, per board in `platformio.ini`) by closing unfinished handshakes first, then the least recently heard link. Counters for all three (`links_closed_stuck`/`idle`/`mem`) are on the Health page and in `/api/sys`. Not a confirmed fix for the TX stall that prompted it: memory exhaustion and a radio-interference deadlock both fit that incident, and this only covers the first.
- ESP32 post-upload firmware-hash write no longer races the provisioning step's USB-CDC reset (5s settle delay added).
- RAK4631 and T-Echo builds link again (hard-fault dump now Solar Node P1 only).
- Dropped the libstdc++ demangler (~32 KB flash) by recording uncaught C++ exceptions in the fault record instead.
- ScotMesh's own loop (`sm_loop()`) is now wrapped in the same try/catch as the RNS loop and LoRa interface: an exception thrown by LXMF send/announce/path-request/link-teardown work used to reach the uncaught-exception handler and force a reboot, with only a generic fault-record `lr` (which always resolves to the same spot inside the exception runtime, not the actual throw site) to go on. It's now logged with the real message and the device keeps running instead.
- `/api/sys` now reports `free_pool`/`pool_fragmented`: the actual TLSF memory pool Reticulum Links/Packets/Bytes allocate from, not just `free_ram` (the general system heap, which doesn't move with link count and was the only memory figure visible before). Added while investigating a live TX-stall incident where the open-link count climbed into the high 20s; the pool's real headroom wasn't observable at all before this.
- Home page footer centering now uses one `` `c `` for the block instead of per-line.

### Changed

- Solar Node P1: the link-pool floor is 15 KB (`-DSM_MIN_POOL_FREE=15360`) instead of the 10 KB default, as measured on a live repeater: about 2 KB per link, and a full day at this floor with up to 21 links open ran without a TX stall.
- The JSON API is always on. The "Telemetry API" checkbox on the Home page layout page is gone: it switched `/api/...` for everyone (including Node-RED feeds) but changed nothing on the home page, and a disabled API still cost the same link and request airtime.
- Telemetry page shows sea-level pressure (from the height set on the Map page) instead of an altitude worked out from the standard 1013.25 hPa, which drifted with the weather (an Amsterdam roof read 160 m). `/api/env` keeps its `altitude` field.
- Heltec V4 button: hold-to-sleep removed (unsafe on a roof-mounted repeater); single click toggles the display, double click announces, hold sends a 5x announce burst.
- WiFi update window moved to `10.73.73.1`, clear of common home-network ranges, with upload progress and a clear message if the connection drops.
- History page shows the first hour live from 1 minute after boot instead of staying empty for an hour.
- **Provisioning wire format** — reworked the wire protocol for lower LoRa airtime and fewer round-trips. Breaking change from the previous format:
    - `GET_STATE` now returns `{Values, Drafts?, Hash}`; the `Draft` request flag folds drafts into the same response.
    - `GET_STATE` supports a `PriorHash` short-circuit: clients echo the previous response's `Hash` back, and the server responds `{Unchanged: true}` when state hasn't changed.
    - `SET_STATE` requests use a `{State: {...}, IncludeState?, ReqCompress?}` envelope. With `IncludeState: true`, the response includes `PostOpValues` / `PostOpDrafts` / `PostOpHash` — Save no longer needs a follow-up `GET_STATE`.
    - `COMMIT` requests use a `{NamespaceFilter?, IncludeState?, ReqCompress?}` map (was a bare `[ns_ids]` array). With `IncludeState: true`, the response carries post-commit `PostOpValues` and `PostOpHash` — Commit no longer needs a follow-up refresh.
    - `GET_CAPABILITIES` returns a namespace hierarchy map (`{NsId, NsName, NsParent, NsFieldCount, NsSchemaHash}`) instead of a bare id array; combined with `GET_SCHEMA`'s new `NamespaceFilter` support, this enables lazy per-namespace schema loading.
- **Web console** — refactored to use the new wire protocol. Save and Commit each collapse from two transactions to one; namespace-panel refreshes cache-hit via `PriorHash` when state hasn't changed since the last poll.

## [1.86.4] - 2026-06-27

### Added

- Added WebSocket support for KISS interfaces
- Added public WebSocket operating mode
- Added RNS transport support in the web console for remote provisioning
- Added integration with MeshChatX API servers for remote link establishment
- Added native SX1262 board support
- Added extended provisioning and remote-management metrics
- Added expanded provisioning field definitions
- Added provisioning reboot as a first-class operation
- Added browser local-storage schema caching for provisioning
- Added schema and provisioning-data compression
- Added log filtering by substring in the web console
- Added configurable maximum log-level filtering
- Added command-line configuration path support for native builds
- Added additional native packet and diagnostic logging
- Added detailed provisioning and management documentation
- Added Docker workspace auto-clone behavior when repository is missing

### Changed

- Refactored build configuration to reduce firmware size
- Refactored provisioning draft, working, and effective value handling
- Refactored native TCXO enablement
- Improved radio initialization and reset sequencing
- Improved provisioning transfer efficiency for low-bandwidth links
- Updated native pin mappings and board configuration support
- Updated web console packaging and provisioning behavior
- Disabled periodic statistics dumps to logs
- Improved modem status and noise floor handling
- Improved transaction handling in radio drivers

### Fixed

- Fixed embedded single-transfer regression affecting SX radios
- Fixed TX queue lockup when interference avoidance was enabled
- Fixed `medium_free()` incorrectly returning false after startup
- Fixed RX LED remaining active when noise floor sampling was unavailable
- Fixed float-to-int conversion truncation for values below 1
- Fixed heatshrink decoding issues in the web console
- Fixed Linux reboot failure in native builds
- Fixed WebSocket failures occurring after reboot
- Removed ineffective pre-initialization board reset behavior

### Documentation

- Added comprehensive provisioning documentation
- Expanded remote management documentation
- Updated README examples and provisioning guidance

### Contributors

- @attermann
- @nilu96
