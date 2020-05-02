/* splread.c -- Read data back from the GM1356 Sound Level Meter
 *
 * Copyright (C) 2019 Phil Vachon <phil@security-embedded.com>
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license.  See the LICENSE file for details.
 */
#include <hidapi.h>

#include <assert.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define GM1356_SPLMETER_VID         0x64bd
#define GM1356_SPLMETER_PID         0x74e3

#define GM1356_FAST_MODE            0x40
#define GM1356_HOLD_MAX_MODE        0x20
#define GM1356_MEASURE_DBC          0x10

#define GM1356_RANGE_30_130_DB      0x0
#define GM1356_RANGE_30_80_DB       0x1
#define GM1356_RANGE_50_100_DB      0x2
#define GM1356_RANGE_60_110_DB      0x3
#define GM1356_RANGE_80_130_DB      0x4

#define GM1356_FLAGS_RANGE_MASK     0xf

#define GM1356_COMMAND_CAPTURE      0xb3
#define GM1356_COMMAND_CONFIGURE    0x56

#define SEV_SUCCESS     "S"
#define SEV_INFO        "I"
#define SEV_WARNING     "W"
#define SEV_ERROR       "E"
#define SEV_FATAL       "F"

#define MESSAGE(subsys, severity, ident, message, ...) \
        do { \
            fprintf(stderr, "%%" subsys "-" severity "-" ident ", " message " (%s:%d in %s)\n", ##__VA_ARGS__, __FILE__, __LINE__, __FUNCTION__); \
        } while (0)
#define SPL_MSG(sev, ident, message, ...)     MESSAGE("SPL", sev, ident, message, ##__VA_ARGS__)

#define ASSERT_ARG(_x_) \
    do { \
        if (!(_x_)) { \
            SPL_MSG(SEV_FATAL, "BAD-AGUMENTS", "Bad arguments - %s:%d (function %s): " #_x_ " is FALSE", __FILE__, __LINE__, __FUNCTION__); \
            return A_E_BADARGS; \
        } \
    } while (0)

#define DIAG(...) /* Define as an alias to MESSAGE for debug output */

#define FAILED(_x_)                 (0 != (_x_))

#define A_OK                        0
#define A_E_NOTFOUND                -1
#define A_E_BADARGS                 -2
#define A_E_INVAL                   -3
#define A_E_EMPTY                   -4
#define A_E_TIMEOUT                 -5

const char *gm1356_range_str[] = {
    "30-130",
    "30-80",
    "50-100",
    "60-110",
    "80-130",
};

/*
 * Configuration items, taken from the command line
 */
static
bool fast_mode = false;

static
bool measure_dbc = true;

static
unsigned config_range = GM1356_RANGE_30_130_DB;

static
uint64_t interval_ms = 500ul;

static
wchar_t *config_serial = NULL;

/*
 * App state - whether or not we've been asked to terminate
 */
static volatile
bool running = true;

static
void _sigint_handler(int signal)
{
    if (false == running) {
        fprintf(stderr, "User insisted we exit promptly (signal = %d), goodbye.", signal);
        exit(EXIT_FAILURE);
    }

    running = false;
}

static
uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec *  1000000000ull + ts.tv_nsec;
}

static
int splread_find_device(hid_device **pdev, uint16_t vid, uint16_t pid, wchar_t const *serial)
{
    int ret = A_OK;

    struct hid_device_info *devs = NULL,
                           *cur_dev = NULL;
    hid_device *dev = NULL;
    size_t nr_devs = 0;
    uint16_t tgt_pid = 0,
             tgt_vid = 0;

    ASSERT_ARG(NULL != pdev);

    *pdev = NULL;

    devs = hid_enumerate(vid, pid);
    if (NULL == devs) {
        SPL_MSG(SEV_INFO, "NO-DEVICE", "Could not find devices of type %04x:%04x", vid, pid);
        ret = A_E_NOTFOUND;
        goto done;
    }

    cur_dev = devs;
    while (NULL != cur_dev) {
        SPL_MSG(SEV_INFO, "DEVICE", "Device found: %04hx:%04hx path: %s serial: %ls",
                cur_dev->vendor_id, cur_dev->product_id, cur_dev->path, cur_dev->serial_number);

        if (NULL != serial) {
            /* Only count devices where the serial number matches */
            if (0 == wcscmp(serial, cur_dev->serial_number)) {
                tgt_vid = cur_dev->vendor_id;
                tgt_pid = cur_dev->product_id;
                nr_devs++;
            }
        } else {
            /* Just count all the devices we find */
            tgt_vid = cur_dev->vendor_id;
            tgt_pid = cur_dev->product_id;
            nr_devs++;
        }

        cur_dev = cur_dev->next;
    }

    if (nr_devs > 1) {
        SPL_MSG(SEV_ERROR, "MULTIPLE-DEVICES", "Found multiple devices, don't know which one to open, aborting.");
        ret = A_E_INVAL;
        goto done;
    }

    if (0 == nr_devs) {
        SPL_MSG(SEV_ERROR, "NO-DEVICES", "Found no devices that match criteria, aborting.");
        ret = A_E_EMPTY;
        goto done;
    }

    /* Open the HID device */
    if (NULL == (dev = hid_open(tgt_vid, tgt_pid, serial))) {
        SPL_MSG(SEV_ERROR, "CANT-OPEN", "Failed to open device %04x:%04x s/n: %ls - aborting", tgt_vid, tgt_pid, serial);
        ret = A_E_NOTFOUND;
        goto done;
    }

    *pdev = dev;

done:
    if (NULL != devs) {
        hid_free_enumeration(devs);
        devs = NULL;
    }

    if (FAILED(ret)) {
        if (NULL != dev) {
            hid_close(dev);
            dev = NULL;
        }
    }

    return ret;
}

static
int splread_send_req(hid_device *dev, uint8_t *report)
{
    int ret = A_OK;

    int written = 0;

    ASSERT_ARG(NULL != dev);
    ASSERT_ARG(NULL != report);

    if (8 != (written = hid_write(dev, report, 8))) {
        SPL_MSG(SEV_ERROR, "REQUEST-FAIL", "Failed to write 8 bytes to device (wrote %d): %ls", written, hid_error(dev));
        ret = A_E_INVAL;
        goto done;
    }

    DIAG("Wrote %d bytes, waiting for response", written);

done:
    return ret;
}

static
int splread_read_resp(hid_device *dev, uint8_t *response, size_t response_len, unsigned timeout_ns)
{
    int ret = A_OK;

    int read_bytes = 0;
    uint64_t start_time = 0;

    ASSERT_ARG(NULL != dev);
    ASSERT_ARG(NULL != response);
    ASSERT_ARG(8 <= response_len);

    start_time = get_time_ns();

    while (8 != read_bytes) {
        int nr_bytes = 0;
        if (0 > (nr_bytes = hid_read_timeout(dev, &response[read_bytes], response_len + 1 - read_bytes, timeout_ns/1000000ul))) {
            SPL_MSG(SEV_ERROR, "READ-FAIL", "Failed to read back an 8 byte report (got %d): %ls", read_bytes, hid_error(dev));
            ret = A_E_INVAL;
            goto done;
        }

        read_bytes += nr_bytes;

        if (get_time_ns() - start_time > timeout_ns) {
            SPL_MSG(SEV_WARNING, "TIMEOUT", "Timeout waiting for response from device, skipping this read");
            ret = A_E_TIMEOUT;
            goto done;
        }
    }

#ifdef DEBUG_MESSAGES
    SPL_MSG(SEV_INFO, "RESPONSE", "%02x:%02x:%02x:%02x - %02x:%02x:%02x:%02x",
            response[0],
            response[1],
            response[2],
            response[3],
            response[4],
            response[5],
            response[6],
            response[7]);
#endif

done:
    return ret;
}

static
int splread_set_config(hid_device *dev, unsigned int range, bool fast, bool dbc)
{
    int ret = A_OK;

    uint8_t command[8] = { 0x0 };
    int rret = A_OK;

    ASSERT_ARG(NULL != dev);
    ASSERT_ARG(range <= 0x4);

    command[0] = GM1356_COMMAND_CONFIGURE;
    command[1] |= range;

    if (true == fast) {
        command[1] |= GM1356_FAST_MODE;
    }

    if (true == dbc) {
        command[1] |= GM1356_MEASURE_DBC;
    }

    if (FAILED(splread_send_req(dev, command))) {
        SPL_MSG(SEV_FATAL, "CONFIG-FAIL", "Failed to set configuration for SPL meter, aborting");
        ret = A_E_INVAL;
        goto done;
    }

    /* Always wait 500ms for the configuration to succeed */
    if (FAILED(rret = splread_read_resp(dev, command, sizeof(command), 500ul * 1000ul * 1000ul))) {
        SPL_MSG(SEV_FATAL, "CONFIG-NO-ACK", "Did not get the configuration packet acknowledgement, aborting.");
        ret = A_E_INVAL;
        goto done;
    }

done:
    return ret;
}

static
void _print_help(const char *name)
{
    printf("Usage: %s -i [interval ms] [-h] [-f] [-C] [-r {range}] [-S {serial number}]\n", name);
    printf("Where: \n");
    printf(" -i         - polling interval for the device, in milliseconds\n");
    printf(" -h         - get help (this message)\n");
    printf(" -f         - use fast mode\n");
    printf(" -C         - measure dBc instead of dBa\n");
    printf(" -S         - serial number of device to use (optional - if not set, will use first device found\n");
    printf(" -r [range] - specify the range to operate in (in dB). One of:\n");
    printf("            30-130\n");
    printf("            30-80\n");
    printf("            50-100\n");
    printf("            60-110\n");
    printf("            80-130\n");
}

static
bool _arg_find_range(const char *range_arg, unsigned *prange)
{
    bool found = false;

    assert(NULL != prange);

    /* Iterate over the supported range values */
    for (size_t i = 0; i < sizeof(gm1356_range_str)/sizeof(gm1356_range_str[0]); i++) {
        if (0 == strcmp(gm1356_range_str[i], range_arg)) {
            /* Lock in the range value we found */
            *prange = i;
            found = true;
            break;
        }
    }

    if (false == found) {
        SPL_MSG(SEV_FATAL, "UNKNOWN-RANGE", "Unknown dB range configuration entry: %s", range_arg);
    }

    return found;
}

static
void _parse_args(int argc, char *const *argv)
{
    int a = -1;

    size_t serial_len = 0;

    while (-1 != (a = getopt(argc, argv, "i:fCr:S:h"))) {
        switch (a) {
        case 'i':
            interval_ms = strtoull(optarg, NULL, 0);
            SPL_MSG(SEV_INFO, "POLL-INTERVAL", "Setting poll interval to %zu milliseconds", interval_ms);
            break;

        case 'h':
            /* Print help message and terminate */
            _print_help(argv[0]);
            exit(EXIT_SUCCESS);
            break;

        case 'f':
            fast_mode = true;
            SPL_MSG(SEV_INFO, "FAST-MODE-ENABLED", "Enabling fast mode.");
            break;

        case 'C':
            measure_dbc = true;
            SPL_MSG(SEV_INFO, "MEASURE-DBC", "Measuring in units of dBC instead of dBA.");
            break;

        case 'r':
            if (false == _arg_find_range(optarg, &config_range)) {
                /* Don't proceed if we don't have a valid, supported range */
                exit(EXIT_FAILURE);
            }
            break;

        case 'S':
            /* This is a bit shady, but will work */
            serial_len = strlen(optarg);
            config_serial = calloc(serial_len + 1, sizeof(wchar_t));
            mbstowcs(config_serial, optarg, serial_len + 1);
            SPL_MSG(SEV_INFO, "DEVICE-SERIAL-NUMBER", "Using device with serial number %S", config_serial);
            break;
        }
    }
}

int main(int argc, char *const *argv)
{
    int ret = EXIT_FAILURE;

    hid_device *dev = NULL;
    struct sigaction sa = { .sa_handler = _sigint_handler };

    SPL_MSG(SEV_INFO, "STARTUP", "Starting the Chinese SPL Meter Reader");

    /* Catch SIGINT */
    if (0 > sigaction(SIGINT, &sa, NULL)) {
        SPL_MSG(SEV_FATAL, "STARTUP", "Failed to set up SIGINT handler, bizarre. Aborting.");
        exit(EXIT_FAILURE);
    }

    if (0 > sigaction(SIGTERM, &sa, NULL)) {
        SPL_MSG(SEV_FATAL, "STARTUP", "Failed to set up SIGTERM handler, bizarre. Aborting.");
        exit(EXIT_FAILURE);
    }

    /* Parse command line arguments */
    _parse_args(argc, argv);

    /* Search for the first connected device. TODO: suport Serial as an argument, a */
    if (FAILED(splread_find_device(&dev, GM1356_SPLMETER_VID, GM1356_SPLMETER_PID, NULL))) {
        goto done;
    }

    DIAG("HID device: %p", dev);

    /* Set the configuration we just read in */
    if (FAILED(splread_set_config(dev, config_range, fast_mode, measure_dbc))) {
        SPL_MSG(SEV_FATAL, "BAD-CONFIG", "Failed to load configuration, aborting.");
        goto done;
    }

    do {
        int tret = A_OK;
        uint8_t report[8] = { GM1356_COMMAND_CAPTURE };

        /* Send a capture/trigger command */
        if (FAILED(splread_send_req(dev, report))) {
            SPL_MSG(SEV_FATAL, "BAD-REQ", "Failed to send read data request");
            goto done;
        }

        /* Read the response; if we time out, just fire up the loop again */
        if (FAILED(tret = splread_read_resp(dev, report, sizeof(report), interval_ms * 1000000ull))) {
            if (A_E_TIMEOUT != tret) {
                SPL_MSG(SEV_FATAL, "BAD-RESP", "Did not get response, aborting.");
                goto done;
            } else {
                continue;
            }
        } else {
            uint16_t deci_db = report[0] << 8 | report[1];
            uint8_t flags = report[2],
                    range_v = report[2] & 0xf;
            time_t now = time(NULL);
            struct tm *gmt = gmtime(&now);

#ifdef DEBUG_MESSAGES
            SPL_MSG(SEV_INFO, "MEASUREMENT", "%4.2f dB%c SPL (%s, range %s)", (double)deci_db/10.0,
                    flags & GM1356_MEASURE_DBC ? 'C' : 'A',
                    flags & GM1356_FAST_MODE ? "FAST" : "SLOW",
                    range > 0x4 ? "UNKNOWN" : gm1356_range_str[range_v]
                    );
#endif

            fprintf(stdout, "{\"measured\":%4.2f,\"mode\":\"%s\",\"freqMode\":\"%s\","
                    "\"range\":\"%s\",\"timestamp\":\"%04i-%02i-%02i %02i:%02i:%02i UTC\"}\n",
                    (double)deci_db/10.0,
                    flags & GM1356_FAST_MODE ? "fast" : "slow",
                    flags & GM1356_MEASURE_DBC ? "dBC" : "dBA",
                    range_v > 0x4 ? "UNKNOWN" : gm1356_range_str[range_v],
                    gmt->tm_year + 1900, gmt->tm_mon + 1, gmt->tm_mday, gmt->tm_hour, gmt->tm_min, gmt->tm_sec
                   );
            fflush(stdout);
        }

        /* Sleep until the next measurement interval */
        usleep(interval_ms * 1000ul);
    } while (true == running);

    ret = EXIT_SUCCESS;
done:
    if (NULL != dev) {
        hid_close(dev);
    }

    return ret;
}

