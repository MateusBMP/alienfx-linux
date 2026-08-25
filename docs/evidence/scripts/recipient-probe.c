// recipient-probe -- does an AlienFX device answer HID class requests that
// need no claim on its keyboard interface?
//
// Background: usbfs only gates control transfers whose recipient is an
// INTERFACE or an ENDPOINT. check_ctrlrecip() (drivers/usb/core/devio.c) has
// no USB_RECIP_DEVICE case at all, so a class request at recipient=device
// needs no claim -- and claiming an interface that has no kernel driver bound
// detaches nothing. Either one would let us drive lighting over libusb without
// ever taking the keyboard away from usbhid.
//
// Whether the *firmware* honors them is not something the kernel can tell us,
// so this asks the hardware. It issues GET_REPORT (a read; no device state
// changes) for one Feature report over three routes:
//
//   A  class/device       wIndex 0          no claim at all
//   B  class/interface    wIndex <free>     claim an unbound interface
//   C  class/interface    wIndex <hid>      detach usbhid, then claim
//
// C is the route hidapi's libusb backend always takes, and the one that
// disables a keyboard for as long as the interface is held. It is the control
// for this experiment: if C answers and A does not, the firmware really is
// filtering on recipient. If nothing answers, the report simply isn't
// readable and the routes have to be compared with a write instead.
//
// C is skipped unless --allow-detach is passed, so a plain run cannot touch
// an input device. Build with build-recipient-probe.sh; run as root.

#include <errno.h>
#include <libusb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_IFACES 32

struct iface_info {
    int number;
    int is_hid;
    int driver_active;
};

static libusb_device_handle *g_handle;
static int g_detached_iface = -1;

// Best-effort cleanup: a SIGINT between detach and reattach would otherwise
// leave the keyboard bound to nothing until the process is reaped. The kernel
// releases claimed interfaces on fd close (usbdev_release()), so this is a
// belt-and-braces path, not the only one.
static void restore(void) {
    if (g_handle && g_detached_iface >= 0) {
        libusb_release_interface(g_handle, g_detached_iface);
        libusb_attach_kernel_driver(g_handle, g_detached_iface);
        g_detached_iface = -1;
    }
}

static void on_signal(int sig) {
    restore();
    _exit(128 + sig);
}

static const char *rc_str(int rc) {
    switch (rc) {
        case LIBUSB_ERROR_TIMEOUT:      return "TIMEOUT";
        case LIBUSB_ERROR_PIPE:         return "STALL (firmware refused)";
        case LIBUSB_ERROR_NO_DEVICE:    return "NO_DEVICE";
        case LIBUSB_ERROR_ACCESS:       return "ACCESS (need root?)";
        case LIBUSB_ERROR_BUSY:         return "BUSY (interface claimed)";
        case LIBUSB_ERROR_INVALID_PARAM:return "INVALID_PARAM (kernel refused recipient)";
        default:                        return libusb_error_name(rc);
    }
}

// One GET_REPORT. reportType 3 = Feature, 1 = Input.
static int get_report(uint8_t recipient, uint16_t w_index, uint8_t report_type,
                      uint8_t report_id, unsigned char *buf, int len) {
    return libusb_control_transfer(
        g_handle,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS | recipient,
        0x01 /* HID GET_REPORT */,
        (uint16_t)((report_type << 8) | report_id),
        w_index, buf, (uint16_t)len, 2000);
}

static void report_result(const char *tag, const char *desc, int rc,
                          const unsigned char *buf, int len) {
    printf("  %-3s %-42s ", tag, desc);
    if (rc < 0) {
        printf("FAIL  %s\n", rc_str(rc));
        return;
    }
    printf("OK    %d byte(s):", rc);
    for (int i = 0; i < rc && i < len; i++) printf(" %02x", buf[i]);
    printf("\n");
}

int main(int argc, char **argv) {
    unsigned vid = 0x0d62, pid = 0x3740;
    unsigned report_id = 0xcc, report_type = 3, length = 8;
    int allow_detach = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--allow-detach")) {
            allow_detach = 1;
        } else if (!strncmp(argv[i], "--device=", 9)) {
            if (sscanf(argv[i] + 9, "%x:%x", &vid, &pid) != 2) {
                fprintf(stderr, "bad --device (want VVVV:PPPP)\n");
                return 2;
            }
        } else if (!strncmp(argv[i], "--report=", 9)) {
            report_id = (unsigned)strtoul(argv[i] + 9, NULL, 0);
        } else if (!strncmp(argv[i], "--type=", 7)) {
            report_type = (unsigned)strtoul(argv[i] + 7, NULL, 0);
        } else if (!strncmp(argv[i], "--length=", 9)) {
            length = (unsigned)strtoul(argv[i] + 9, NULL, 0);
        } else {
            fprintf(stderr,
                    "usage: %s [--device=VVVV:PPPP] [--report=0xCC] "
                    "[--type=3] [--length=8] [--allow-detach]\n", argv[0]);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    atexit(restore);

    int rc = libusb_init(NULL);
    if (rc < 0) {
        fprintf(stderr, "libusb_init: %s\n", libusb_error_name(rc));
        return 1;
    }

    libusb_device **devs;
    ssize_t n = libusb_get_device_list(NULL, &devs);
    if (n < 0) {
        fprintf(stderr, "libusb_get_device_list: %s\n", libusb_error_name((int)n));
        libusb_exit(NULL);
        return 1;
    }

    libusb_device *target = NULL;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(devs[i], &d) == 0 &&
            d.idVendor == vid && d.idProduct == pid) {
            target = devs[i];
            break;
        }
    }
    if (!target) {
        fprintf(stderr, "device %04x:%04x not found\n", vid, pid);
        libusb_free_device_list(devs, 1);
        libusb_exit(NULL);
        return 1;
    }

    struct libusb_config_descriptor *cfg;
    rc = libusb_get_config_descriptor(target, 0, &cfg);
    if (rc < 0) {
        fprintf(stderr, "config descriptor: %s\n", libusb_error_name(rc));
        libusb_free_device_list(devs, 1);
        libusb_exit(NULL);
        return 1;
    }

    rc = libusb_open(target, &g_handle);
    if (rc < 0) {
        fprintf(stderr, "libusb_open: %s\n", rc_str(rc));
        libusb_free_config_descriptor(cfg);
        libusb_free_device_list(devs, 1);
        libusb_exit(NULL);
        return 1;
    }

    // libusb_open() alone claims nothing and detaches nothing -- it is just an
    // open() of /dev/bus/usb/BBB/DDD.
    struct iface_info ifaces[MAX_IFACES];
    int niface = 0, hid_iface = -1, free_iface = -1;

    printf("device %04x:%04x  bus %d addr %d  interfaces:\n", vid, pid,
           libusb_get_bus_number(target), libusb_get_device_address(target));

    for (int i = 0; i < cfg->bNumInterfaces && niface < MAX_IFACES; i++) {
        const struct libusb_interface_descriptor *a = &cfg->interface[i].altsetting[0];
        int active = libusb_kernel_driver_active(g_handle, a->bInterfaceNumber);

        ifaces[niface].number = a->bInterfaceNumber;
        ifaces[niface].is_hid = (a->bInterfaceClass == LIBUSB_CLASS_HID);
        ifaces[niface].driver_active = active;

        printf("  iface %d  class 0x%02x  %d endpoint(s)  kernel driver: %s\n",
               a->bInterfaceNumber, a->bInterfaceClass, a->bNumEndpoints,
               active == 1 ? "BOUND" : active == 0 ? "none" : libusb_error_name(active));

        if (ifaces[niface].is_hid && hid_iface < 0)
            hid_iface = a->bInterfaceNumber;
        if (active == 0 && free_iface < 0)
            free_iface = a->bInterfaceNumber;
        niface++;
    }

    printf("\nGET_REPORT  type=%u id=0x%02x length=%u\n", report_type, report_id, length);

    unsigned char buf[256];
    if (length > sizeof(buf)) length = sizeof(buf);

    // --- Rung A: class/device. No claim, no detach, nothing to undo. ---
    memset(buf, 0, sizeof(buf));
    rc = get_report(LIBUSB_RECIPIENT_DEVICE, 0, (uint8_t)report_type,
                    (uint8_t)report_id, buf, (int)length);
    report_result("A", "class/device, wIndex=0, no claim", rc, buf, (int)length);

    // --- Rung B: class/interface at an interface no kernel driver owns. ---
    if (free_iface < 0) {
        printf("  B   %-42s SKIP  no unbound interface on this device\n",
               "class/interface at an unbound interface");
    } else {
        char desc[64];
        snprintf(desc, sizeof(desc), "class/interface, wIndex=%d (unbound)", free_iface);
        int claimed = libusb_claim_interface(g_handle, free_iface);
        if (claimed < 0) {
            printf("  B   %-42s FAIL  claim: %s\n", desc, rc_str(claimed));
        } else {
            memset(buf, 0, sizeof(buf));
            rc = get_report(LIBUSB_RECIPIENT_INTERFACE, (uint16_t)free_iface,
                            (uint8_t)report_type, (uint8_t)report_id, buf, (int)length);
            report_result("B", desc, rc, buf, (int)length);
            libusb_release_interface(g_handle, free_iface);
        }
    }

    // --- Rung C: the hidapi route. Detaches usbhid; opt-in only. ---
    if (hid_iface < 0) {
        printf("  C   %-42s SKIP  no HID interface\n", "class/interface at the HID interface");
    } else if (!allow_detach) {
        printf("  C   %-42s SKIP  needs --allow-detach (iface %d is %s)\n",
               "class/interface at the HID interface", hid_iface,
               "bound to usbhid; detaching it disables that input device");
    } else {
        char desc[64];
        snprintf(desc, sizeof(desc), "class/interface, wIndex=%d (HID)", hid_iface);

        int had_driver = libusb_kernel_driver_active(g_handle, hid_iface) == 1;
        if (had_driver) {
            int d = libusb_detach_kernel_driver(g_handle, hid_iface);
            if (d < 0) {
                printf("  C   %-42s FAIL  detach: %s\n", desc, rc_str(d));
                goto done;
            }
            g_detached_iface = hid_iface;
        }
        int claimed = libusb_claim_interface(g_handle, hid_iface);
        if (claimed < 0) {
            printf("  C   %-42s FAIL  claim: %s\n", desc, rc_str(claimed));
        } else {
            memset(buf, 0, sizeof(buf));
            rc = get_report(LIBUSB_RECIPIENT_INTERFACE, (uint16_t)hid_iface,
                            (uint8_t)report_type, (uint8_t)report_id, buf, (int)length);
            report_result("C", desc, rc, buf, (int)length);
            libusb_release_interface(g_handle, hid_iface);
        }
        if (had_driver) {
            libusb_attach_kernel_driver(g_handle, hid_iface);
            g_detached_iface = -1;
        }
    }

done:
    printf("\nreading: A or B answering means lighting can be driven with the\n"
           "keyboard left bound to usbhid. Only C answering means the claim is\n"
           "unavoidable and has to be held for as short a window as possible.\n");

    libusb_close(g_handle);
    g_handle = NULL;
    libusb_free_config_descriptor(cfg);
    libusb_free_device_list(devs, 1);
    libusb_exit(NULL);
    return 0;
}
