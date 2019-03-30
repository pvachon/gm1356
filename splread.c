#include <config/engine.h>

#include <app/app.h>

#include <tsl/assert.h>
#include <tsl/errors.h>
#include <tsl/diag.h>
#include <tsl/time.h>

#include <hidapi.h>

#include <wchar.h>

#define SPL_MSG(sev, ident, message, ...)     MESSAGE("SPL", sev, ident, message, ##__VA_ARGS__)

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

static const char *gm1356_range_str[] = {
    "30-130 dB",
    "30-80 dB",
    "50-100 dB",
    "60-110 dB",
    "80-130 dB",
};

static
aresult_t splread_find_device(hid_device **pdev, uint16_t vid, uint16_t pid, wchar_t const *serial)
{
    aresult_t ret = A_OK;

    struct hid_device_info *devs = NULL,
                           *cur_dev = NULL;
    hid_device *dev = NULL;
    size_t nr_devs = 0;
    uint16_t tgt_pid = 0,
             tgt_vid = 0;

    TSL_ASSERT_ARG(NULL != pdev);

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
aresult_t splread_send_req(hid_device *dev, uint8_t *report)
{
    aresult_t ret = A_OK;

    int written = 0;

    TSL_ASSERT_ARG(NULL != dev);
    TSL_ASSERT_ARG(NULL != report);

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
aresult_t splread_read_resp(hid_device *dev, uint8_t *response, size_t response_len, unsigned timeout_millis)
{
    aresult_t ret = A_OK;

    int read_bytes = 0;
    uint64_t start_time = 0,
             timeout_ns = (uint64_t)timeout_millis * 1000000ull;

    TSL_ASSERT_ARG(NULL != dev);
    TSL_ASSERT_ARG(NULL != response);
    TSL_ASSERT_ARG(8 <= response_len);

    start_time = tsl_get_clock_monotonic();

    while (8 != read_bytes) {
        int nr_bytes = 0;
        if (0 > (nr_bytes = hid_read_timeout(dev, &response[read_bytes], response_len + 1 - read_bytes, timeout_millis))) {
            SPL_MSG(SEV_ERROR, "READ-FAIL", "Failed to read back an 8 byte report (got %d): %ls", read_bytes, hid_error(dev));
            ret = A_E_INVAL;
            goto done;
        }

        read_bytes += nr_bytes;

        if (tsl_get_clock_monotonic() - start_time > timeout_ns) {
            SPL_MSG(SEV_WARNING, "TIMEOUT", "Timeout waiting for response from device, skipping this read");
            ret = A_E_TIMEOUT;
            goto done;
        }
    }

    SPL_MSG(SEV_INFO, "RESPONSE", "%02x:%02x:%02x:%02x - %02x:%02x:%02x:%02x",
            response[0],
            response[1],
            response[2],
            response[3],
            response[4],
            response[5],
            response[6],
            response[7]);

done:
    return ret;
}

static
aresult_t splread_set_config(hid_device *dev, unsigned int range, bool fast, bool dbc)
{
    aresult_t ret = A_OK;

    uint8_t command[8] = { 0x0 };
    aresult_t rret = A_OK;

    TSL_ASSERT_ARG(NULL != dev);
    TSL_ASSERT_ARG(range <= 0x4);

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

    if (FAILED(rret = splread_read_resp(dev, command, sizeof(command), 500))) {
        SPL_MSG(SEV_FATAL, "CONFIG-NO-ACK", "Did not get the configuration packet acknowledgement, aborting.");
        ret = A_E_INVAL;
        goto done;
    }

done:
    return ret;
}

int main(int argc, char const *argv[])
{
    int ret = EXIT_FAILURE;

    struct config *cfg CAL_CLEANUP(config_delete) = NULL;

    hid_device *dev = NULL;

    SPL_MSG(SEV_INFO, "STARTUP", "Starting the Chinese SPL Meter Reader");

    if (argc <= 1) {
        SPL_MSG(SEV_FATAL, "NO-CONFIG", "Need to specify at least one configuration file, aborting.");
        goto done;
    }

    TSL_BUG_IF_FAILED(config_new(&cfg));

    for (int i = 1; i < argc; i++) {
        if (FAILED(config_add(cfg, argv[i]))) {
            SPL_MSG(SEV_FATAL, "MALFORMED-CONFIG", "Failed to add configuration file [%s], aborting.", argv[i]);
            goto done;
        }
        DIAG("Configuration added from file '%s'", argv[i]);
    }

    TSL_BUG_IF_FAILED(app_init("splread", cfg));
    TSL_BUG_IF_FAILED(app_sigint_catch(NULL));

    if (FAILED(splread_find_device(&dev, GM1356_SPLMETER_VID, GM1356_SPLMETER_PID, NULL))) {
        goto done;
    }

    DIAG("HID device: %p", dev);

    if (FAILED(splread_set_config(dev, GM1356_RANGE_50_100_DB, true, false))) {
        SPL_MSG(SEV_FATAL, "BAD-CONFIG", "Failed to load configuration, aborting.");
        goto done;
    }

    do {
        aresult_t tret = A_OK;
        uint8_t report[8] = { GM1356_COMMAND_CAPTURE };

        if (FAILED(splread_send_req(dev, report))) {
            SPL_MSG(SEV_FATAL, "BAD-REQ", "Failed to send read data request");
            goto done;
        }

        if (FAILED(tret = splread_read_resp(dev, report, sizeof(report), 500))) {
            if (A_E_TIMEOUT != tret) {
                SPL_MSG(SEV_FATAL, "BAD-RESP", "Did not get response, aborting.");
                goto done;
            }
        } else {
            uint16_t deci_db = report[0] << 8 | report[1];
            uint8_t flags = report[2],
                    range = report[2] & 0xf;

            SPL_MSG(SEV_INFO, "MEASUREMENT", "%4.2f dB%c SPL (%s, range %s)", (double)deci_db/10.0,
                    flags & GM1356_MEASURE_DBC ? 'C' : 'A',
                    flags & GM1356_FAST_MODE ? "FAST" : "SLOW",
                    range > 0x4 ? "UNKNOWN" : gm1356_range_str[range]
                    );
        }

        usleep(500000ul);
    } while (app_running());

    ret = EXIT_SUCCESS;
done:
    if (NULL != dev) {
        hid_close(dev);
    }

    return ret;
}

