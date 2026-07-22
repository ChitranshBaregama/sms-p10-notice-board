# Changelog

## v4 (current)

- **Admin authentication**: only whitelisted sender numbers are displayed (last-10-digit match). Unauthorized messages are deleted and ignored.
- **Notification-independent architecture**: `+CMTI` demoted to a trigger; all reading goes through `AT+CMGL="ALL"` polling with a 10 s fallback and chain-polling. A missed notification can no longer lose a message.
- `+CMS ERROR` / `+CME ERROR` responses recognized (previously only plain `ERROR` was handled and the state machine could get stuck).
- 8-second state timeout auto-recovers any stuck SMS transaction.
- GSM UART is drained during EEPROM writes to prevent RX buffer overflow.

## v3

- **Finish-then-swap queueing**: a new SMS waits for the current message to complete its scroll pass before taking over.
- Runtime delete changed to a targeted `AT+CMGD=<index>` (removes the race between delete-all and an incoming message; better firmware compatibility).
- Boot-time wipe changed to `AT+CMGDA="DEL ALL"` (correct text-mode syntax).

## v2

- **O(1) windowed rendering**: only the visible ~13 characters are drawn per frame. Fixes the hang/starvation with long messages (v1 drew the full string every 35 ms, starving the main loop and overflowing the Serial RX buffer).
- Message pixel width precomputed once per message instead of every frame.
- Optional **EEPROM persistence**: last message restored after power cut.
- `freeRam()` diagnostic added.

## v1

- Initial rewrite of the original sketch:
  - Proper SMS read state machine (body captured only after the `+CMGR:` header, multi-line bodies joined) — the original captured any non-`+` line, including command echo and boot URCs.
  - Boot-time SMS storage wipe (a full SIM silently blocks all new messages).
  - `String` class replaced with fixed char buffers (no heap fragmentation).
  - RX drained during modem init to prevent buffer overflow.
