# trackpad-filter

Disables configurable portions of Apple SPI trackpad by filtering input events. Works on Wayland/Hyprland (Asahi Linux).

## How it works

Grabs exclusive access to real trackpad, creates virtual filtered device via uinput. Touches in dead zones get dropped.

## Build

```
gcc -O2 -o trackpad-filter trackpad-filter.c
```

## Usage

```
sudo ./trackpad-filter [--left PCT] [--right PCT] [--top PCT] [--bottom PCT]
```

Defaults: `--left 15 --right 15 --top 20 --bottom 0`

Example - disable 20% on each side and 40% top:
```
sudo ./trackpad-filter --left 20 --right 20 --top 40
```

## Install as service

```
sudo ./install.sh
```

Edit dead zone percentages in `/etc/systemd/system/trackpad-filter.service` (`ExecStart` line), then:
```
sudo systemctl restart trackpad-filter
```

## Uninstall

```
sudo ./uninstall.sh
```

## Stop temporarily

```
sudo systemctl stop trackpad-filter
```
