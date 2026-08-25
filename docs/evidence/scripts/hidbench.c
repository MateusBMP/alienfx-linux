/*
 * hidbench - microbenchmark for hidapi's hid_send_output_report / hid_write /
 * hid_send_feature_report on a real HID device path, to compare the hidraw
 * and libusb backends on this fork's actual SDK call sites.
 *
 * Build twice, once per backend, linking against each backend's static lib
 * from this repo's own build trees -- see docs/evidence/scripts/build-hidbench.sh.
 *
 * Usage: hidbench <path-or-vid:pid> <output|write|feature> <length> <report-id> <iterations>
 *
 * <path-or-vid:pid> is either a hidapi device path (e.g. /dev/hidraw4 --
 * works as-is under the hidraw backend) or "vid:pid" in hex (e.g. 187c:0550)
 * to go through hid_open() instead -- needed for the libusb backend, whose
 * hid_open_path() expects its own synthetic "bus-port.port:cfg.iface" string
 * rather than a filesystem path, and hid_open() finds the first matching
 * device internally.
 *
 * Prints one "iter,usec" CSV line per call to stdout, and a min/mean/p50/p99/max
 * summary to stderr. The buffer content is a zeroed report; this measures pure
 * transport latency (ioctl/write/control-transfer round trip), not protocol
 * semantics -- see docs/evidence/README.md for why that's still a valid
 * comparison of the two backends' wire paths.
 */
#include <hidapi/hidapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long long now_usec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static int cmp_ll(const void *a, const void *b) {
    long long x = *(const long long *)a, y = *(const long long *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr,
                "usage: %s <path> <output|write|feature> <length> <report-id> <iterations>\n",
                argv[0]);
        return 2;
    }
    const char *path = argv[1];
    const char *mode = argv[2];
    int length = atoi(argv[3]);
    int report_id = atoi(argv[4]);
    int iterations = atoi(argv[5]);

    if (length < 1 || length > 512 || iterations < 1) {
        fprintf(stderr, "bad length/iterations\n");
        return 2;
    }

    if (hid_init() != 0) {
        fprintf(stderr, "hid_init failed\n");
        return 1;
    }

    hid_device *dev = NULL;
    unsigned vid, pid;
    if (sscanf(path, "%x:%x", &vid, &pid) == 2 && strchr(path, '/') == NULL) {
        dev = hid_open((unsigned short)vid, (unsigned short)pid, NULL);
        if (!dev) {
            fprintf(stderr, "hid_open(%04x:%04x) failed: %ls\n", vid, pid,
                    hid_error(NULL));
            return 1;
        }
    } else {
        dev = hid_open_path(path);
        if (!dev) {
            fprintf(stderr, "hid_open_path(%s) failed: %ls\n", path,
                    hid_error(NULL));
            return 1;
        }
    }

    unsigned char buf[512];
    memset(buf, 0, sizeof(buf));
    buf[0] = (unsigned char)report_id;

    long long *samples = (long long *)calloc((size_t)iterations, sizeof(long long));
    int failures = 0;

    for (int i = 0; i < iterations; i++) {
        long long t0 = now_usec();
        int res;
        if (strcmp(mode, "output") == 0) {
            res = hid_send_output_report(dev, buf, (size_t)length);
        } else if (strcmp(mode, "write") == 0) {
            res = hid_write(dev, buf, (size_t)length);
        } else if (strcmp(mode, "feature") == 0) {
            res = hid_send_feature_report(dev, buf, (size_t)length);
        } else {
            fprintf(stderr, "unknown mode '%s'\n", mode);
            hid_close(dev);
            return 2;
        }
        long long t1 = now_usec();
        samples[i] = t1 - t0;
        if (res < 0) failures++;
        printf("%d,%lld\n", i, samples[i]);
    }

    hid_close(dev);
    hid_exit();

    qsort(samples, (size_t)iterations, sizeof(long long), cmp_ll);
    long long sum = 0;
    for (int i = 0; i < iterations; i++) sum += samples[i];
    double mean = (double)sum / iterations;
    long long p50 = samples[iterations / 2];
    long long p99 = samples[(int)(iterations * 0.99)];
    if (p99 >= iterations) p99 = samples[iterations - 1];

    fprintf(stderr,
            "mode=%s length=%d n=%d failures=%d min=%lldus mean=%.1fus "
            "p50=%lldus p99=%lldus max=%lldus\n",
            mode, length, iterations, failures, samples[0], mean, p50, p99,
            samples[iterations - 1]);

    return 0;
}
