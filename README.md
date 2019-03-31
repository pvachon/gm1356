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

## I keep having to run this as `root`!!1

Copy the file `99-gm1356.rules` to `/etc/udev/rules.d` then go through your
distribution's dance to reload `udev` rules. For many it will simply be:

```
udevadm control -R
```

then either unplug/replug the device or invoke

```
udevadm trigger
```

if you're too lazy to replug.

## Details

I don't understand the details of the protocol super well, but I can say the
following:

 1. The protocol is USB-HID carried over a full-speed USB link
 2. Each HID report (input and output) is 8 bytes long

There are two types of messages I've seen: CONFIGURE and MEASURE. A CONFIGURE
message seems to only involve the first two bytes of the report:
 1. `0x56` - indicates the CONFIGURE command
 2. 1 byte of flags where:
    * `0x40` indicates we're in fast mode (bit not set == in slow mode)
    * `0x10` indicates we're measuring dBC rather than dBA (bit not set == dBA)
    * bits 0-3 are one of the enumerated supported ranges:
      * `0` == 30-130 dB
      * `1` == 30-80 dB
      * `2` == 50-100 dB
      * `3` == 60-110 dB
      * `4` == 80-130 dB
      * All others are unknown/unsupported

It seems this gets a response where the first byte is `0xc4` consistently. You
should poll until you get this response, because HID messages can and will get
lost in transit.

The MEASURE/TRIGGER message is as follows:
 1. `0xb3` - indicates the MEASURE/TRIGGER command
It seems that all other bytes are "don't care" values.

The response is as follows:
 1. 2 bytes representing the sound level, in 100ths of a decibel (i.e. divide
    by 100 to get dB value)
 2. 1 byte of flags where:
    * `0x40` indicates we're in fast mode (bit not set == in slow mode)
    * `0x20` indicates we're in hold max mode (bit not set == normal mode)
    * `0x10` indicates we're measuring dBC rather than dBA (bit not set == dBA)
    * bits 0-3 are one of the enumerated supported ranges:
      * `0` == 30-130 dB
      * `1` == 30-80 dB
      * `2` == 50-100 dB
      * `3` == 60-110 dB
      * `4` == 80-130 dB
      * All others are unknown/unsupported

Still working out what the remaining 5 bytes actually represent.

