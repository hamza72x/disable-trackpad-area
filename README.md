# trackpad-filter

Disables configurable edge portions of Apple SPI trackpad by filtering input events at the source. Built for Asahi Linux (Wayland/Hyprland).

## How it works

Grabs exclusive access to real trackpad, creates virtual filtered device via uinput. Touches that **originate** in dead zones are dropped. Touches starting in the active zone stay active even if the finger slides into a dead zone (no mid-gesture drops).

Bottom of trackpad is always fully active.

## Build

```
gcc -O2 -o trackpad-filter trackpad-filter.c
```

## Usage

```
sudo ./trackpad-filter [--left PCT] [--right PCT] [--top PCT]
```

Defaults: `--left 20 --right 20 --top 25`

Example - larger top dead zone:
```
sudo ./trackpad-filter --left 20 --right 20 --top 40
```

## Install as service

```
sudo ./install.sh
```

Edit dead zone percentages in `/etc/systemd/system/trackpad-filter.service`, then:
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
