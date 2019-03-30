# splread - Read Sound Level from GM1356 meters

This tool is intended to read, and print to stdout, the current sound level
from a USB-connected GM1356 sound level meter.

The GM1356 is not a precision instrument, but it's handy if you want to measure
and monitor sound levels over a long period of time.

## Requirements

You will need to have installed the following:
 * The TSL (https://github.com/pvachon/tsl)
 * `libhidapi`

For Ubuntu Linux, you can install `libhidapi` as follows:
```
apt-get install libhidapi-dev
```

You can simply build the tool by invoking `make` in the source directory. The
output file, called `splread` is invoked with a configuration file. A sample
configuration file is included in the distributoin, and is called
`splread.json`.

## Details

I don't understand the details of the protocol super well, but I can say the
following:

 1. The protocol is USB-HID carried over a full-speed USB link
 2. Each HID report (input and output) is 8 bytes long
 3. The first two bytes of the input hid report is the sound level in 100ths of
    a decibel. The subsequent byte is a status byte.

Still working out the polling command meaning, and what the remaining 6 bytes
of the HID report mean.

