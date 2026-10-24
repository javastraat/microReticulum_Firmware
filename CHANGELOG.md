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

### Fixed

- Solar Node P1 image is back under the 798,720 B its bootloader accepts: the "Your link" pages took it 2.4 KB over, so the bootloader silently refused both the UF2 and the DFU zip. The P1 build now compiles out TRACE logging (`RNS_LOG_LEVEL_DEBUG`), leaving ~28 KB headroom. The build now fails when the image exceeds that limit (`custom_max_image_size`), and the P1 UF2 is packaged with the Seeed family ID `0x28860044` its bootloader requires (the generic `0xADA52840` was silently ignored).
- Closed stale/abandoned Reticulum Links after 15 minutes idle — the library's `Link::start_watchdog()` is an unimplemented stub upstream, so a NomadNet client that never sent `LINKCLOSE` leaked memory forever.
- ESP32 post-upload firmware-hash write no longer races the provisioning step's USB-CDC reset (5s settle delay added).
- RAK4631 and T-Echo builds link again (hard-fault dump now Solar Node P1 only).
- Dropped the libstdc++ demangler (~32 KB flash) by recording uncaught C++ exceptions in the fault record instead.
- Home page footer centering now uses one `` `c `` for the block instead of per-line.

### Changed

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
