# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Linux port of [T-Troll/alienfx-tools](https://github.com/T-Troll/alienfx-tools): direct hardware control of Alienware/Dell G-series RGB lighting and fans, without Dell's official (and limited) LightFX/AlienFX SDK. Two static C++23 libraries plus a CLI and a minimal example app:

- `AlienFX-SDK` — RGB lighting control over HID/libusb, for keyboards, notebooks, mice, monitors, external keyboards.
- `AlienFan-SDK` — fan speed, sensors, and power-profile control via sysfs (`alienware-wmi` kernel driver).
- `alienfx-cli` — command-line tool built on both SDKs (see README for full subcommand reference).
- `Example-App` — minimal usage sample of `AlienFX-SDK`.

## Build

```bash
cmake -S . -B build -G Ninja -DALIENFX_BUILD_CLI=ON -DALIENFX_BUILD_EXAMPLE=ON
cmake --build build
```

- Both apps are **off by default** (`CMakeLists.txt`) — the two SDK libraries always build; `alienfx-cli` and `Example-App` need their `-DALIENFX_BUILD_*=ON` flags.
- `-DCMAKE_BUILD_TYPE=Debug` defines `DEBUG`, which raises loguru to `INFO` verbosity and enables hex dumps of every HID packet sent in `Functions::PrepareAndSend` — this is the primary way to debug protocol issues.
- Binaries land directly in `build/` (`alienfx_cli`, `example_app`); libraries in `build/<SDK-dir>/`.
- `.cpp` sources are picked up via `file(GLOB_RECURSE ... CONFIGURE_DEPENDS src/*.cpp)` per target, so new source files don't need CMakeLists edits (re-run cmake configure if a new file isn't picked up).
- Dependencies (libusb-cmake, loguru, nlohmann/json, CLI11) are pulled with `FetchContent`, pinned to release tags — configuring needs network access.
- `AlienFX-SDK` talks to HID devices directly over libusb (`AlienFX-SDK/src/libusb_helper.cpp`) — no hidapi, no hidraw. Every transfer claims (and, where a kernel driver owns the interface, detaches) the USB interface for the duration of that one call only, never longer — see `docs/hid-transport.md` for why (short version: hidraw was safe but ~425x slower on some hardware; a device-handle-lifetime libusb claim, like hidapi's libusb backend does it, is fast but can disable a keyboard whose input interface doubles as an AlienFX vendor ID, e.g. Darfon `0x0d62` — see `docs/probe-keyboard-lockup.md` for that incident).

## Testing

There is no automated test suite, linter, or CI in this repo. Verification is manual, on real hardware, and generally needs root for HID access:

```bash
sudo ./build/alienfx_cli status
sudo ./build/alienfx_cli setall 255 0 0
```

Device/light name mappings are configured interactively once via `probe` — note `sudo` resets `$HOME`, so the invocation must preserve the real user's home for mappings to land in the right place: `HOME=/home/<username> sudo ./alienfx_cli probe`. Run `probe --dry-run` first (no root needed) to see which HID devices/collections would be opened without touching any of them — useful sanity check before probing unfamiliar hardware. `probe` also warns and prints recovery commands before opening anything, and honors SIGINT/SIGTERM cleanly (closes all open HID handles instead of leaving the process, and any kernel driver it touched, mid-open) — see `docs/probe-keyboard-lockup.md` for the incident this hardening responds to.

## Architecture

### AlienFX-SDK: per-device-API dispatch, not per-device classes

`AlienFX_SDK::Functions` (`AlienFX-SDK/src/AlienFX_SDK.cpp`) represents one HID device. There is no per-model subclassing — almost every public method (`SetAction`, `SetMultiColor`, `SetBrightness`, `Reset`, `UpdateColors`, ...) is a `switch (version)` over `Afx_Version` (`API_V2` .. `API_V8`, plus the unused/removed `API_ACPI`). Adding support for a new protocol variant means adding cases to these switches and to the command tables in `alienfx_control.h`, not writing a new class.

`Functions::AlienFXProbeDevice` determines the API version from **VID + max HID packet size**, not a static device table — see the `switch (vidd) { case 0x187c: switch (checker) {...} }` logic. Known VIDs: `0x187c` Alienware, `0x0d62` Darfon (RGB keyboards), `0x0424` Microchip (monitors), `0x0461` Primax (mice), `0x04f2` Chicony (external keyboards). For Darfon specifically, the version isn't assigned from VID alone: `AlienFXProbeDevice` parses the interface's HID report descriptor (`hid_report_descriptor.h`/`.cpp`, `ParseTopLevelCollections`) and only claims the device as `API_V5` if it finds the AlienFX lighting collection (`usage_page 0xFF89`, `usage 0xCC`) — porting upstream's Windows `HIDP_CAPS.Usage == 0xcc` check, and getting the real Feature-report length (8 bytes) instead of the interrupt-IN endpoint's unrelated `wMaxPacketSize` (32). See `docs/hid-transport.md` for why. `Mappings::AlienFXEnumDevices` walks the libusb device list directly and probes each HID-class interface of a known-vendor device — one entry per real USB interface, so unlike an hidapi-based enumeration there's no need to dedupe multiple top-level-collection entries sharing one node.

`alienfx_control.h` holds the wire protocol as byte-template arrays per version (`COMMV1_*` .. `COMMV8_*`), each documented inline with its byte layout. `Functions::PrepareAndSend` copies a template into a buffer, applies positional patches (`Afx_icommand{offset, bytes}`), sets the report ID from `reportIDList[version]`, then dispatches to the version-appropriate transport (output report / feature report / raw write / write-then-read) in `libusb_helper.cpp`.

This is a **Linux port of a Windows codebase**; deltas from the original are called out with `// NOTE:` comments — e.g. packet lengths run one byte longer than on Windows, and the report-ID-0 byte is stripped before hitting the wire (compensated with `length++` in `AlienFXProbeDevice`, matching the strip in `libusb_helper.cpp`'s `ControlTransferReport`). `AlienFX-SDK/include/libusb_helper.h` + `src/libusb_helper.cpp` reimplement the Win32 HID API surface (`HidD_SetFeature`, `HidD_SetOutputReport`, `WriteFile`, `ReadFile`, `HidD_GetFeature`, `HidD_GetInputReport`) directly on libusb control/interrupt transfers so the rest of the SDK reads like the original Windows source — see `docs/hid-transport.md` for why this talks to libusb directly rather than through hidapi, and how it scopes every USB interface claim to a single transfer. Preserve the `// NOTE:` comments when touching protocol code — they encode reverse-engineered, hardware-verified behavior.

### AlienFX_SDK::Mappings: device registry + persistence

`Mappings` owns the libusb context, the `fxdevs` vector of detected devices, user-assigned light/device names, and zones (`Afx_group`, referencing devices by **PID** not array index). It **owns** the `Functions*` stored in each `Afx_device` and deletes them in its destructor and in `AlienFxUpdateDevice` — never manually delete a `Functions*` obtained from it.

Name/zone mappings persist as JSON at `$XDG_DATA_HOME/alienfx/mappings.json`, falling back to `$HOME/.local/share/alienfx/mappings.json` (`Mappings::GetMappingsPath`).

Standard usage sequence, as seen in both `alienfx-cli/src/main.cpp` (`initCli()`) and `Example-App/main.cpp`:

```
Mappings::LoadMappings() → Mappings::AlienFXEnumDevices() → Functions::Set*(...) → Functions::UpdateColors()
```

`UpdateColors()` is required — most `Set*` calls only stage state.

### AlienFan-SDK: sysfs, unrelated to HID

`AlienFan_SDK::Control` (`AlienFan-SDK/src/AlienFan-SDK.cpp`) has nothing to do with the HID/libusb path. It locates the `alienware_wmi` hwmon driver under `/sys/class/hwmon/*/name` (underscore) for fan/sensor RPM, and the `alienware-wmi` platform-profile driver under `/sys/class/platform-profile/*/name` (hyphen) for power profiles — note the differing separator between the two. Each feature area degrades independently to a `*Supported = false` flag when its driver node isn't found, rather than failing outright.

### alienfx-cli

Single-file CLI11 app (`alienfx-cli/src/main.cpp`). Each subcommand is registered with `app.add_subcommand(...)` and a callback lambda; hardware init (`initCli()`, which probes both SDKs and enumerates HID devices) is deferred via a shared `ensureInit()` closure so read-only commands like `getpowerprofile`/`supportedprofiles` don't touch USB at all.

## Conventions

- C++23 throughout (`CMAKE_CXX_STANDARD 23`); the SDK header pulls in `using namespace std;`.
- Logging via loguru (`LOG_S(...)` streams, `LOG_F(...)` printf-style); verbose/diagnostic logging is wrapped in `#ifdef DEBUG` and only active in Debug builds.
- No `.clang-format` is checked in. `AlienFX-SDK`/`alienfx-cli` follow a 4-space, Google-ish style; `AlienFan-SDK` follows a different (LLVM-ish) style. Match whatever the file you're editing already uses rather than reformatting wholesale.
- Commit messages use Conventional Commits with a scope, e.g. `feat(sdk): ...`, `fix(cli): ...`, `chore(readme): ...`.
- Do not add a `Co-Authored-By: Claude ...` trailer to commit messages.
- Do not use `git worktree` / the `EnterWorktree` tool in this repo. Work directly in the main checkout on whatever branch is already active (normally `main`) — do not create a separate worktree or branch for changes.
