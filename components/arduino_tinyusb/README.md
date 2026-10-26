# arduino_tinyusb

TinyUSB for the Arduino library rebuild (`custom_sdkconfig` in platformio.ini). The
prebuilt Arduino libraries have it, but pioarduino's rebuild does not fetch it, so the
native USB port could only be the USB-Serial/JTAG port. With this component the port runs
in TinyUSB mode (`ARDUINO_USB_MODE=0`): serial port + USB keyboard (stream deck screen).

- `CMakeLists.txt`, `Kconfig.projbuild`, `include/`, `src/dcd_dwc2.c`:
  pioarduino/esp32-arduino-lib-builder, branch release/v5.5, commit fdb4a87
  (same `tusb_config.h` as the Arduino 3.3.0 libraries of pioarduino 55.03.30).
- `tinyusb/`: hathach/tinyusb commit 8a78804ff (the version those libraries were built
  with), `src/` with only the Synopsys DWC2 port. MIT license, see `tinyusb/LICENSE`.
