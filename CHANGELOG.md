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
- Visitors page (`/page/v.mu`, admin only): who has a link open to the node, longest silent first, with time since anything was received, hops, the visitor's name (or short identity, or "anon") and the repeater a 2+ hop link comes through. Admin → Device gets a "Routes  Neighbours  Visitors" row; Routes and Neighbours link to Visitors for admins.

### Fixed

- API paging (`o`, `n`, `s` on `/api/paths`, `/api/nbrs`, `/api/links`, `/api/hist`) now also works from a NomadNet browser: link variables (`` `/api/links`s=1|n=4 ``, sent as `var_s`/`var_n`) were ignored, only raw request-data keys from scripts counted. The docs no longer suggest a `?s=1` query string, which NomadNet treats as part of the path, so the request timed out.
- nRF52 boards: NaN checks work again. The Adafruit core appends `-Ofast` after our `-Os`, and its fast-math folded every `isnan()` to false, so missing readings showed as "nan" (e.g. `nanhPa` / `nan%` on the Telemetry page per-chip rows with an ENV III). Built with `-fno-finite-math-only` now.
- Solar Node P1: the radio is reset before it is probed at boot. The P1 bootloader (0.9.2-OTAFIX2.2-BP1.3) can leave the SX1262 in reset, giving "No radio module found", `hw_ready 0` and a TNC config that never loads (the node fell back to host-controlled after a restart). A failed probe now logs the sync-word bytes read on nRF52 too.
- microReticulum is pinned by its full commit hash: PlatformIO cannot fetch the short `fd76f15` on a clean build, it only worked from an existing `.pio` cache.
- Solar Node P1 image is back under the 798,720 B its bootloader accepts: the "Your link" pages took it 2.4 KB over, so the bootloader silently refused both the UF2 and the DFU zip. The P1 build now compiles out TRACE logging (`RNS_LOG_LEVEL_DEBUG`), leaving ~28 KB headroom. The build now fails when the image exceeds that limit (`custom_max_image_size`), and the P1 UF2 is packaged with the Seeed family ID `0x28860044` its bootloader requires (the generic `0xADA52840` was silently ignored).
- Closed stale/abandoned Reticulum Links after 15 minutes idle — the library's `Link::start_watchdog()` is an unimplemented stub upstream, so a NomadNet client that never sent `LINKCLOSE` leaked memory forever.
- ESP32 post-upload firmware-hash write no longer races the provisioning step's USB-CDC reset (5s settle delay added).
- RAK4631 and T-Echo builds link again (hard-fault dump now Solar Node P1 only).
- Dropped the libstdc++ demangler (~32 KB flash) by recording uncaught C++ exceptions in the fault record instead.
- ScotMesh's own loop (`sm_loop()`) is now wrapped in the same try/catch as the RNS loop and LoRa interface: an exception thrown by LXMF send/announce/path-request/link-teardown work used to reach the uncaught-exception handler and force a reboot, with only a generic fault-record `lr` (which always resolves to the same spot inside the exception runtime, not the actual throw site) to go on. It's now logged with the real message and the device keeps running instead.
- `/api/sys` now reports `free_pool`/`pool_fragmented` -- the actual TLSF memory pool Reticulum Links/Packets/Bytes allocate from, not just `free_ram` (the general system heap, which doesn't move with link count and was the only memory figure visible before). Added while investigating a live TX-stall incident where the open-link count climbed into the high 20s; the pool's real headroom wasn't observable at all before this.
- `sm_links_watchdog()` now also closes the oldest active Link whenever the TLSF link pool drops below a configurable floor (`SM_MIN_POOL_FREE`, default 10 KB, override per board with `-DSM_MIN_POOL_FREE=...` in `platformio.ini`), on top of its existing idle-timeout sweep. Picks the victim by `request_time()` (when established), not `inactive_for()` -- the latter is a min() of in/out activity, so a client stuck retrying a dead handshake looks "freshest" and would wrongly survive. Guardrail against the memory-exhaustion half of the TX-stall investigation above; not a confirmed fix, since the actual root cause (memory exhaustion vs. a radio-interference deadlock, which this does nothing for) was never conclusively captured mid-incident. See `TODO.md`.
- Link watchdog now follows Python RNS's rules: an established link is closed after 725 s with nothing received (`STALE_TIME` + `STALE_GRACE`, measured with `no_inbound_for()` -- client keepalives every 360 s keep live sessions open), and a handshake that never finishes is closed after its `establishment_timeout` instead of waiting the full idle period. Previously `inactive_for()` also counted our own sends, so a peer we kept sending to after it had gone looked alive. Under memory pressure the cap now closes unfinished handshakes first, then the least recently heard link, instead of the oldest-opened one (which could be a long session still in use). The pressure cap is also skipped on builds without the TLSF heap pool: on ESP32 boards using PSRAM or the plain heap (e.g. Heltec V4) `heap_pool_free()` reads 0, so it would have closed every link every 30 s. New counters on the Health page ("Closed stuck/idle/mem") and in `/api/sys` (`links_closed_stuck`, `links_closed_idle`, `links_closed_mem`) show the watchdog at work.
- Home page footer centering now uses one `` `c `` for the block instead of per-line.

### Changed

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
