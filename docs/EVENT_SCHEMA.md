# Event schema

Journal files are little-endian. Version 1.

File header, 8 bytes:

| Offset | Type | Value |
| --- | --- | --- |
| 0 | u32 | `0x4B525053` (`SPRK`) |
| 4 | u16 | schema version `1` |
| 6 | u16 | reserved `0` |

Record header, 40 bytes, then payload:

| Field | Type | Meaning |
| --- | --- | --- |
| magic | u32 | `0x544E5645` (`EVNT`) |
| schema | u16 | `1` |
| type | u16 | event type below |
| exchange_ns | i64 | 0 if the venue did not stamp the event |
| recv_ns | i64 | local receive time |
| process_ns | i64 | local time after handling |
| payload_len | u32 | |
| crc | u32 | CRC-32 of the payload only |

Event types: 1 book snapshot, 2 book delta, 3 trade, 4 funding, 5 decision, 6 order new, 7 ack, 8 reject, 9 cancel, 10 fill, 11 position, 12 risk, 13 connection, 14 sync.

Book snapshot payload:

- u32 bid count
- bid count times: i64 px, i64 qty, i32 order count (`-1` if unknown), all scaled by 1e8 for px and qty
- u32 ask count, same triples
- u8 truncated (1 if the venue only published the top of book)

A reader stops at a torn header, a short payload, or a bad CRC, and returns the records that were complete. It does not invent the rest. Rotation opens `path.1`, `path.2`, … when the writer crosses its byte limit. The current writer uses `std::ofstream`, not a memory map.

Replay applies those snapshot payloads in order. `OrderBook::fingerprint()` must match the book that was recorded. The test writes two snapshots, truncates the file with four junk bytes, and checks the fingerprint.

JSON lines from the recorder are a second, explicit capture format (`data/samples/live/*.jsonl`): `venue`, `recv_ns`, `process_ns`, `exchange_ns`, `channel`, `payload`. The payload is the venue message, unmodified. Normalized fields are derived from it; the original is kept.
