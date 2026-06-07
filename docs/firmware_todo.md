# Firmware — open items & things to think about

Status snapshot: the firmware compiles and the USB ↔ CAN data path is structurally
complete (two cores, lock-free SPSC queues, MCP251xFD driver, framed USB protocol with
SOF + CRC). It has **not** been verified on real hardware. The items below are what's
left to finish or decide before it's a dependable CAN module, grouped by theme.

## 1. Hardware bring-up (the big unknown)
- [ ] Nothing has run on real silicon. Verify: SPI comms with the MCP2518FD, 40 MHz
      crystal/oscillator lock, INT pin timing, transceiver TX/RX, the 120 Ω termination relay.
- [ ] Confirm `CAN_SPI_BAUDRATE` (15 MHz) is within the MCP2518FD spec for the chosen
      mode and that reads/writes (incl. CRC reads) are reliable at that rate.
- [ ] Confirm pin map in `hardware_defs.h` matches the actual PCB.

## 2. Error handling / observability (currently silent failures)
- [ ] `initialise_can_controller()` ignores **every** return code
      (`mcp251xfd_initialise`, `change_opmode`, `set_baudrates`, FIFO config). If bring-up
      fails, USB still enumerates and ACKs commands while CAN does nothing. Add checks +
      surface failure (LED + an error/status packet to the host).
- [ ] No firmware-side logging/diagnostics channel for field debugging.
- [ ] No watchdog. A blocking SPI call or a hung core has no recovery path.
- [x] DONE (partial): bus-error events were already reported (CAN_MSG_ERROR). Added
      software RX-queue overflow reporting — when core0 can't enqueue a frame/event
      because can_rx_queue is full (host too slow), it latches and sends an error with
      usb_can_error_t.sw_overflow set once the queue drains.

## 3. Host-visible status (protocol gap)
- [ ] No way for the host to *query* state: firmware version, current bitrate/mode,
      bus state, error counters on demand. Today errors are only pushed asynchronously.
      Add a control request with a data-IN stage (e.g. `GET_STATUS`, `GET_VERSION`).
- [ ] A version/capability handshake would let the host driver detect protocol mismatches.

## 4. TX path: echo, confirmation, flow control (needed for SocketCAN)
- [x] DONE: TX confirmation via TEF. core0 enables the TEF, drains it on the TEF
      interrupt, and emits a USB_CAN_MSG_TX_EVENT per transmitted frame carrying the
      host's echo cookie (sent in the TX frame's timestamp_us), the frame id/flags/dlc,
      and a transmit timestamp. Cookie correlation uses an in-order software FIFO on
      core0 (single TX FIFO + TEF are in-order); the cookie FIFO is reset on bus-off.
      Kernel side still needs to consume TX_EVENT and call can_get_echo_skb (milestone 3).
- [ ] **TX drops are still silent on the host→device side.** A full `can_tx_queue`
      (filled from core1/USB) drops the frame with no host signal. SocketCAN wants
      back-pressure (stop/wake the netdev queue). Still needs a flow-control/credit
      scheme. (Note: device→host RX-queue drops are now reported — see item 2.)

## 5. Timestamps
- [ ] Inconsistency: `can_queue.h` documents the timestamp as the MCP251xFD hardware
      counter, but `core0.c` actually stamps with `time_us_32()` (RP2350). Pick one.
- [ ] `time_us_32()` wraps every ~71 min — define wrap handling, or move to a 64-bit base.
- [ ] For hardware timestamping in SocketCAN, the MCP251xFD TBC is the better source.

## 6. Bit timing / bitrate (reconcile with SocketCAN model)
- [x] DONE: chose option (a). The protocol now carries register-level segments
      (brp/tseg1/tseg2/sjw) per phase via SET_BITTIMING (nominal) and
      SET_DATA_BITTIMING (data). Firmware calls mcp251xfd_set_bit_timing(); the kernel
      driver's do_set_bittiming/do_set_data_bittiming forward the kernel-computed segments.
      `mcp251xfd_set_baudrates()` is retained and still used for the boot-time default.
- [ ] Sample point / SJW are now whatever the kernel computes (good). The legacy
      baud-based path is still used at init only — fine, but could be unified later.

## 7. Operating modes & CAN FD
- [ ] `SET_MODE` forwards a raw `mcp251xfd_opmode_t`. Map SocketCAN ctrlmodes
      (LISTENONLY, LOOPBACK, ONE_SHOT, PRESUME_ACK, FD) to MCP modes/registers.
- [ ] Verify full CAN FD path end-to-end: data-phase baud, BRS/FDF/ESI flags, 64-byte DLC.
- [ ] Filters: firmware accepts all (fine — SocketCAN filters host-side). Decide whether to
      offer hardware filter offload later.

## 8. Bus-off & error reporting
- [x] DONE: bus-off recovery is now host-controlled. core0 reports bus-off and stops
      auto-recovering (no longer blocks the loop in recover_bus_off); a new IPC_CMD_RESTART
      / USB_CAN_REQ_RESTART performs the recovery. The kernel driver's
      do_set_mode(CAN_MODE_START) issues it (manual `ip link ... restart` or restart-ms),
      so SocketCAN restart semantics are respected.
- [x] DONE: usb_can_error_t is translated to SocketCAN error frames in the driver
      (can_change_state for warning/passive, can_bus_off, CAN_ERR_CRTL_RX_OVERFLOW for
      overflows, CAN_ERR_CNT with TEC/REC).
- [ ] Hardware-verify the bus-off → report → restart cycle end to end (timing, whether
      the MCP251xFD auto-leaves bus-off independently, restart-ms behaviour).

## 9. RX flush on open (known limitation)
- [ ] `USB_CAN_REQ_OPEN` flushes only the OUT (host→device) FIFO. Stale RX frames already
      in `can_rx_queue` aren't dropped (clearing it from core 1 races core 0's producer).
      If a hard IN flush is wanted, do it via IPC on core 0. Workaround for now: host
      ignores frames with timestamps predating its open.

## 10. USB
- [ ] VID/PID `0xCAFE/0x0CDA` are placeholders — get a real allocation (or use pid.codes
      for open-source) before distributing.
- [ ] Full-Speed only (RP2350 USB). Practical bulk throughput (~1 MB/s) can bottleneck
      sustained high-rate CAN FD (8 Mbit/s data). Quantify worst-case and document limits.
- [ ] No MS OS descriptors → Windows won't auto-bind WinUSB (irrelevant for the Linux
      kernel module, noted for any future Windows/userspace host).
- [ ] Verify suspend/resume behavior (`tud_suspend_cb` closes the bus; resume waits for
      host `OPEN`).

## 11. Misc / housekeeping
- [ ] Queue depth is 1024 (~160 KB SRAM across both queues). Fine, but no longer trivial —
      revisit if other large allocations appear.
- [ ] Confirm SPSC invariant holds on every path (RX: core0→core1; TX: core1→core0). All
      RX pushes currently originate on core 0 only — keep it that way.
