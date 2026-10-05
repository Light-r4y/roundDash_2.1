roundGauge - Quick Start
Version: {{VERSION}}
Built:   {{BUILD_DATE}}

Files
-----
  roundGauge_firmware_v{{VERSION}}.bin   application (ota_0)
  roundGauge_www_v{{VERSION}}.bin        web interface (www partition)
  roundGauge_media_v{{VERSION}}.bin      dashboard images (media partition)
  bootloader.bin, partition-table.bin, ota_data_initial.bin
  flash_args                             esptool flags and partition offsets

Flashing a board over USB
-------------------------
Connect the board's Type-C port and run from this folder:

  python -m esptool --chip esp32s3 -b 460800 --before default-reset --after hard-reset write-flash "@flash_args"

This writes the application, the web interface and the media partition: images
and fonts uploaded through the web interface are replaced with the base set.
Screen layout, CAN mapping table, settings and password are stored in NVS and are
kept.

Configuring over Wi-Fi
----------------------
Hold button 1 for 2 s: the screen shows a card with the access point
  SSID:     {{AP_SSID_PREFIX}}XXXX   (XXXX - end of the board's MAC address)
  Password: {{AP_PASSWORD}}          (factory default from fw/common/config/conf.h)
  Address:  http://192.168.4.1
The access point stays on until the board is restarted; a Wi-Fi icon with the
number of connected devices remains at the bottom of the screen.

Pages
  /             status, brightness
  /access.html  settings password and Wi-Fi network name/password
  /editor.html  screens: widgets, signals, colors (drag widgets on the preview)
  /media.html   images and fonts on the board
  /can.html     CAN speed and mode, signal mapping table, frame sniffer
  /imu.html     accelerometer: live g-force dot, calibration, forward direction
  /ota          firmware and web interface update
                (roundGauge_firmware_*.bin and roundGauge_www_*.bin)

Without a CAN bus connected, turn on "Demo" on /can.html: the signals are driven
by a generator, which helps to tune the screens on the desk.

Controls
--------
  Button 1, short press   next screen
  Button 1, hold 2 s      Wi-Fi access point
  Button 1, hold 10 s     clear a forgotten settings password (a warning shows 3 s before)
  Button 2                next screen (does not work while powered over Type-C)
  Swipe left / right      next / previous screen (touch builds)
