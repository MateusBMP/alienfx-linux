Thanks for the detailed reply — this deserves a real answer rather than just restating the
report, so I went and measured/traced both things you raised.

**On performance: you're right, and here's the actual mechanism — measured, not just
theorized.** I benchmarked `hid_send_output_report`/`hid_write`/`hid_send_feature_report` on
the real `187c:0550` LED controller, n=200, real 34-byte `API_V4` reports, **both backends,
same device, same calls**:

| call | wire path | hidraw | libusb |
|---|---|---|---|
| `hid_send_feature_report` | control transfer, both backends | 127us mean / 120us p50 | 146us mean / 139us p50 |
| `hid_send_output_report` | interrupt-OUT (hidraw) vs. control (libusb) | 63.8ms / 64.0ms | **150us / 140us** |
| `hid_write` | interrupt-OUT, both backends | 63.7ms / 64.0ms | 63.7ms / 64.0ms |

This device's OUT endpoint descriptor has `bInterval=100` — USB only guarantees it a slot
once per 100ms at full speed. hidraw's `hid_send_output_report`
(`linux/hid.c:1242`, `ioctl(HIDIOCSOUTPUT)`) rides that endpoint; the libusb backend's
version of the same call (`libusb/hid.c:1657`) uses a **control**-endpoint `SET_REPORT`
instead, which has no polling interval — confirmed as a real **~425x** gap (63.8ms vs
150us) on exactly the call our `API_V2/V3/V4` write path uses.
`hid_send_feature_report` is control on both backends (what `API_V5`/`V8` use) and shows no
such gap — that's the control experiment, and it's what makes the `output` result credible
rather than harness noise. `hid_write` confirms it from the other side: it uses the
interrupt-OUT endpoint on *both* backends when one exists, and lands at the same ~64ms on
both — it's the endpoint's `bInterval`, not "hidraw" as a category, that's slow.

**On "why does it work fine for Tron": it's not that the detach is Darfon-specific — it's
that ownership is per USB *interface*, not per HID *collection*, and Tron's lighting
interface almost certainly isn't also an input device.** hidapi's libusb backend detaches
the kernel driver from *whatever interface it opens*, unconditionally, on every non-FreeBSD
build:

- `libusb/hid.c:68-70` — `#define DETACH_KERNEL_DRIVER`, no VID/PID/class check.
- `libusb/hid.c:1169-1197`, specifically **1173-1174** (`libusb_kernel_driver_active` →
  `libusb_detach_kernel_driver`) and **1185** (`libusb_claim_interface`) — this runs inside
  `hidapi_initialize_device()`, called from every successful `hid_open`/`hid_open_path`.
- Reattach is only in `hid_close()`, **1728** — never runs while a handle's still open.

(all against hidapi `d6b2a97`, i.e. the `hidapi-0.15.0` tag this repo pins)

The Darfon keyboard's report descriptor puts *four* HID collections — the keyboard itself,
consumer controls, wireless-radio, and the vendor `0xFF89`/usage `0xCC` lighting collection
— on one USB interface (interface 1, the boot-protocol keyboard). Interface 0 is
vendor-class with zero endpoints, which the libusb backend can't even see
(`should_enumerate_interface()` only admits `LIBUSB_CLASS_HID`, line 776). So on this
hardware there's no "open the lighting part instead" option under libusb — the interface
*is* the keyboard. I reproduced the identical detach/claim/reattach sequence against the LED
controller instead (a device nothing else depends on), with an actual breakpoint on the real
call:

```
Thread 1 "hidbench-libusb" hit Breakpoint 1, libusb_detach_kernel_driver
    (dev_handle=0x5555555ab920, interface_number=0) at libusb/core.c:2180
#0  libusb_detach_kernel_driver (...) at libusb/core.c:2180
#1  hidapi_initialize_device (...) at libusb-src/libusb/hid.c:1174
#2  hid_open_path (path=0x... "3-7:1.0") at libusb-src/libusb/hid.c:1315
#3  hid_open (vendor_id=6268, product_id=1360, serial_number=0x0) at libusb-src/libusb/hid.c:946
#4  main ()
```

`vendor_id=6268`/`product_id=1360` decimal = `0x187c`/`0x0550` — the LED controller,
unambiguously, opened by nothing more than `hid_open(vid, pid, NULL)`. Same code path,
harmless here only because nothing needs that interface's usbhid binding.

**On the backtrace of the hang itself:** built a repro that reaches the exact same
unanswerable prompt without needing a human to withhold input (stdin on a held-open FIFO,
under `setsid` so `/dev/tty` is unreachable), with a watchdog armed before anything opens.
Ran clean against the real `0d62:3740` keyboard: detach confirmed, the script's own recovery
rebound `usbhid` in ~2.2s, watchdog never had to fire, keyboard confirmed physically
responsive afterward. `thread apply all bt` on the genuinely blocked process:

```
Thread 1 "alienfx_cli" (main): #3 poll() #4 ReadLineTrimmed() at main.cpp:185
                                #5 operator() at main.cpp:728
Thread 2/3 (reader threads, one per open device): pthread_cond_timedwait / poll()
                                -> libusb_handle_events -> read_thread at hid.c:1055
Thread 4 "libusb_event":       poll() -> linux_udev_event_thread_main (hotplug monitor)
```

Every thread idle. None inside `libusb_detach_kernel_driver`, `libusb_claim_interface`, or
any acquisition code — **you're right, libusb itself isn't what's blocking.** "Deadlock" in
my original report overstated it: the main thread is parked in the per-device naming prompt
(`main.cpp:728`, "New name (ENTER to skip):"), reached even with `probe --yes` since that
flag only gates the earlier y/N confirmation. The process log shows both devices were
already open — and the keyboard interface already detached — before this prompt was ever
reached: it's two independent things composing. (a) opening the device detaches the
keyboard, (b) `probe` then blocks on a prompt that keyboard can't answer, and reattach only
happens in `hid_close()`, which never runs while (b) is still blocked. Neither half is a bug
in the other's code; together they take out the input device with no in-process recovery.
That's the actual bug this report/fix targets, not libusb being slow or wrong about anything
else.

**Where I land:** keep hidraw as the default — it's the only backend that can reach the
Darfon's lighting collection at all without also taking the keyboard, since that collection
has no separate interface to claim. But you're right that it costs real throughput on the
output-report path, so I'd rather close that gap than argue the tradeoff away:

- Investigate the `usbhid` `HID_QUIRK_NO_OUTPUT_REPORTS_ON_INTR_EP` quirk, scoped via
  udev/hwdb, to force output reports back onto the control endpoint under hidraw.
- Prefer `hid_send_feature_report` over `hid_send_output_report` wherever a device's
  descriptor exposes the command as a feature report instead.
- Keep `-DALIENFX_HID_BACKEND=libusb` available for hardware whose AlienFX interface really
  is standalone — where the interval cost is real and the detach risk isn't.
- Port your Windows original's `caps.Usage == 0xcc` guard for the Darfon case — hidraw's
  `hid_enumerate()` now gives real `usage_page`/`usage` per collection, which the libusb
  backend never did, so that guard is finally implementable on Linux.

Also found in passing while building the keyboard repro: `probe --yes` only skips the
top-level y/N confirmation — the per-device/per-light naming prompts right after it aren't
gated by `--yes` either, so a libusb-backend `probe --yes` can still reach an unanswerable
prompt, just one step later. Worth gating those too if libusb stays available.

Full writeup, scripts, and raw data: `docs/probe-keyboard-lockup.md` (addendum at the
bottom) and `docs/evidence/` in the fork.
