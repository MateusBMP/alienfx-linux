# HID transport: scoped-claim libusb, not hidraw

## Summary

`AlienFX-SDK` talks to HID devices directly over libusb control/interrupt
transfers (`AlienFX-SDK/src/libusb_helper.cpp`), not through hidapi and not
through the kernel's hidraw interface. This supersedes the hidraw-backend
switch in [`docs/probe-keyboard-lockup.md`](probe-keyboard-lockup.md): that
fix traded away performance to buy safety (hidraw output reports go over an
interrupt-OUT transfer, ~425x slower than the control transfer libusb can
use — measured, see `docs/evidence/data/`). This rewrite gets both, by
scoping every USB interface claim to the single transfer that needs it,
instead of holding one for the life of a device handle.

## Why hidraw wasn't the answer, even though it was safe

hidraw's `hid_open_path()` never claims an interface or touches kernel
driver ownership at all — that's what made it safe. But every
`hid_send_output_report()` then rides `HIDIOCSOUTPUT`
(`drivers/hid/hidraw.c`), which by default falls through to an
interrupt-OUT transfer (`usbhid_output_report`, `drivers/hid/usbhid/hid-core.c`)
whenever the device declares a matching OUT endpoint. On hardware whose OUT
endpoint has a long `bInterval` — the Alienware LED controller in this repo
declares `bInterval=100` — that transfer is bound by the endpoint's polling
interval, not by anything the USB controller or CPU could do faster:

| path | mean | p50 |
|---|---|---|
| hidraw output report (interrupt-OUT) | 63.8ms | 64.0ms |
| libusb control transfer (`SET_REPORT`) | 150us | 140us |

(`docs/evidence/data/hidraw-output.summary.txt`,
`docs/evidence/data/libusb-output.summary.txt`, n=200, length=34, 0
failures either way.) A `setall`-style command that touches several lights
sequentially turns that gap into whole seconds of visible lag. There's no
kernel knob to fix this for hidraw specifically for an arbitrary device
without root touching `/sys/module/usbhid/parameters/quirks` on every boot
(see §Kernel alternative considered, below) — so the fix has to be in how
this SDK talks to the device, not in convincing hidraw to behave differently.

## Why plain libusb wasn't the answer either

hidapi's libusb backend — and naive direct-libusb code following the same
pattern — claims one fixed USB interface for the life of the open handle,
detaching whatever kernel driver owns it and reattaching only on close
(`libusb/hid.c`'s `DETACH_KERNEL_DRIVER`). `alienfx_cli probe` opens every
detected device and then blocks on an interactive prompt; on a Darfon RGB
keyboard (VID `0x0d62`), the AlienFX lighting collection (`usage_page
0xFF89`, `usage 0xCC`) lives on the **same USB interface** as the keyboard's
own input collection — confirmed on this repo's test hardware:

```
interface 0: class 0xFF (vendor-specific), 0 endpoints, no kernel driver
interface 1: class 0x03 (HID), bInterfaceProtocol 1 (Keyboard), usbhid-bound
             -> carries all 4 top-level collections: keyboard, consumer,
                wireless radio, AND the 0xFF89/0xCC AlienFX collection
```

So claiming the interface AlienFX needs *is* claiming the keyboard. Holding
that claim across `probe`'s prompt is what disabled the keyboard and
deadlocked the process (see `probe-keyboard-lockup.md` for the full
incident). This is a property of the firmware's interface layout, not of
which HID library is used — any approach that claims this interface for
longer than a single transfer reproduces the same failure.

### Two escapes that don't exist on this hardware, checked directly

The Linux USB stack has two ways a control transfer can reach a device
*without* claiming an interface at all:

- **Recipient = device**, not interface (`check_ctrlrecip()` in
  `drivers/usb/core/devio.c` only gates `USB_RECIP_ENDPOINT` and
  `USB_RECIP_INTERFACE`; there's no case for `USB_RECIP_DEVICE`).
- **Recipient = interface, but an interface with no kernel driver bound**
  (claiming interface 0 above, the vendor-class one, would detach nothing).

Both were tested directly against the hardware with
`docs/evidence/scripts/recipient-probe.c` — a `GET_REPORT` for the AlienFX
Feature report (`0xCC`), a read that changes no device state, tried at
device recipient and at interface-0 recipient before ever touching interface
1:

```
device 0d62:3740  bus 3 addr 6  interfaces:
  iface 0  class 0xff  0 endpoint(s)  kernel driver: none
  iface 1  class 0x03  1 endpoint(s)  kernel driver: BOUND

  A   class/device, wIndex=0, no claim           FAIL  STALL (firmware refused)
  B   class/interface, wIndex=0 (unbound)         FAIL  STALL (firmware refused)
  C   class/interface at the HID interface        SKIP  needs --allow-detach
```

Both stall. This firmware only answers class requests addressed to the
interface that actually owns the report descriptor — interface 1, the
keyboard. So for this device, a claim (and a detach) is unavoidable; the fix
has to be about how long that claim is held, not about avoiding it.

## The fix: scope every claim to one transfer

`AlienFX-SDK/src/libusb_helper.cpp`'s `ScopedClaim` claims an interface,
using libusb's own `libusb_set_auto_detach_kernel_driver(handle, 1)` so
detach-on-claim and reattach-on-release are handled by libusb itself, and
releases it in its destructor — so the claim's lifetime is exactly the
lifetime of the C++ scope it's created in, which is always just the body of
one HID transfer function (`HidD_SetFeature`, `HidD_SetOutputReport`,
`WriteFile`, `ReadFile`, `HidD_GetFeature`, `HidD_GetInputReport`). Nothing
above that layer — `PrepareAndSend`, `SetColor`, `probe`'s interactive
prompts — can observe or extend a claim; there is no path through this code
where an interface stays claimed while the process waits on user input.

For control-transfer-based calls (`HidD_SetFeature`/`HidD_SetOutputReport`/
`HidD_GetFeature`/`HidD_GetInputReport`), `ControlTransferReport` also tries
the claim-free device-recipient request first, negotiating once per device
(`UsbHidHandle::deviceRecipientWorks`, cached after the first call) and
falling back to the interface-recipient/claim/detach path only if that
fails. This means the recipient-probe result above isn't hardcoded into the
SDK — a device whose firmware *does* answer at device recipient (rungs A/B)
never claims anything at all, while the Darfon correctly falls through to
the scoped claim on its very first send. `WriteFile`/`ReadFile` (interrupt
transfers, used by API_V6/V7) have no device-recipient equivalent — a USB
interrupt endpoint can only be reached through its owning interface — so
those always take the scoped-claim path, but still only for the one
transfer.

### Verified on hardware

Against `0d62:3740` (Darfon, API_V5) and `187c:0550` (Alienware LED
controller, API_V4) on this repo's test machine, running
`alienfx_cli setall` (Debug build, INFO logging) under `dmesg` monitoring:

- The keyboard kept responding throughout, including while `setall` was
  actively sending to it.
- `dmesg` showed three clean detach→reattach cycles, one per `PrepareAndSend`
  call to the Darfon device (`Resetting device`, `Sending hid packet (8
  uint8_ts): cc 94...`, `cc 93...`, `cc 83...`), each one completing —
  `usbhid` re-registering a fresh input device — before the next transfer
  began. No overlap, no stuck detach.
- Every "Sending hid packet" log line was near-instantaneous; the only
  multi-second gaps in the run were the SDK's own pre-existing
  `WaitForReady()`/`usleep()` polling loops in `Reset()` (API_V4's ~2s
  ready-wait, unrelated to transport), not transport latency. No trace of
  the old ~64ms-per-packet hidraw path.
- On-wire bytes for the Alienware controller were unchanged from before this
  rewrite (`length=34`, `buffer[0]=0x00`), confirming the protocol layer
  above the transport is untouched.

### The Darfon usage guard (porting upstream's 6 lines)

Upstream's Windows `AlienFXProbeDevice` selects the Darfon's AlienFX
collection by usage (`HIDP_CAPS.Usage == 0xcc`), using a per-collection
device node Windows' HID class driver hands out. Linux has no such node —
one report descriptor covers every top-level collection on the interface —
so `AlienFX-SDK/src/hid_report_descriptor.cpp` parses it directly
(`ParseTopLevelCollections`) and `AlienFXProbeDevice` matches on
`usage_page == 0xFF89 && usage == 0xCC`, the same collection identified in
`probe-keyboard-lockup.md`'s hardware survey. This also finally gets the
report length right: the old code used the interrupt-IN endpoint's
`wMaxPacketSize` (32, wrong) where the descriptor's actual Feature report
for `0xCC` is 7 data bytes + 1 report-ID byte = **8** — confirmed live in
the verification run above (`Length: 8` in the probe log, vs. the
pre-rewrite evidence log's `Sending hid packet (32 uint8_ts): cc 94 00
00...` against an 8-byte report).

The descriptor is read via hid-core's own sysfs attribute
(`/sys/bus/usb/devices/<bus>-<ports>:<config>.<iface>/<hid-dev>/report_descriptor`)
whenever a kernel driver is bound — world-readable, no open/claim/detach at
all — falling back to a claimed `GET_DESCRIPTOR(HID Report)` only for
interfaces with no driver bound (nothing to detach there either).

## Known follow-up: Darfon lighting collection doesn't visibly respond yet

With the transport and the length fix both verified correct — the real
`colorSet` packet (`cc 8c 02 00 01 ff 00 00`) reaches the device with no
STALL and no dropped write — the keyboard's AlienFX zone still didn't
visibly change color in testing. This is a protocol-level question above
the transport layer: `AddV5DataBlock`/`SetAction`'s V5 mask/index encoding
(`AlienFX_SDK.cpp`) is unchanged by this rewrite, and
`probe-keyboard-lockup.md`'s own evidence had already flagged this
collection as never confirmed working through this SDK. Establishing
whether the mask encoding, the effect/mode selection, or something else is
wrong is out of scope for this transport fix and needs further
hardware-in-the-loop protocol work.

## Kernel alternative considered, not taken

`drivers/hid/hidraw.c` has a per-device escape from the interrupt-OUT path:
`HID_QUIRK_NO_OUTPUT_REPORTS_ON_INTR_EP` (`BIT(18)` = `0x40000`) makes
`hidraw_send_report()` fall through to the same `SET_REPORT` control
transfer libusb uses. Set via the `usbhid.quirks=VID:PID:MASK` kernel
command-line parameter (the sysfs node is read-only), this would have kept
hidraw's zero-claim safety while fixing the Alienware controller's speed —
but it requires a bootloader edit per machine, and doesn't help the Darfon
case at all (hidraw already avoided claiming that interface; the quirk only
changes hidraw's *output-report* transfer, and the Darfon problem was never
about output reports being slow, but about opening the collection at all).
Scoping claims in userspace, as implemented here, fixes both cases without
asking users to edit kernel boot parameters.
