# USB CAN-FD Module — Host Protocol Guide

This document describes the USB wire protocol exposed by the module firmware, for
anyone writing a host driver or library against it. The canonical definitions live in
[`src/common/usb_can_protocol.h`](../src/common/usb_can_protocol.h); this guide is the
human-readable companion.

- **Transport:** USB 2.0 Full-Speed, vendor-specific class.
- **Endianness:** every multi-byte field is **little-endian**.
- **Two channels:** a *control* channel (EP0) for configuration, and a *bulk* channel
  (EP1) carrying fixed-size 84-byte packets in both directions.

---

## 1. Device identification

| | Value |
|---|---|
| idVendor | `0xCAFE` *(placeholder — not a registered VID)* |
| idProduct | `0x0CDA` |
| bDeviceClass | `0xFF` (vendor-specific) |
| bcdUSB | `0x0200` (Full-Speed) |
| Configurations | 1 |
| Interface | 0, vendor class, 2 bulk endpoints |
| iSerialNumber | per-unit, from the chip's unique ID |

The **serial number** is exposed only as the standard USB serial-number string
descriptor (read it via `iSerialNumber`, `lsusb -v`, libusb `usb_get_string_descriptor`,
etc.) — there is no in-band request for it.

Claim interface 0 before using the bulk endpoints (on Linux, detach the kernel driver
first if `lw_can` is bound).

---

## 2. Endpoints

| Endpoint | Address | Type | Direction | Use |
|----------|---------|------|-----------|-----|
| Control | `0x00` | Control | bidirectional | configuration requests (§3) |
| Bulk OUT | `0x01` | Bulk | host → device | CAN frames to transmit (§4) |
| Bulk IN | `0x81` | Bulk | device → host | received frames, bus events, TX confirmations (§4) |

Bulk max packet size is **64 bytes**, so each 84-byte protocol packet spans two USB
transactions (64 + 20). The USB stack reassembles this; always read and write whole
84-byte units.

---

## 3. Control requests (EP0)

All requests use `bmRequestType` = vendor + device recipient:

- **Host → device** (no data, or `OUT` data stage): `bmRequestType = 0x40`
- **Device → host** (`IN` data stage, GET_STATUS only): `bmRequestType = 0xC0`

`wIndex` is 0 for all. `bRequest`, `wValue`, and the data stage are:

| `bRequest` | Name | `wValue` | Data stage |
|-----------|------|----------|-----------|
| `0x01` | OPEN | 0 | none |
| `0x02` | CLOSE | 0 | none |
| `0x03` | SET_BITTIMING | 0 | OUT, `usb_can_bittiming` (6 B), nominal phase |
| `0x04` | SET_MODE | opmode | none |
| `0x05` | SET_TERMINATION | 1=on / 0=off | none |
| `0x06` | SET_DATA_BITTIMING | 0 | OUT, `usb_can_bittiming` (6 B), data phase |
| `0x07` | RESTART | 0 | none |
| `0x08` | GET_STATUS | 0 | IN, `usb_can_status` (8 B) |

### Request semantics

- **OPEN** — put the controller on the bus (NORMAL mode). The device flushes its bulk-OUT
  buffer on receipt; the host should discard inbound bulk-IN bytes until the first packet
  whose CRC validates. Until OPEN, the device is off the bus (does not drive or ACK).
- **CLOSE** — take the controller off the bus (low-power).
- **SET_BITTIMING / SET_DATA_BITTIMING** — program nominal / data-phase bit timing from
  register-level segments (§3.1). Set these while the link is "down" (before OPEN).
- **SET_MODE** — set the controller operating mode directly; `wValue` is one of:
  `0` NORMAL, `1` SLEEP, `2` INTERNAL_LOOPBACK, `3` LISTEN_ONLY, `4` CONFIG,
  `5` EXTERNAL_LOOPBACK, `6` CLASSIC, `7` RESTRICTED. (Most hosts use OPEN/CLOSE/RESTART
  and never touch this directly.)
- **SET_TERMINATION** — enable/disable the on-board 120 Ω resistor.
- **RESTART** — recover from bus-off and resume NORMAL mode. Send after a bus-off event
  (see §4.4); the device stays bus-off until told.
- **GET_STATUS** — read firmware version, bus state, error counters, and termination state
  (§3.2).

### 3.1 `usb_can_bittiming` (6 bytes)

```c
struct usb_can_bittiming {   // little-endian, packed
    uint16_t brp;    // baud-rate prescaler: 1..256
    uint16_t tseg1;  // time segment 1 (prop + phase1): 1..256 nominal, 1..32 data
    uint8_t  tseg2;  // time segment 2 (phase2):        1..128 nominal, 1..16 data
    uint8_t  sjw;    // sync jump width:                1..128 nominal, 1..16 data; <= tseg2
};
```

These are **actual** time-quanta counts (the device subtracts 1 per field when writing
hardware registers). The CAN clock is **40 MHz**, so:

```
bitrate      = 40 000 000 / (brp × (1 + tseg1 + tseg2))
sample point = (1 + tseg1) / (1 + tseg1 + tseg2)
```

**Example — 500 kbit/s, ~80% sample point:** `brp=1, tseg1=63, tseg2=16, sjw=16`
→ 40 MHz / (1 × 80) = 500 kbit/s, sample point 64/80 = 80%.

For CAN FD send the nominal phase via SET_BITTIMING and the data phase via
SET_DATA_BITTIMING.

### 3.2 `usb_can_status` (8 bytes, GET_STATUS reply)

```c
struct usb_can_status {       // little-endian, packed
    uint8_t fw_major, fw_minor, fw_patch;
    uint8_t bus_state;        // 0=config 1=active 2=warning 3=passive 4=bus-off
    uint8_t tec, rec;         // transmit / receive error counters (last known)
    uint8_t termination;      // 1 if on-board 120 Ω enabled
    uint8_t _reserved;
};
```

`bus_state`/`tec`/`rec` reflect the firmware's last published snapshot (updated on bus
events), not a live read.

---

## 4. Bulk packets (EP1)

Every bulk packet — both directions — is a fixed **84-byte** little-endian structure:

| Offset | Size | Field | Notes |
|-------:|-----:|-------|-------|
| 0 | 2 | `sof` | start-of-frame marker, always `0xAA55` (bytes `55 AA`) |
| 2 | 1 | `type` | `0x00` frame, `0x01` error, `0x02` tx-event |
| 3 | 1 | `_reserved` | 0 |
| 4 | 4 | `timestamp_us` | RX/TX timestamp; **echo cookie** on host TX frames (see §4.3) |
| 8 | 72 | `payload` | union, interpreted per `type` (§4.1–4.3) |
| 80 | 4 | `crc` | CRC-32 of bytes `[0, 80)` (§5) |

A receiver **must** verify the CRC and discard packets that fail it, re-aligning to the
next SOF (§6). The `type` field selects the payload:

- **Bulk OUT** (host → device): `type` must be `0x00` (frame).
- **Bulk IN** (device → host): `type` is frame, error, or tx-event.

### 4.1 Frame payload — `type = 0x00` (offset 8, 72 bytes)

```c
struct can_frame {    // NOT packed → 72 bytes (note the 2 trailing pad bytes)
    uint32_t id;      // 11-bit (standard) or 29-bit (extended) identifier, raw value
    uint8_t  flags;   // see table below
    uint8_t  dlc;     // data length code 0..15
    uint8_t  data[64];
    // 2 bytes padding to 72
};
```

**Flags** (bitfield):

| Bit | Name | Meaning |
|-----|------|---------|
| `0x01` | EFF | extended 29-bit identifier |
| `0x02` | FDF | CAN FD frame |
| `0x04` | BRS | bit-rate switch (FD only) |
| `0x08` | ESI | error state indicator (FD only) |
| `0x10` | RTR | remote frame (classic only; carries no data) |

`RTR` and `FDF` are mutually exclusive (CAN FD has no remote frames). For an RTR frame,
`dlc` is the requested length and `data` is ignored.

**DLC → byte length:**

| dlc | 0–8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|-----|-----|----|----|----|----|----|----|----|
| bytes | 0–8 | 12 | 16 | 20 | 24 | 32 | 48 | 64 |

Only the first *length* bytes of `data` are meaningful; the rest are unspecified (but are
still covered by the CRC, so fill consistently — zero them).

### 4.2 Error payload — `type = 0x01` (device → host)

Emitted asynchronously on CAN error/state changes. `timestamp_us` is the event time.

```c
struct usb_can_error {   // little-endian, packed (27 bytes)
    uint8_t  rec, tec;             // receive / transmit error counters
    uint8_t  error_warn;           // TEC or REC >= 96
    uint8_t  rx_passive;           // REC >= 128
    uint8_t  tx_passive;           // TEC >= 128
    uint8_t  bus_off;              // node is bus-off — see RESTART (§4.4)
    uint8_t  rx_overflow;          // hardware RX FIFO dropped a frame
    uint8_t  nominal_rx_errors, nominal_tx_errors;
    uint8_t  data_rx_errors, data_tx_errors;
    uint16_t error_frame_count;
    uint8_t  nbit0_err, nbit1_err, nack_err, nform_err, nstuff_err, ncrc_err; // nominal phase
    uint8_t  dbit0_err, dbit1_err, dform_err, dstuff_err, dcrc_err;           // data phase
    uint8_t  txbo_err;             // entered bus-off since last read
    uint8_t  dlc_mismatch;         // received DLC exceeded configured payload
    uint8_t  sw_overflow;          // device dropped a frame/event: software RX queue full
};
```

### 4.3 Transmit-confirmation payload — `type = 0x02` (device → host)

Emitted once a frame has actually been transmitted on the bus. `timestamp_us` is the
transmit time.

```c
struct usb_can_tx_event {   // little-endian, packed (10 bytes)
    uint32_t cookie;  // echoes the timestamp_us the host set on the matching TX frame
    uint32_t id;      // identifier of the transmitted frame
    uint8_t  flags;   // EFF/FDF/BRS/ESI/RTR
    uint8_t  dlc;
};
```

**Echo mechanism:** when the host transmits, it places an opaque value in the outgoing
packet's `timestamp_us`. The device returns it as `cookie` in the matching TX_EVENT once
the frame is on the bus. Use this to correlate confirmations to pending transmits (e.g. a
SocketCAN echo index). Frames are confirmed in transmission order. If a frame never
transmits (e.g. bus-off), no TX_EVENT is sent — flush pending transmits on a bus-off
event.

### 4.4 Bus-off recovery

When an error packet reports `bus_off = 1`, the controller has stopped communicating and
will **not** auto-recover. To resume, send the **RESTART** control request (`0x07`). The
device performs the CAN recovery sequence and returns to NORMAL mode.

---

## 5. CRC-32

The `crc` field is the standard **CRC-32 (IEEE 802.3 / zlib)**:

| Parameter | Value |
|-----------|-------|
| Polynomial | `0x04C11DB7` (reflected `0xEDB88320`) |
| Init | `0xFFFFFFFF` |
| RefIn / RefOut | true / true |
| XorOut | `0xFFFFFFFF` |
| Check (`"123456789"`) | `0xCBF43926` |

Computed over the **first 80 bytes** of the packet (everything before `crc`), stored
little-endian at offset 80.

```c
/* C */                  crc = crc32(0, packet, 80);              /* zlib */
```
```python
# Python            import binascii; crc = binascii.crc32(packet[:80]) & 0xFFFFFFFF
```
```c
/* Linux kernel */       crc = crc32_le(~0u, packet, 80) ^ ~0u;
```

---

## 6. Framing & re-synchronisation

USB bulk is lossless and ordered, so in steady state the stream is simply back-to-back
84-byte packets. Re-synchronisation is only needed at session start or after a partial
read:

1. Scan the byte stream for the SOF marker `55 AA`.
2. Treat the following 84 bytes as a candidate packet.
3. Accept it only if `sof == 0xAA55` **and** the CRC matches; otherwise advance one byte
   and rescan.

On **OPEN**, the device flushes its inbound buffer; the host should likewise drop bulk-IN
bytes until the first CRC-valid packet.

---

## 7. Typical session

```
1.  Claim interface 0.
2.  (optional) GET_STATUS                     → firmware version, termination
3.  SET_BITTIMING (nominal)                   while off the bus
3b. SET_DATA_BITTIMING (data)                 if using CAN FD
4.  SET_TERMINATION                           if needed
5.  OPEN                                       → bus goes live; discard IN until first valid CRC
6.  loop:
       read 84-byte packets on EP1 IN         → frame / error / tx-event
       write 84-byte FRAME packets on EP1 OUT  (sof, type=0, cookie in timestamp_us, crc)
7.  on error packet with bus_off=1: RESTART
8.  CLOSE
```

---

## 8. Implementation notes

- **Packet size is exactly 84 bytes.** If you redefine the structs, reproduce the layout
  precisely — in particular `can_frame` is **72 bytes** (padded), so the union is 72 and
  the packet is 84. The reference header has a compile-time assertion for this.
- **Fill the whole packet before CRC.** The CRC covers all 80 leading bytes including
  unused `data` tail bytes; zero them so both ends agree.
- **Bitrates must divide the 40 MHz clock cleanly** given integer segments; standard rates
  (1 M, 500 k, 250 k, 125 k, …) work. The host computes the segments (§3.1).
- **No flow control on bulk OUT.** If the host floods faster than the bus drains, the
  device's transmit queue can fill and silently drop frames (no TX_EVENT will arrive for a
  dropped frame). Pace transmits or rely on TX_EVENT/echo accounting.
- **VID/PID are placeholders** — match on them, but expect them to change before any
  official release.
