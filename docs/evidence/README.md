# Evidence bundle: libusb kernel-driver acquisition, hang backtrace, hidraw-vs-libusb performance

Supporting material for the addendum in `../probe-keyboard-lockup.md`, gathered in response
to upstream feedback on that report. Machine used: `187c:0550` (Alienware AW-ELC LED
controller, API_V4) and `0d62:3740` (Darfon keyboard, API_V5) as described there.

> This bundle's own timing data (hidraw's ~425x slowdown vs. libusb control transfers, see
> `data/*-output.summary.txt`) is what motivated `../hid-transport.md`'s scoped-claim libusb
> rewrite, which supersedes the hidraw-backend switch this bundle otherwise documents. Two
> more items added for that rewrite:
>
> - `scripts/recipient-probe.c` + `scripts/build-recipient-probe.sh` -- checks, directly
>   against hardware, whether a HID class request can reach the Darfon's AlienFX Feature
>   report (`0xCC`) without claiming its keyboard interface (device-recipient or
>   unbound-interface-recipient, per `check_ctrlrecip()` in the kernel's usbfs). Result in
>   `data/recipient-probe-darfon.log`: both claim-free routes STALL, so a scoped claim on the
>   keyboard interface is unavoidable for this device -- see `../hid-transport.md`.
> - `data/hid-transport-verification.log` -- the scoped-claim rewrite exercised against both
>   devices (`status`, `setall`, `setone`), with `dmesg` confirming clean per-transfer
>   detach/reattach cycles on the Darfon and no loss of keyboard responsiveness.

## What's here

- `scripts/hidbench.c` + `scripts/build-hidbench.sh` -- a small hidapi microbenchmark for
  `hid_send_output_report` / `hid_write` / `hid_send_feature_report`, built once against each
  backend from this repo's own vendored `hidapi` source (no network needed; both static libs
  already exist in `../../build/_deps` and `../../build-libusb/_deps`).
- `scripts/run-libusb-bench.sh` -- runs the libusb-backend benchmark against `187c:0550`.
  **Needs root.**
- `scripts/safe-acquisition-demo.sh` -- demonstrates the libusb backend's kernel-driver
  detach/reattach on `187c:0550` (the LED controller, not a keyboard -- safe), captures a
  live gdb backtrace at the actual `libusb_detach_kernel_driver()` call. **Needs root.**
- `scripts/keyboard-repro-watchdog.sh` -- full end-to-end repro of the original bug on the
  real affected device (`0d62:3740`), with a self-contained hang (no human needs to not-type
  anything -- stdin is a FIFO this script itself holds open, under `setsid` so `/dev/tty` is
  unreachable) and a watchdog-armed automatic recovery. **Needs root, run attended.** See the
  comment block at the top of the script before running it.
- `data/` -- output from the above: benchmark CSVs + summaries, driver-bind timelines, gdb
  backtraces, dmesg excerpts.

## Why some of this had to be handed off

The environment that produced `hidraw-*` in `data/` (hidraw backend, via the udev ACL already
granting this user RW on `/dev/hidraw4`) has no controlling terminal and can't authenticate
`sudo` -- there's no TTY for a password prompt and no cached credential helper. Everything
that needs root (both `libusb-*` benchmark runs, the acquisition demo, and the keyboard
repro) is written as a standalone, already-reviewed script instead, meant to be run directly:

```bash
cmake -S . -B build-libusb -G Ninja -DALIENFX_BUILD_CLI=ON -DALIENFX_HID_BACKEND=libusb \
  -DCMAKE_BUILD_TYPE=Debug   # reuses already-fetched deps from build/_deps if configured there first
cmake --build build-libusb -j"$(nproc)"
bash docs/evidence/scripts/build-hidbench.sh

sudo bash docs/evidence/scripts/run-libusb-bench.sh
sudo bash docs/evidence/scripts/safe-acquisition-demo.sh
sudo bash docs/evidence/scripts/keyboard-repro-watchdog.sh   # read its header comment first
```

Each script is self-contained, prints what it's doing, and writes its results into `data/`.

## Why the benchmark measures raw transport latency, not full commands

`hidbench` sends a zeroed report of the SDK's real length (34 bytes for `API_V4` on
`187c:0550` -- see `AlienFX-SDK/src/AlienFX_SDK.cpp`'s `AlienFXProbeDevice`) rather than a
real lighting command. The content doesn't change which USB transfer type carries it
(control vs. interrupt), which is the entire question at hand: whether hidraw is slower than
libusb for the SDK's actual write path, and why. The `feature` mode is included specifically
as a same-backend control experiment -- it's a control transfer under *both* backends, so it
should show no meaningful gap; if it did, that would mean the harness itself was measuring
noise rather than a real backend difference, and the `output`/`write` results shouldn't be
trusted either.

## Results: hidraw vs libusb, same device, same calls (n=200, 34-byte reports, `187c:0550`)

| hidapi call | wire path | hidraw mean/p50 | libusb mean/p50 |
|---|---|---|---|
| `hid_send_feature_report` | control transfer, both backends | 126.6us / 120us | 146.0us / 139us |
| `hid_send_output_report` | **interrupt-OUT** (hidraw) vs **control** (libusb) | 63.8ms / 64.0ms | **150.2us / 140us** |
| `hid_write` | interrupt-OUT, both backends (device has an OUT endpoint) | 63.7ms / 64.0ms | 63.7ms / 64.0ms |

Raw data: `data/hidraw-{feature,output,write}.csv` and `data/libusb-{feature,output,write}.csv`
(`.summary.txt` alongside each).

This is a clean confirmation of the theory, in both directions:

- **`hid_send_feature_report`** is a control transfer under both backends (`linux/hid.c:1216`
  `ioctl(HIDIOCSFEATURE)` vs `libusb/hid.c` control transfer in `hid_send_feature_report`) --
  and indeed lands within the same order of magnitude on both (127us vs 146us). This is the
  control experiment: if backends disagreed here, the harness itself would be the explanation
  for any other gap. They don't, so they aren't.
- **`hid_send_output_report`** is where the backends genuinely differ in *which endpoint they
  use*, not just in speed: hidraw's `ioctl(HIDIOCSOUTPUT)` (`linux/hid.c:1254`) rides the
  interrupt-OUT endpoint (`bEndpointAddress=0x01`, `bInterval=100` on this device -- confirm
  against your own `lsusb -v -d 187c:0550`), while libusb's version (`libusb/hid.c:1657`)
  issues a control-endpoint `SET_REPORT` instead. Result: **63.8ms vs 150us -- a ~425x gap**,
  on exactly the call this SDK's `API_V2`/`V3`/`V4` write path uses
  (`PrepareAndSend` -> `HidD_SetOutputReport`). This is upstream's performance complaint,
  confirmed and explained rather than disputed.
- **`hid_write`** uses the interrupt-OUT endpoint on *both* backends when the device exposes
  one (`libusb/hid.c:1442`, `linux/hid.c:1114`), and indeed lands at the same ~64ms on both --
  the endpoint's `bInterval`, not the backend, is what's actually being measured here. (This
  SDK doesn't call `hid_write()` for `API_V4`; included as a second confirmation that the
  interrupt endpoint itself, not "hidraw" as a category, is what's slow.)

### A byproduct: the safe acquisition demo's own numbers show why its sysfs sample missed the window

`scripts/safe-acquisition-demo.sh` runs 100 `hid_send_output_report` iterations against
`187c:0550` under libusb, then samples `/sys/.../driver` mid-run to catch the detach live.
It missed: `data/safe-demo-bench.summary.txt` shows that run averaged **155.8us/iteration**
(min 132us, p99 328us) -- the *entire* 100-iteration burst finished in roughly 15ms, well
under the script's 0.3s pre-sample delay, which was sized for hidraw's ~64ms/iteration before
this data existed. `data/safe-demo-driver-timeline.log` shows `usbhid` bound at all three
checkpoints as a result -- not because nothing was detached, but because detach-claim-release-
reattach for this whole run round-trips faster than a human (or a fixed `sleep`) can sample.
The acquisition itself is proven directly instead, by breakpoint rather than by sampling:

```
Thread 1 "hidbench-libusb" hit Breakpoint 1, libusb_detach_kernel_driver
    (dev_handle=0x5555555ab920, interface_number=0)
    at .../libusb-src/libusb/libusb/core.c:2180
#0  libusb_detach_kernel_driver (...) at libusb/core.c:2180
#1  hidapi_initialize_device (...) at libusb-src/libusb/hid.c:1174
#2  hid_open_path (path=0x... "3-7:1.0") at libusb-src/libusb/hid.c:1315
#3  hid_open (vendor_id=6268, product_id=1360, serial_number=0x0) at libusb-src/libusb/hid.c:946
#4  main ()
```

Full log: `data/safe-demo-gdb-backtrace.log`. `vendor_id=6268` / `product_id=1360` are
`0x187c`/`0x0550` in decimal -- this is unambiguously the LED controller, not the keyboard.

## The keyboard repro (`scripts/keyboard-repro-watchdog.sh`), against the real `0d62:3740`

Ran clean: `data/kbd-repro-driver-timeline.log` shows the Darfon's `3-8:1.1` interface
detached ("no `usbhid` on that path") within the 10s poll window after launching
`probe --yes`, then back on `usbhid` ~2.2s later via the script's own `recover()` -- the
45s backstop watchdog never had to fire (`data/kbd-repro-watchdog.log` doesn't exist).
`data/kbd-repro-dmesg.log` shows the kernel cleanly re-registering the keyboard's input
devices and hidraw node after recovery. The keyboard was confirmed physically responsive
afterward.

The captured hang (`data/kbd-repro-hang-backtrace.log`, `thread apply all bt` on the blocked
process) shows all four threads:

```
Thread 1 "alienfx_cli" (main): #3 poll() #4 ReadLineTrimmed() at main.cpp:185
                                #5 operator() at main.cpp:728
Thread 2/3 "alienfx_cli" (reader threads, one per open device):
                                pthread_cond_timedwait / poll()
                                -> libusb_handle_events -> read_thread at hid.c:1055
Thread 4 "libusb_event":       poll() -> linux_udev_event_thread_main (hotplug monitor)
```

Every thread is idle (`poll()` / `pthread_cond_timedwait`); none are inside
`libusb_detach_kernel_driver`, `libusb_claim_interface`, or any other acquisition code. The
main thread is blocked in the interactive per-device naming prompt
(`alienfx-cli/src/main.cpp:728`, "New name (ENTER to skip):") -- reached even with
`probe --yes`, since that flag only gates the earlier y/N confirmation, not this prompt (see
the follow-up in `../probe-keyboard-lockup.md`). `data/kbd-repro-probe-stdout.log` shows both
devices (`187c:0550` and `0d62:3740`) were already open -- and the keyboard interface already
detached -- before this prompt was ever reached, confirming the enumeration-time detach and
the later prompt-time block are independent events that happen to compose.
