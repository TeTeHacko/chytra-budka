# Host-side configuration for Chytrá Budka development

This directory contains host (Linux workstation) config files needed for
working with the ESP32 hardware.

## udev rules

`90-esp32-serial.rules` + `esp32-serial-name` + `esp32-port-name` — creates
stable symlinks for Espressif USB JTAG/serial devices.

Each connected ESP32 gets **two** symlinks:
- **MAC-based** (`/dev/esp32-aabbccddee02`) — stable across physical ports
- **Port-based** (`/dev/esp32-usb7p11`) — stable across boards (identifies physical position)

### Install

```bash
sudo install -m 755 esp32-serial-name esp32-port-name /usr/local/bin/
sudo install -m 644 90-esp32-serial.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=tty
```

The symlinks are owned `root:uucp` mode `0660` (see the rule), so your user
must be in the **`uucp`** group to open the port (Arch; `dialout` on
Debian/Ubuntu) — otherwise `idf.py flash`/`monitor` fails with permission
denied:

```bash
sudo usermod -aG uucp "$USER"   # permanent — new logins get port access
newgrp uucp                      # activate in the CURRENT shell without re-login
```

### Result

```
/dev/esp32-aabbccddee02 -> ttyACM1   # board ex02 (by MAC)
/dev/esp32-aabbccddee01 -> ttyACM0   # board ex01 (by MAC)
/dev/esp32-usb7p11      -> ttyACM1   # USB bus 7 port 11 (by position)
/dev/esp32-usb7p12      -> ttyACM0   # USB bus 7 port 12 (by position)
```

The MAC-based name uses the full eFuse base MAC (lowercase, no colons) from
the USB serial descriptor. This is unique per chip and stable across reboots
and USB port changes.

The port-based name uses the physical USB bus+port number. This is stable
across board swaps (same physical cable always gets the same name).

### Usage with idf.py

```bash
# Flash specific board by MAC
idf.py -p /dev/esp32-aabbccddee02 flash monitor

# Flash whatever is on USB port 11
idf.py -p /dev/esp32-usb7p11 flash monitor
```

### Duplicate serial numbers?

ESP32-S3 chips have unique eFuse MACs, so USB serials are always unique.
If you encounter boards with duplicate serials (unlikely), the port-based
symlinks still provide unambiguous identification.
