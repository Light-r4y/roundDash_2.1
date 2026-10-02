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

This writes everything, including the media partition: dashboard images
uploaded through the web interface are replaced with the base set.

Configuring over Wi-Fi
----------------------
Not implemented in this version yet - the web configuration is a stub.
Planned: hold button 1 for 2 s to start the access point
  SSID:     {{AP_SSID_PREFIX}}XXXX   (XXXX - end of the board's MAC address)
  Password: {{AP_PASSWORD}}          (factory default from fw/common/config/conf.h)
then open http://192.168.4.1; firmware and web interface updates at /ota
(roundGauge_firmware_*.bin and roundGauge_www_*.bin).
