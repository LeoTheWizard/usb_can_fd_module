![CAN Module Banner](docs/Banner.webp)

# USB CAN-FD Module

A compact, open-source USB CAN FD adapter built around the **RP2354A** microcontroller and the **MCP251xFD** CAN FD controller. Plug it into any laptop or PC via USB-C and get a full SocketCAN-compatible CAN FD interface — no extra power supply needed.

> **Status:** Hardware complete — firmware and host drivers in development.

---

## Hardware

| Component | Details |
|-----------|---------|
| MCU | Raspberry Pi RP2354A (RP2350 family, dual Arm Cortex-M33 + RISC-V) |
| CAN FD Controller | Microchip MCP251xFD (SPI-connected) |
| Host Interface | USB-C (male) — bus-powered |
| CAN Bus Connector | 2-pin PicoBlade (CAN H / CAN L) |

The PCB has been designed and manufactured. Schematics are available in the [`schematics/`](schematics/) directory.

### Schematics

| Sheet | Preview |
|-------|---------|
| MCU | [schematic_mcu.svg](schematics/svgs/schematic_mcu.svg) |
| CAN FD | [schematic_can_fd.svg](schematics/svgs/schematic_can_fd.svg) |
| Power | [schematic_power.svg](schematics/svgs/schematic_power.svg) |
| Interfaces | [schematic_interfaces.svg](schematics/svgs/schematic_interfaces.svg) |

Full PDF: [pcb_schematic_v2.pdf](schematics/pcb_schematic_v2.pdf)

---

## Repository Layout

```
usb_can_fd_module/
├── docs/                        # Images and documentation assets
├── schematics/                  # PCB schematics (SVG + PDF)
│   └── svgs/
├── src/
│   ├── rp2354a_firmware/        # RP2350 firmware (C, Pico SDK)
│   └── drivers/                 # Host-side drivers
│       └── linux/               # Linux SocketCAN USB driver (kernel module)
└── TODO.md
```

---

## Software Components

### RP2350 Firmware (`src/rp2354a_firmware/`)

The firmware runs on the RP2354A and is responsible for:

- SPI communication with the MCP251xFD
- CAN FD frame TX/RX and filtering
- Exposing a USB interface to the host (USB CDC ACM or custom bulk endpoint)
- Bus error detection and recovery

Built with the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk).

### Linux Driver (`src/drivers/`)

A kernel module that registers a SocketCAN `can` network interface over USB. Once loaded, the adapter appears as a standard `canX` device and works with the full [`can-utils`](https://github.com/linux-can/can-utils) toolchain:

```sh
ip link set can0 up type can bitrate 500000 dbitrate 2000000 fd on
candump can0
cansend can0 123##0DEADBEEF
```

### Windows (planned)

A Windows application or WinUSB-based driver providing a cross-platform API and optional GUI for interacting with the adapter.

---

## Building

> Build instructions will be added once the firmware and driver reach a usable state. See [TODO.md](TODO.md) for current progress.

---

## License

[MIT](LICENSE) — Leo Walker, 2025
