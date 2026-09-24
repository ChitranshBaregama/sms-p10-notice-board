# SMS-Controlled P10 LED Notice Board

An SMS-driven scrolling notice board built on an **Arduino Mega 2560**, a **SIM800/SIM900 GSM module**, and a **P10 LED matrix display (2×1, HUB12)**. Send a text message from an authorized phone number and it scrolls on the display. This is a hardware prototype; unattended reliability has not been established by a soak test.

## Features

- **Sender filtering** — only SMS from whitelisted numbers are displayed (requires 10–15 digits and matches the last 10, so `+91xxxxxxxxxx`, `91xxxxxxxxxx`, and plain 10-digit formats all work). Messages from unknown numbers are deleted and ignored.
- **Notification-independent SMS engine** — the `+CMTI` unsolicited notification is used only as a trigger. Actual message retrieval is done by polling `AT+CMGL="ALL"`, with a 10-second fallback poll. Periodic polling can recover from missed notifications, but does not guarantee lossless delivery.
- **Message queueing** — a new SMS waits for the current message to finish its scroll pass, then takes over. While a pass is in progress, only the latest incoming SMS is kept.
- **O(1) windowed rendering** — only the ~13 visible characters are drawn per frame, so frame time is constant regardless of message length (up to 299 characters). Long messages scroll as smoothly as short ones.
- **EEPROM persistence** — two CRC-checked slots retain the previous committed message if a write is interrupted. `EEPROM.update()` avoids rewriting unchanged bytes.
- **Robust AT handling** — `+CMS ERROR` / `+CME ERROR` responses are recognized, a per-message storage-index delete avoids races with incoming SMS, and an 8-second state timeout auto-recovers any stuck transaction.
- **No heap usage in the hot path** — fixed `char` buffers throughout, no `String` class, no heap fragmentation over long uptimes.

## Hardware Required

| Component | Notes |
|---|---|
| Arduino Mega 2560 | Uses `Serial2` for GSM, so a Mega (or any board with a spare hardware UART) is required |
| SIM800L / SIM900 module | Any AT-command-compatible GSM modem in text mode |
| P10 LED panel ×2 (HUB12) | Configured as 2 wide × 1 high = 64×16 px |
| 5 V / 3 A+ power supply | For the P10 panels |
| 4.0–4.2 V / 2 A power supply | For the SIM800 module |
| Activated SIM card | SMS-capable, without PIN lock |

## Wiring

### P10 panel (HUB12 connector → Mega)

The sketch uses the DMD2 `SoftDMD` driver, which bit-bangs its default pins — the wiring is identical on Uno and Mega (no hardware-SPI pin remapping needed).

| P10 (HUB12) | Arduino Mega |
|---|---|
| A | 6 |
| B | 7 |
| CLK | 13 |
| SCLK (LAT) | 8 |
| R (DATA) | 11 |
| OE | 9 |
| GND | GND (common) |

### GSM module (Serial2)

| SIM800 | Arduino Mega |
|---|---|
| TX | 17 (RX2) |
| RX | 16 (TX2) |
| GND | GND (common) |

### Power — read this first

Most "code not working" reports with this hardware are power problems:

- **Never power the SIM800 from the Arduino 5 V pin.** The module draws up to 2 A bursts during network activity; an undersized supply causes brownouts and silent module resets. Use a dedicated 4.0–4.2 V / 2 A supply. (SIM900 shields with an onboard regulator can take 5 V.)
- **The P10 panels need their own 5 V / 3 A+ supply.**
- **All grounds must be common** (Mega, GSM supply, P10 supply).

## Software Setup

1. Install the **DMD2** library (Freetronics) from the Arduino IDE Library Manager.
2. Open `SMS_P10_NoticeBoard/SMS_P10_NoticeBoard.ino`.
3. Set your admin number(s) at the top of the sketch:

   ```cpp
   const char *ADMIN_NUMBERS[] = {
     "+91XXXXXXXXXX",
   };
   ```

4. Optionally adjust the build flags:

   | Flag | Default | Purpose |
   |---|---|---|
   | `DEBUG_ENABLE` | 1 | Serial debug output at 9600 baud |
   | `PERSIST_SMS_EEPROM` | 1 | Restore last message after power cut |
   | `AUTH_ENABLE` | 1 | Set to 0 to allow all senders (testing) |
   | `SCROLL_SPEED_MS` | 35 | Lower = faster scroll |

5. Upload, open the Serial Monitor at **9600 baud**, and send a test SMS.

## Expected Serial Output

```
--- BOOT: init modem ---
>> AT
<< OK
...
--- SYSTEM ONLINE ---
Free RAM: 6xxx
<< +CMTI: "SM",1
>> AT+CMGL="ALL"        (sent automatically)
<< +CMGL: 1,"REC UNREAD","+91XXXXXXXXXX","","26/07/20,10:30:00+22",145,18
SMS from ADMIN: +91XXXXXXXXXX
<< HELLO NOTICE BOARD
NOW SCROLLING: HELLO NOTICE BOARD
```

An SMS from a non-whitelisted number instead logs:

```
UNAUTHORIZED sender: +91YYYYYYYYYY -> ignore & delete
```

## How It Works

**SMS engine.** New-message notifications (`+CMTI`) only raise a poll flag. The main loop, whenever the SMS state machine is idle, issues `AT+CMGL="ALL"` — on notification, after every completed transaction (chain-poll), and at least every 10 seconds. The state machine walks the CMGL response: extended header → sender filtering → length-delimited body capture → final `OK` → targeted `AT+CMGD=<index>` delete. Anything left in storage is drained by the chain-poll, one message per transaction.

**Display engine.** Rendering is non-blocking and windowed: from the current scroll offset the sketch computes which character sits at the left edge and draws only the visible substring. The message pixel width is computed once per message, not per frame. When the message fully exits the left edge, the queued message (if any) is loaded; otherwise the same message repeats.

**Persistence.** Two EEPROM slots store a length, sequence number, CRC and commit marker. The inactive slot is invalidated first and committed last, so an interrupted update leaves the previous valid slot available. UART reception is serviced during payload writes. On boot the newest valid slot resumes scrolling. The old single-slot format is ignored; send a new message after upgrading.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Display dead / garbage | P10 wiring (see table above) or P10 power supply |
| No `<< OK` responses at boot | GSM TX/RX swapped, wrong baud, or module power |
| `+CMTI` never appears, first SMS works via poll only | Normal — the 10 s poll covers it |
| No SMS ever received | SIM balance/validity, network signal (`AT+CSQ` ≥ 10), or SIM PIN lock |
| Module resets randomly | GSM power supply too weak — see Power section |
| `UNAUTHORIZED sender` for your own number | Number format mismatch in logs vs. whitelist — copy the number exactly as printed in the log |

## Notes & Limitations

- SMS longer than 160 characters arrive from the network as separate parts and each part is treated as its own message.
- Message text is stored/displayed up to 299 characters.
- The GSM character set is configured for standard GSM text; Unicode (e.g., Devanagari) SMS is not supported by this sketch.

## Related

- [**Embedded systems reference**](https://github.com/ChitranshBaregama/embedded-systems-resources) — bare-metal firmware notes and runnable Cortex-M examples. The [UART document](https://github.com/ChitranshBaregama/embedded-systems-resources/blob/main/peripherals/uart.md) and the [state machine document](https://github.com/ChitranshBaregama/embedded-systems-resources/blob/main/patterns/state-machines.md) cover the two techniques this sketch leans on hardest — a notification-independent serial engine, and an AT transaction driven as an explicit state machine with a timeout.
- [**DLMS/COSEM security reference**](https://github.com/ChitranshBaregama/EncryptionAlgorithm-) — seven volumes on smart-metering protocol security.

## License

MIT — see [LICENSE](LICENSE).

## Validation and limitations

Run the sender-boundary regression suite with:

```sh
g++ -std=c++11 -Wall -Wextra -Werror tests/test_phone_match.cpp -o test_phone_match
./test_phone_match
g++ -std=c++11 -Wall -Wextra -Werror tests/test_protocol.cpp -o test_protocol
./test_protocol
```

The CI workflow also compiles the actual sketch for Arduino Mega 2560 with
the pinned AVR core and DMD2 library. Compilation and host tests do not prove
display timing, supply integrity, or unattended operation.

- The display has one pending slot: newer messages replace older pending messages.
- Boot preserves SIM messages; normal polling reads and deletes individual indexes.
- `AT+CSDH=1` enables body lengths and `AT+CSCS="IRA"` selects a single-byte
  terminal character set. The parser consumes the declared body before
  interpreting response tokens, including embedded line breaks. Missing or
  invalid lengths abort without deleting the message; incomplete transactions
  time out. The display stores at most 299 characters while draining the rest.
- The host suite includes literal `OK`/`ERROR` and header-like bodies,
  fragmented input, multiple messages, truncation, timeout recovery, EEPROM
  corruption, and simulated power loss at each write boundary.
- Modem framing follows the [SIM800 AT command manual, sections 4.2.3 and
  4.2.14](https://simcom.ee/documents/SIM800C/SIM800%20Series_AT%20Command%20Manual_V1.10.pdf).
  Confirm extended-header behavior on your exact SIM800/SIM900 firmware.
  The display does not decode UCS2 or reassemble concatenated SMS.
- EEPROM records are CRC-checked, not cryptographically authenticated.
  Brownout behavior and actual EEPROM endurance still require board testing.
- Sender-number filtering is not cryptographic authentication. The configured
  suffix policy intentionally treats matching last-ten-digit numbers alike.
