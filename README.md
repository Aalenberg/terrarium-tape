# Terrarium Tape

A Daisy Seed tape-wear effect for the PedalPCB Terrarium platform. It uses a
modulated delay for wow and flutter, adjustable tape aging, saturation, hiss,
and random dropouts. The circuit's audio path is mono; the firmware sends the
processed signal to both Seed outputs for the Terrarium's mono output stage.

Terrarium hardware reference: [PedalPCB Terrarium Platform](https://www.pedalpcb.com/product/pcb351/).

## Controls

| Terrarium control | Function |
| --- | --- |
| POT1 | Wear: darker bandwidth and more tape compression/noise |
| POT2 | Slow wow depth |
| POT3 | Fast flutter depth |
| POT4 | Dropout frequency |
| POT5 | Dry/wet mix |
| POT6 | Wet signal level |
| SW1 | Extra tape hiss |
| SW2 | Darker tape bandwidth |
| SW3 | Unstable modulation character |
| SW4 | Extra saturation |
| FS1 | Latching effect bypass; LED1 indicates enabled |
| FS2 | Hold for a tape-stop slowdown; LED2 indicates held |


## Build and flash

The Arm GNU toolchain must be on `PATH`. The libDaisy SDK is included as a Git
submodule.

```sh
git clone --recurse-submodules <repository-url>
cd <repository-directory>
make -C third_party/libDaisy
make
make program-dfu
```

For an existing clone, initialize the SDK with
`git submodule update --init --recursive`. Override `LIBDAISY_DIR` if the SDK
is installed elsewhere, or set `GCC_PATH` to the directory containing the
`arm-none-eabi` tools if they are not on `PATH`.