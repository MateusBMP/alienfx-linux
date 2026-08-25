# `alienfx_cli probe` can disable a keyboard and then deadlock waiting for it

> **Superseded.** The hidraw-backend switch this report proposed (§Proposed
> fix below) was the fix at the time, but it traded away the performance
> upstream had for a good reason (see §Addendum). The SDK has since moved
> back to libusb, keeping the safety property this report cares about by a
> different means: every HID transfer claims -- and, where a kernel driver
> owns the interface, detaches -- only for the duration of that one
> transfer, never across an interactive prompt. See
> [`docs/hid-transport.md`](hid-transport.md) for the current design and the
> hardware evidence for it; the root-cause analysis below (§Root cause,
> §Why the Windows original doesn't hit this) still applies unchanged.

## Summary

On hardware where an AlienFX-family vendor ID also owns the machine's actual
keyboard (Darfon, VID `0x0d62` — used both for some Alienware per-key RGB
keyboards and as a plain USB-keyboard vendor), running `sudo ./alienfx_cli
probe` detaches the kernel's `usbhid` driver from that keyboard's interface
as a side effect of opening it, then immediately blocks on an interactive
stdin prompt (`Do you want to set devices and lights names? (y/N)`) that the
now-unresponsive keyboard cannot answer. There is no way to recover short of
a second input device, SSH from another machine, or a reboot.

This report explains the root cause, why the Windows original doesn't have
this problem, and a fix (already implemented against this fork, described
below) that removes the underlying cause rather than working around it.

## Impact and who is affected

Any machine where a HID device that `AlienFXProbeDevice` can match also
exposes a HID *input* collection (keyboard/mouse) is affected. Concretely,
of the five vendor IDs the SDK recognizes:

- `0x0d62` (Darfon) — the vendor of some Alienware per-key RGB keyboards, and
  also a generic keyboard-controller vendor. **Confirmed affected.**
- `0x0461` (Primax) — mice.
- `0x04f2` (Chicony) — external keyboards.

are all vendors whose chips are just as likely to be the input device itself
as to be an AlienFX-only lighting controller. `0x187c` (Alienware) and
`0x0424` (Microchip, monitors) are not typically also input devices, so they
are lower risk in practice, though the same enumeration issue (§Root cause,
step 3) applies to all of them.

## Reproduction

```
$ sudo ./build/alienfx_cli probe
...
+++++ Detected as: HID 0d62:3740, APIv5 +++++
...
Do you want to set devices and lights names? (y/N)
```

At this point the built-in keyboard stops producing input. The process is
blocked in `getline()` and cannot be answered. `Ctrl-C` does not help (see
step 6 below). The only recovery is from another input path.

## Root cause

1. `AlienFX-SDK/CMakeLists.txt` set `HIDAPI_WITH_LIBUSB ON` /
   `HIDAPI_WITH_HIDRAW OFF` and linked `hidapi::libusb` — the SDK used
   hidapi's **libusb** transport backend exclusively.
2. hidapi's libusb backend unconditionally compiles
   `#define DETACH_KERNEL_DRIVER` (`libusb/hid.c`, non-`__FreeBSD__` builds).
   Every `hid_open_path()` call therefore does:
   - `libusb_kernel_driver_active()` / `libusb_detach_kernel_driver()` on the
     interface it's about to open, then `libusb_claim_interface()`.
   - Spawns a background thread that continuously resubmits an interrupt-IN
     transfer for as long as the handle stays open.
   - Reattaches the kernel driver **only** inside `hid_close()`.
3. `Mappings::AlienFXEnumDevices` enumerates every HID device on the system
   (`hid_enumerate(0x0, 0x0)`) and, for each one, calls
   `Functions::AlienFXProbeDevice(vid, pid, path)`, which maps VID `0x0d62`
   **unconditionally** to `API_V5` and immediately opens it — with no
   filtering by usage page, usage, or interface number, and no de-duplication
   of devices that enumerate as multiple HID collections sharing one node.
4. On the affected hardware, the Darfon device (`0d62:3740`) exposes two USB
   interfaces: interface 0 is vendor-class (`bInterfaceClass=0xFF`, no
   kernel driver, not visible to hidapi's libusb backend at all), and
   interface 1 is a standard boot-protocol keyboard
   (`bInterfaceClass=0x03`, bound to `usbhid`) — this **is** the physical
   keyboard. Because interface 0 is invisible to that backend, interface 1
   is the *only* one it can open, and that is exactly what gets detached.
5. `~Mappings` → `~Functions` → `hid_close()` — the only code path that
   reattaches the kernel driver — runs solely at normal process exit
   (destruction of the file-scope `static AlienFX_SDK::Mappings afx_map`),
   which never happens while the process is blocked mid-prompt.
6. Worse: `loguru::init()` was called with its default `Options`, which
   capture `SIGINT`/`SIGTERM` and end in a handler that re-raises the signal
   with `SIG_DFL` after restoring the default disposition. That kills the
   process **without running static destructors**, so even `Ctrl-C` could
   not trigger the reattach.

## Why the Windows original doesn't hit this

Two structural differences, neither of which ports to Linux for free:

- It opens with `CreateFile(..., FILE_SHARE_READ | FILE_SHARE_WRITE, ...)`
  — a *non-exclusive* open that never touches driver ownership.
- Windows' HID class driver exposes each HID **top-level collection** as its
  own device interface path. The keyboard collection and the vendor lighting
  collection of a composite device are different `CreateFile` targets, so
  opening one never has any bearing on the other.

The Windows code also applies a guard this port dropped when probing Darfon:

```cpp
case 0x0d62: // Darfon
    if (caps.Usage == 0xcc && !length) {
        length = caps.FeatureReportByteLength;
        version = API_V5;
    }
    break;
```

only `Usage == 0xcc` (the vendor lighting collection) qualifies for
`API_V5` — the keyboard collection (`Usage == 0x06`, Generic Desktop) never
matches. The Linux port's equivalent code accepts **any** device with VID
`0x0d62`, with no usage check at all, because usage-page/usage information
wasn't available from the transport being used (see next section).

## Proposed fix (implemented in this fork)

The headline fix is switching hidapi's transport backend from **libusb to
hidraw**. hidraw's `hid_open_path()` is a plain, non-exclusive `open(2)` of
`/dev/hidrawN` — it never touches kernel-driver ownership, never claims an
interface, and starts no background reader thread. Multiple processes
(including the kernel's own `usbhid`) can hold a hidraw node open
simultaneously without interfering with each other. As a secondary benefit,
hidraw's `hid_enumerate()` populates real `usage_page`/`usage` per
top-level collection (read from the kernel's report-descriptor sysfs node,
without opening the device) — libusb's backend leaves both at 0 unless a
build-time `INVASIVE_GET_USAGE` flag most builds don't set, which is what
made a Windows-equivalent usage check impossible before this change.

Full change set, in order (each independently buildable/testable):

1. **Fix an out-of-bounds read**: `AlienFXProbeDevice` indexed
   `reportIDList[version]` *before* checking `version == API_UNKNOWN`
   (`API_UNKNOWN == -1`), reading one byte before the array for every
   non-matching HID device on the bus. Hoisted the early return above the
   lookup.
2. **Fix a dangling pointer**: `Functions::path` stored the raw `char*`
   from a `hid_device_info` entry that `hid_free_enumeration()` frees
   immediately after the enumeration loop. Every surviving `Functions`
   object held a dangling pointer for the rest of the process. Made it an
   owned `std::string`.
3. **Stop loguru swallowing SIGINT/SIGTERM**, and make an interrupted prompt
   actually recoverable: disabled loguru's signal capture, installed our own
   handlers, and reworked the interactive prompt helper to wait on `poll()`
   (reliably interruptible via `EINTR`, unlike a buffered stdio read) before
   reading a line. A signal now throws an exception that unwinds cleanly out
   of `main()`, letting `afx_map`'s destructor close every open HID handle
   instead of the process dying with devices still open.
4. **Added `probe --dry-run` and `probe --yes`**, made prompts read from
   `/dev/tty` instead of being at the mercy of whatever stdin is redirected
   to, and moved the safety warning (with recovery commands) to print
   *before* any device is opened, not after.
5. **Filtered enumeration to the five known AlienFX vendor IDs and
   deduplicated by hidraw path** before probing at all — behavior-preserving
   (no other VID can match `AlienFXProbeDevice`'s version switch) and
   strictly safer, since this process then never opens an unrelated HID
   device. The dedupe matters specifically because hidraw's per-collection
   enumeration means a single composite device (like the Darfon, which
   enumerates as four collections on one node) would otherwise be opened
   multiple times in a row before the existing VID/PID-based dedupe in
   `AlienFxUpdateDevice` got a chance to close the extras.
6. **The backend switch itself**: a new `ALIENFX_HID_BACKEND` CMake cache
   option, `hidraw` by default, with `libusb` kept available as an
   explicitly-warned opt-out (for hardware where an AlienFX interface has no
   hidraw node at all — see Known follow-ups). Building with hidraw needs
   libudev development files (checked at configure time with a clear error
   naming the distro package); libusb itself stays linked regardless, since
   `GetMaxPacketSize()` — the API-version detection heuristic — is a
   read-only descriptor inspection unrelated to which hidapi backend is
   used. Building `alienfx_cli` against `ALIENFX_HID_BACKEND=libusb` also
   now refuses to run `probe` interactively (without `--yes`) as a last-line
   defense.
7. **Pin FetchContent dependencies** to release tags instead of tracking
   `main`/`master` unpinned: libusb-cmake, hidapi, nlohmann/json, and CLI11
   all pin cleanly. loguru is the one exception — it has never cut a release
   tag with CMake support at all (its `CMakeLists.txt`, and the
   `loguru::loguru` target this project links against, exist only on
   `master`; its latest tag predates both and fails to configure). Pinned to
   a specific `master` commit instead, which still gets reproducibility
   without a buildable tag to pin to.

### One behavioral delta worth calling out

hidapi's `hid_send_output_report` maps to `HidD_SetOutputReport` on Windows
— always the control endpoint there and on hidapi's libusb backend, but
hidraw's equivalent ioctl (`HIDIOCSOUTPUT`) prefers the interrupt-OUT
endpoint when the interface has one. On this test machine the affected
device (`187c:0550`, `API_V4`) does have an interrupt-OUT endpoint, so this
device's write path changes transport under the new backend. It was
verified against real hardware (`setall` producing the expected error-free
run with device access available via an existing udev ACL) rather than
assumed; see Verification below.

## Recovery for an affected user (today, unpatched)

From a second input path (SSH, external keyboard, etc.):

```bash
sudo pkill alienfx_cli
# Find the keyboard's interface first, e.g.:
#   lsusb -t   (or)  grep -l Keyboard /sys/bus/usb/devices/*/product 2>/dev/null
echo -n '<bus-port:iface>' | sudo tee /sys/bus/usb/drivers/usbhid/bind
ls -l /sys/bus/usb/devices/<bus-port:iface>/driver   # should now point at usbhid
```

If that doesn't bring it back, a full re-enumerate of the device usually
does:

```bash
echo -n '<bus-port>' | sudo tee /sys/bus/usb/drivers/usb/unbind
echo -n '<bus-port>' | sudo tee /sys/bus/usb/drivers/usb/bind
```

## Verification performed

- Clean rebuild with the new default (`rm -rf build && cmake -S . -B build
  -G Ninja -DALIENFX_BUILD_CLI=ON && cmake --build build`), including the
  pinned dependency versions, confirms `libhidapi-hidraw.a` is the library
  actually built and linked (no `libhidapi-libusb.a` in the tree) and that
  the whole tree still configures and builds cleanly end to end.
- `probe --dry-run` (needs no root) now shows real per-collection
  `usage_page`/`usage` values, confirming hidraw's report-descriptor-based
  enumeration is active: the Darfon node correctly shows four collections
  on one `/dev/hidrawN` path — Generic Desktop/Keyboard (`0x01`/`0x06`),
  Consumer (`0x0C`/`0x01`), Generic Desktop/Wireless Radio (`0x01`/`0x0C`),
  and the vendor lighting collection (`0xFF89`/`0xCC`) — and that the
  keyboard's `usbhid` driver binding is unaffected by running it, before
  and after.
- On this test machine, `/dev/hidraw4` (the Alienware light-controller
  node, `187c:0550`) carries a udev ACL granting the logged-in user RW
  access independent of `alienfx_cli`'s own root requirement, which allowed
  running `status` and `setall` against real hardware over the new hidraw
  path without root and without touching the higher-risk keyboard node at
  all. Both completed with a clean exit and no protocol errors.
- The keyboard-specific interactive path (`sudo ./alienfx_cli probe`
  answered from the affected keyboard itself, plus a `Ctrl-C` recovery
  test) needs to be exercised interactively with root by whoever verifies
  this on their own hardware — that step could not be performed from this
  non-interactive environment. A `pty`-based automated test of the
  SIGINT-recovery path did confirm that a process genuinely blocked in
  `poll()` on the confirmation prompt now exits cleanly (code 0, not
  killed by signal) on `SIGINT`, with `Mappings`' destructor observed
  running before exit.

## Known follow-ups found in passing (deliberately not bundled into this fix)

Bundling these would make any regression from the backend switch
unattributable, so they're listed here as separate future work instead:

- `libusb_helper.cpp`'s four `bool`-returning wrappers around
  `hid_send_output_report`/`hid_send_feature_report`/etc. convert hidapi's
  `-1` error return into `true`, so a failed HID write currently reads as
  success throughout the SDK.
- `GetDeviceStatus()` passes an **uninitialized** `buffer[0]` (the report
  number both transports read from that byte) into
  `HidD_GetFeature`/`HidD_GetInputReport`.
- The V5 (Darfon) `length` is currently derived from `wMaxPacketSize` (32
  bytes), but the actual feature report (`0xCC`) the device implements is 8
  bytes end-to-end. Every V5 command is a guaranteed hardware `STALL`, on
  either transport backend — this specific keyboard's AlienFX collection
  has, as far as could be determined, never actually worked through this
  SDK. Deciding whether and how to drive it at all (versus filtering it out
  entirely) is separate work.
- `hid_read()` on the V7 (Primax) code path can block forever if the device
  never produces an input report; not exercised by any hardware in this
  report but worth a timeout regardless.
- The `Functions::devID` union (`{pid, vid}` aliased with `unsigned long
  devID`) leaves 4 bytes of padding indeterminate; `Functions::devID` is
  never actually read anywhere, but the equivalent pattern recurs in
  `Afx_device` where it *is* used for lookups — worth auditing separately.
- Not every HID interface has a corresponding hidraw node — a vendor
  interface can be `bInterfaceClass` other than `0x03`/HID entirely, in
  which case hidraw cannot reach it at all and only raw libusb (with the
  detach behavior this fix removes) could. This did not apply to either
  device on the test hardware used here (both AlienFX collections did have
  hidraw nodes), but a build configured with `-DALIENFX_HID_BACKEND=libusb`
  remains available, clearly warned, for hardware where it would.
- `probe --yes` only skips the top-level "Do you want to set devices and
  lights names?" confirmation (`alienfx-cli/src/main.cpp`, the `if
  (!probe_yes)` guard around that prompt). The unconditional per-device
  "New name (ENTER to skip):" and per-light naming prompts right after it are
  *not* gated by `--yes` — on a `libusb`-backend build, `probe --yes` still
  reaches an unanswerable `ReadLineTrimmed()` after opening every detected
  device, just one prompt later than before. Found while building the
  addendum below's keyboard repro, which uses exactly this path. Worth
  gating those too if `libusb` stays available as an opt-out backend.

## Addendum: response to upstream review

Upstream (T-Troll/alienfx-tools) responded to this report by defending
`libusb` on performance grounds and challenging two things: whether the
kernel-driver detach is really `libusb`-specific (citing Tron hardware
working fine), and asking for the acquisition call site plus a real hang
backtrace. This addendum answers both, with sources and measurements taken
on this fork's test hardware (`187c:0550` Alienware LED controller, API_V4;
`0d62:3740` Darfon keyboard, API_V5), against hidapi pinned at
`hidapi-0.15.0` (commit `d6b2a974608dec3b76fb1e36c189f22b9cf3650c`, as
vendored by this repo's own `FetchContent`).

### Where the acquisition happens, and why it isn't device-specific

`libusb/hid.c` compiles `#define DETACH_KERNEL_DRIVER` unconditionally on
every non-FreeBSD build (line 68-70) — this is not conditional on VID/PID,
device class, or anything else. Every `hid_open()`/`hid_open_path()` call
that succeeds runs `hidapi_initialize_device()`, which:

- detaches the kernel driver at line 1173-1174
  (`libusb_kernel_driver_active()` → `libusb_detach_kernel_driver()`),
- claims the interface at line 1185 (`libusb_claim_interface()`),
- and only reattaches inside `hid_close()`, line 1728
  (`libusb_attach_kernel_driver()`), which never runs while a process holds
  the handle open.

So the claim "it works fine for Tron" isn't evidence the detach is
Darfon-specific — it's evidence that *Tron's* AlienFX interface isn't also
its keyboard. USB kernel-driver ownership is per **USB interface**, not per
HID **collection** (Windows' HID class driver exposes each top-level
collection as its own `CreateFile` target — that's the actual reason the
Windows original never hits this). This Darfon keyboard's report descriptor
(`docs/evidence/README.md`, or read `/sys/class/hidraw/hidrawN/device/report_descriptor`
yourself for the node bound to VID `0d62`) puts four top-level HID
collections — keyboard, consumer controls, wireless-radio, and the vendor
`0xFF89`/usage `0xCC` lighting collection — on **one** interface (interface
1, the boot-protocol keyboard); interface 0 is vendor-class with zero
endpoints, invisible to hidapi's libusb backend entirely
(`should_enumerate_interface()`, line 776, only admits `LIBUSB_CLASS_HID`).
There is no "open the lighting collection instead" option under `libusb` on
this hardware — the interface *is* the keyboard, full stop. `docs/evidence/scripts/safe-acquisition-demo.sh`
reproduces the identical detach/claim/reattach sequence against `187c:0550`
(the LED controller) instead, to show it isn't Darfon-specific either — it's
just harmless there because nothing depends on that interface's `usbhid`
binding. Captured live with a breakpoint on the real call
(`docs/evidence/data/safe-demo-gdb-backtrace.log`):

```
Thread 1 "hidbench-libusb" hit Breakpoint 1, libusb_detach_kernel_driver
    (dev_handle=0x5555555ab920, interface_number=0) at libusb/core.c:2180
#0  libusb_detach_kernel_driver (...) at libusb/core.c:2180
#1  hidapi_initialize_device (...) at libusb-src/libusb/hid.c:1174
#2  hid_open_path (path=0x... "3-7:1.0") at libusb-src/libusb/hid.c:1315
#3  hid_open (vendor_id=6268, product_id=1360, serial_number=0x0) at libusb-src/libusb/hid.c:946
#4  main ()
```

(`vendor_id=6268`/`product_id=1360` decimal = `0x187c`/`0x0550` — unambiguously
the LED controller, opened by nothing more than `hid_open(vid, pid, NULL)`.)

### The hang backtrace

`docs/evidence/scripts/keyboard-repro-watchdog.sh` reproduces the original
hang end-to-end against the real `0d62:3740` device: it launches
`probe --yes` under `setsid` (so `/dev/tty` is unreachable) with stdin on a
FIFO the script itself holds open (so the read never sees EOF either) — the
process reaches the exact same unanswerable prompt as the original report,
with no human needing to withhold input to get there, and a watchdog armed
before anything opens as an automatic backstop. Ran clean: the keyboard's
`3-8:1.1` interface was confirmed detached, the script's own recovery
rebound it to `usbhid` in ~2.2s (well inside the 45s backstop, which never
had to fire), and the keyboard was confirmed physically responsive
afterward. `docs/evidence/data/kbd-repro-hang-backtrace.log`
(`thread apply all bt` on the genuinely blocked process):

```
Thread 1 "alienfx_cli" (main): #3 poll() #4 ReadLineTrimmed() at main.cpp:185
                                #5 operator() at main.cpp:728
Thread 2/3 "alienfx_cli" (reader threads, one per open device):
                                pthread_cond_timedwait / poll()
                                -> libusb_handle_events -> read_thread at hid.c:1055
Thread 4 "libusb_event":       poll() -> linux_udev_event_thread_main (hotplug monitor)
```

Every thread is idle. None are inside `libusb_detach_kernel_driver`,
`libusb_claim_interface`, or any other acquisition code — confirming
**upstream is right that `libusb` itself isn't what's blocking**. The
original report's word "deadlock" overstated it: the main thread is parked
in the interactive per-device naming prompt
(`alienfx-cli/src/main.cpp:728`, "New name (ENTER to skip):"), reached even
with `probe --yes` since that flag only gates the earlier y/N confirmation
(see the follow-up above). The process log
(`docs/evidence/data/kbd-repro-probe-stdout.log`) shows both devices already
open — and the keyboard interface already detached — before this prompt was
ever reached: enumeration-time detach and prompt-time block are two
independent events that happen to compose. Neither half alone is a bug in
the other's code; together they disable input with no in-process recovery
path, which was the entire point of switching the default backend,
independent of whether "hang" was the precisely correct word for it.

### Performance: upstream has a real point, with a mechanism

Measured with `docs/evidence/scripts/hidbench.c` against `187c:0550`, both
backends, n=200, 34-byte reports (this device's real `API_V4` length):

| hidapi call | wire path | hidraw mean/p50 | libusb mean/p50 |
|---|---|---|---|
| `hid_send_feature_report` | control transfer, both backends | 126.6us / 120us | 146.0us / 139us |
| `hid_send_output_report` | interrupt-OUT (hidraw) vs. control (libusb) | 63.8ms / 64.0ms | **150.2us / 140us** |
| `hid_write` | interrupt-OUT, both backends | 63.7ms / 64.0ms | 63.7ms / 64.0ms |

Full data and methodology: `docs/evidence/README.md`. The mechanism: this
device's OUT endpoint (`0x01`, `wMaxPacketSize=33`) descriptor has
`bInterval=100`, i.e. USB scheduling only guarantees it a slot once per
100ms at full speed. hidraw's `hid_send_output_report`
(`linux/hid.c:1242-1258`, `ioctl(HIDIOCSOUTPUT)`) and `hid_write`
(`linux/hid.c:1104-1120`) both go over that interrupt-OUT endpoint. The
**libusb** backend's `hid_send_output_report` (`libusb/hid.c:1645-1672`)
instead uses a **control**-endpoint `SET_REPORT`, which has no polling
interval — confirmed as a real, descriptor-level **~425x** latency
difference (63.8ms vs 150us) on exactly the call the SDK's
`API_V2`/`V3`/`V4` code path uses (`PrepareAndSend` → `HidD_SetOutputReport`).
`hid_send_feature_report` is a control transfer under both backends
(`API_V5`/`V8` in the SDK) and confirms the control experiment: 127us vs.
146us, no meaningful gap — which is what makes the `output` result credible
rather than harness noise. `hid_write` confirms the same theory from the
other direction: it uses the interrupt-OUT endpoint on *both* backends when
one exists (`libusb/hid.c:1442`, `linux/hid.c:1114`), and lands at the same
~64ms either way — it's the endpoint's `bInterval`, not "hidraw" as a
category, that's slow. (`API_V4` doesn't call `hid_write()`; included as a
second confirmation of the mechanism.)

### Where this leaves things

Upstream is right that `hidraw` is meaningfully slower on this specific
call, for a real, identifiable reason (endpoint choice, not implementation
quality), and right that `libusb` itself isn't what blocks the process during
the hang. Upstream's "unrelated to other devices" framing doesn't hold up:
the detach is unconditional in hidapi's `libusb` backend on every device it
opens; it just isn't visible on hardware where the AlienFX interface and a
real input device aren't the same USB interface.

Proposed way to keep both properties instead of picking one backend
globally:

- Keep `hidraw` as the default — it's the only backend able to open the
  `0d62:3740` lighting collection at all without taking the keyboard
  interface, since that collection has no separate USB interface to open.
- Port the Windows original's usage guard (`caps.Usage == 0xcc`) into
  `AlienFXProbeDevice`'s Darfon case — hidraw's `hid_enumerate()` now
  populates real `usage_page`/`usage` per collection, which the `libusb`
  backend never did, so this guard is implementable on Linux for the first
  time.
- Close the output-report latency gap without reverting the backend:
  investigate the `usbhid` kernel quirk
  `HID_QUIRK_NO_OUTPUT_REPORTS_ON_INTR_EP` (shippable as a udev/hwdb rule
  scoped to `187c:0550`) to force output reports back onto the control
  endpoint under `hidraw`; prefer `hid_send_feature_report` over
  `hid_send_output_report` where a device's descriptor exposes the same
  command as a feature report; keep `-DALIENFX_HID_BACKEND=libusb` available
  for hardware whose AlienFX interface is genuinely standalone, where the
  interval cost is real and the detach risk isn't.
- Fix the `API_V5` length bug noted above (32 from `wMaxPacketSize` vs. the
  descriptor's actual 8-byte feature report) so the Darfon lighting
  collection can be addressed at all, once it's being opened correctly.
- Gate the per-device/per-light naming prompts behind `--yes` too (see the
  follow-up above), so a `libusb`-backend `probe --yes` can't still reach an
  unanswerable prompt.
