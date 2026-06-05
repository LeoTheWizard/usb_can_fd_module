# TODO

Roughly in dependency order. Items within a section can be worked in parallel.

---

## RP2350 Firmware

### Project Setup
- [x] Configure `CMakeLists.txt` with Pico SDK and target (`rp2350`)
- [x] Add `pico_enable_stdio_usb` / USB descriptor boilerplate
- [x] Set up SPI peripheral for MCP251xFD (pins, clock speed, CS)

### MCP251xFD Driver
- [x] Implement SPI read/write register helpers
- [x] MCP251xFD initialization sequence (oscillator, PLL, CRC)
- [x] Configure nominal and data bitrate (CAN FD)
- [x] Configure TX/RX FIFOs and message filters
- [x] Interrupt pin wiring and ISR on RP2350
- [x] TX frame submission (standard + extended IDs, FD payload up to 64 bytes)
- [x] RX frame reception and timestamping
- [ ] Bus-off detection and auto-recovery
- [ ] Error frame reporting (RX error counter, TX error counter)

### USB Interface
- [x] Decide on USB transfer type: CDC ACM (simple) vs. custom bulk (higher throughput)
- [x] Define wire protocol / frame format for CAN↔USB encapsulation
- [x] Implement USB TX path (CAN frame → USB packet)
- [x] Implement USB RX path (USB packet → CAN frame enqueue)
- [ ] Handle USB suspend/resume (bring bus down gracefully)
- [x] USB descriptor strings (VID/PID, manufacturer, product, serial)

### Testing
- [x] Loop-back test on bench (TX → RX on same node)
- [ ] Two-node test with a known-good adapter (e.g. PEAK, Kvaser)
- [ ] Stress test at max FD data bitrate (8 Mbit/s)

---

## Linux Driver

### Skeleton
- [ ] Create kernel module `Makefile` and `Kconfig` entry
- [ ] USB probe/disconnect callbacks, device matching by VID/PID
- [ ] Register/unregister `net_device` with `alloc_candev`

### SocketCAN Integration
- [ ] `ndo_open` / `ndo_stop` — bring CAN bus up/down via USB control messages
- [ ] `ndo_start_xmit` — submit CAN/FD frames to the firmware TX queue
- [ ] USB bulk RX URB pool — receive frames and hand to `netif_rx`
- [ ] `do_set_bittiming` / `do_set_data_bittiming` — configure firmware bitrates
- [ ] CAN FD capability advertisement (`CAN_CTRLMODE_FD`)
- [ ] Error frame generation (`CAN_ERR_*` flags) from firmware error reports
- [ ] Bus-off state machine and automatic restart timer

### Quality
- [ ] `ethtool` stats (TX/RX frames, errors, restarts)
- [ ] Proper URB cancellation on device disconnect / interface down
- [ ] Test with `can-utils`: `cansend`, `candump`, `cangen`, `canbusload`
- [ ] Test with `python-can` library
- [ ] Fuzz USB input path (malformed firmware packets)

### Stretch Goals
- [ ] Conform to `gs_usb` protocol so the adapter works with the existing `gs_usb` driver (avoids maintaining a separate driver)

---

## Windows Support

- [ ] Evaluate approach: WinUSB + userspace library vs. full kernel driver vs. `gs_usb`-compatible (works with existing open-source tools)
- [ ] WinUSB INF/descriptor setup for driverless enumeration
- [ ] Userspace library wrapping USB bulk I/O (C API matching `libpcanbasic` or custom)
- [ ] CLI tool: `can_send`, `can_dump` equivalents
- [ ] Optional: simple GUI (frame monitor / sender) using the userspace library
- [ ] `python-can` bus interface class for Windows

---

## Documentation

- [ ] Add build instructions to `README.md` once firmware is buildable
- [ ] Document USB wire protocol
- [ ] Add photos of the assembled PCB to `docs/`

---
