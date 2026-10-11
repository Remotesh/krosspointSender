# CrossPoint BLE file transfer protocol, version 1

This document is the binding contract between the CrossPoint firmware fork
(branch `ble-transfer`, "Receive via Bluetooth" mode) and the two senders in
this repository (web app, Web Bluetooth; desktop app, Windows BLE). Changes
go here first, then into the three implementations.

## Scope

- Transport: Bluetooth Low Energy GATT. The reader is the peripheral and the
  GATT server. The computer is the central and the GATT client.
- One file per session. One connection at a time.
- No pairing, no bonding, no encryption. The reader only runs the service
  while its "Receive via Bluetooth" screen is open, and stops advertising as
  soon as the user leaves that screen. Web Bluetooth and the Windows BLE
  stack both work without pairing on unencrypted characteristics; this is
  deliberate.
- Throughput is 10 to 50 KB/s depending on the central. Senders warn above
  3 MB and refuse above 16 MB. The reader has no size limit of its own
  beyond free space on the card.
- All integers are little-endian. All strings are UTF-8 without a
  terminator.

## Advertising

- Advertising data: flags, complete list of 128-bit service UUIDs holding
  the service UUID below.
- Scan response: complete local name `CrossPoint-XXXX` where `XXXX` is the
  last two bytes of the BLE MAC address in upper-case hex. Senders filter
  on the service UUID, not on the name, and show the name to the user.
- Advertising interval 100 ms. Connectable, undirected.
- The reader asks for ATT MTU 247 (`NimBLEDevice::setMTU(247)`). The
  negotiated MTU decides the chunk size (see READY).

## Service and characteristics

Base: `6b726472-00XX-4b52-8000-00000000f11e` (`6b726472` is "krdr").

| Name | UUID (XX) | Properties | Direction |
|---|---|---|---|
| FILE_SERVICE | 0001 | primary service | |
| META | 0002 | write with response | central to reader |
| CHUNK | 0003 | write without response | central to reader |
| CONTROL | 0004 | write with response | central to reader |
| STATUS | 0005 | notify | reader to central |
| INFO | 0006 | read | reader to central |

Full UUIDs:

```
FILE_SERVICE 6b726472-0001-4b52-8000-00000000f11e
META         6b726472-0002-4b52-8000-00000000f11e
CHUNK        6b726472-0003-4b52-8000-00000000f11e
CONTROL      6b726472-0004-4b52-8000-00000000f11e
STATUS       6b726472-0005-4b52-8000-00000000f11e
INFO         6b726472-0006-4b52-8000-00000000f11e
```

### INFO (read)

```
u8  protocolVersion   = 1
u16 maxChunkData      bytes of data the reader accepts per CHUNK write
                      (negotiated ATT MTU - 7, clamped to 16..240)
u32 freeKiB           free space on the card in KiB (0 when unknown)
u8  nameLen
u8  name[nameLen]     firmware version string, for example "1.6.5-x4pro"
```

Senders may read INFO before META to show free space. The chunk size the
sender must use comes from READY, not from INFO, because the MTU can change
between the read and the session start.

### META (write with response)

Starts a session. The reader rejects it with STATUS ERROR when a session is
already open.

```
u8  version     = 1
u32 fileSize    bytes, > 0
u32 crc32       CRC-32 (IEEE 802.3, polynomial 0xEDB88320, init 0xFFFFFFFF,
                final xor 0xFFFFFFFF) of the whole file
u8  nameLen     1..200
u8  name[]      file name only, no slashes
u8  pathLen     1..200
u8  path[]      absolute target folder, starts with "/", may end with "/"
```

Rules on the reader:

- `name` must not contain `/` or `\`, must not be `.` or `..`, must not
  start with `.`. Otherwise STATUS ERROR "Invalid file name".
- `path` is normalised like the web server's `normalizeWebPath`. Protected
  paths (see `ProtectedPaths.h`) are refused with STATUS ERROR "Protected
  path". Missing folders are created, one level at a time.
- The reader opens `<path>/.<name>.part` for writing, truncating any old
  partial file, then sends STATUS READY. If the file cannot be opened:
  STATUS ERROR "Cannot create file". If `freeKiB * 1024 < fileSize`: STATUS
  NO_SPACE.
- An existing `<path>/<name>` is allowed. COMMIT replaces it.

### CHUNK (write without response)

```
u32 offset      byte offset of this chunk in the file
u8  data[]      exactly maxChunkData bytes, except the last chunk of the
                file, which carries the remainder (1..maxChunkData)
```

Rules:

- The sender sends chunks in order from offset 0. `offset` must equal the
  number of bytes the reader has received so far. Any other value: STATUS
  ERROR "Out of order", and the session ends (the partial file is deleted).
- Every chunk except the last one carries exactly maxChunkData bytes. A
  short chunk that does not end the file: STATUS ERROR "Bad chunk". This
  keeps the ACK cadence, which is counted in bytes, in step with the
  sender's window, which is counted in chunks.
- Window: the sender may have at most 48 chunks outstanding, that is,
  chunks whose data lies beyond the last ACKed offset. When 48 are
  outstanding it waits for the next ACK before sending more.
- The reader queues chunk data in RAM in the GATT callback and writes it to
  the card from its main loop. It never writes to the SD card inside the
  NimBLE host task. The queue holds at least 48 * maxChunkData bytes, so a
  sender that respects the window can never overflow it; if the reader
  cannot afford that, it lowers maxChunkData in READY.
- The reader sends STATUS ACK with the number of bytes written to the card
  each time the written total crosses a multiple of 16 * maxChunkData, and
  once more when the last byte is written. ACKs are driven by card writes,
  not by chunk arrival, so a slow card still produces ACKs as it drains
  the queue.

### CONTROL (write with response)

```
u8 opcode
```

| Opcode | Name | Reader action |
|---|---|---|
| 0x02 | VERIFY | Flush the file. Compare received size to `fileSize` and the running CRC-32 to `crc32`. Reply STATUS CRC_OK or CRC_BAD (both carry the CRC the reader computed). On size mismatch reply ERROR "Short file" or "Long file". |
| 0x03 | COMMIT | Only after CRC_OK. Close the file, delete `<path>/<name>` if present, rename the `.part` file to `<path>/<name>`, reply STATUS COMMITTED. Then end the session. |
| 0x04 | ABORT | Close and delete the `.part` file, reply STATUS ABORTED, end the session. |
| 0x05 | PING | Reply STATUS PONG. Allowed with or without a session. |

Opcode 0x01 (BEGIN from the earlier draft) is not used. META starts the
session. Unknown opcodes: STATUS ERROR "Bad opcode".

### STATUS (notify)

```
u8  code
u32 value
u8  text[]      optional, rest of the packet, UTF-8
```

| Code | Name | value | text |
|---|---|---|---|
| 0x00 | READY | maxChunkData for this session | |
| 0x01 | ACK | bytes written to the card so far | |
| 0x02 | CRC_OK | crc32 computed by the reader | |
| 0x03 | CRC_BAD | crc32 computed by the reader | |
| 0x04 | COMMITTED | fileSize | final path, for example `/Books/K-rosspoint/a.epub` |
| 0x05 | ERROR | 0 | reason, short English |
| 0x06 | BUSY | 0 | |
| 0x07 | NO_SPACE | freeKiB | |
| 0x08 | ABORTED | bytes received | |
| 0x09 | PONG | protocolVersion | |

After ERROR, BUSY and NO_SPACE the session is closed on the reader and the
partial file is deleted. The central may start again with META.

## Sequence

```
central                                   reader
  connect, discover service
  subscribe STATUS
  read INFO (optional)
  write META  ---------------------------> open .part, send READY(chunk)
  write CHUNK x N (window 48) -----------> queue, write to SD, ACK every 16
  ... wait for ACK(fileSize) ...
  write CONTROL VERIFY ------------------> flush, CRC, send CRC_OK/CRC_BAD
  write CONTROL COMMIT ------------------> rename, send COMMITTED(path)
  disconnect
```

On CRC_BAD the central sends ABORT and may retry from META. On disconnect
with a session open the reader deletes the `.part` file and goes back to
advertising.

## Reader screen

"Receive via Bluetooth" is the fifth item on File Transfer, shown only on
builds with `FREEINK_CAP_BLE_FILE_TRANSFER=1` (ESP32-S3 boards such as the
X4 Pro). The screen shows:

- the advertised name, for example `CrossPoint-3A7F`;
- the state: "Waiting for a connection", "Connected", "Receiving
  <name> 43%", "Saved <path>", or the last error;
- a hint: "Back to stop".

Auto sleep is off while the screen is open. Leaving the screen stops the
GATT server, stops advertising and deinitialises NimBLE so the memory
returns to the reader. The next entry starts it again.

## Sender behaviour

- Filter devices on FILE_SERVICE. Show the local name.
- Subscribe to STATUS before writing META. Treat no READY within 15 s as a
  failure. The reader scans the card for free space when a central
  connects; on a very large card that scan can still be running when META
  arrives, and READY follows it.
- Use the chunk size from READY. Never have more than 48 chunks
  outstanding past the last ACK. Treat no ACK within 10 s as a failure.
- Progress for the UI is the last ACK offset divided by fileSize.
- VERIFY then COMMIT. Report the COMMITTED path as the remote path.
- On any failure after META, send ABORT if the link is still up.
- Refuse files over 16 MB before connecting. Warn, but allow, over 3 MB.

## CRC-32 test vector

`crc32("123456789")` = `0xCBF43926`.
