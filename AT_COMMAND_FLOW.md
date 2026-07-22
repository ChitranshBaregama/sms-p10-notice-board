# AT Command Flow Reference

Quick reference for the modem transactions used by the sketch. Useful when
reading Serial debug logs (`>>` = sent to modem, `<<` = received).

## Initialization sequence

| Command | Purpose |
|---|---|
| `AT` (×2) | Autobaud sync / liveness check |
| `ATE0` | Disable command echo (required for clean parsing) |
| `AT+CMGF=1` | SMS text mode |
| `AT+CSCS="GSM"` | GSM 7-bit character set |
| `AT+CPMS="SM","SM","SM"` | Use SIM card for message storage |
| `AT+CNMI=2,1,0,0,0` | New SMS -> `+CMTI` notification |
| `AT+CMGDA="DEL ALL"` | Wipe all stored SMS (full SIM blocks new messages) |

## Receive transaction (state machine)

```
                +CMTI: "SM",<n>      (or 10 s fallback timer)
                        |
                        v  pollDue
IDLE ----- AT+CMGL="ALL" -----> WAIT_HEADER
                                    |
                 +CMGL: <idx>,"REC UNREAD","<sender>",...
                                    |
                     sender in ADMIN_NUMBERS?
                      yes /              \ no
                         v                v
                   READ_BODY          SKIP_BODY
                 (capture lines)    (ignore lines)
                         \              /
                          v            v
                             OK/ERROR
                                |
                        AT+CMGD=<idx>
                                |
                                v
                          WAIT_DEL_OK --OK--> IDLE (+ chain-poll)
```

- A second `+CMGL:` header during READ_BODY finishes the first message and
  switches to SKIP_BODY; the remaining messages are fetched by the chain-poll.
- Any state stuck longer than 8 s is reset to IDLE (timeout recovery).
- `ERROR`, `+CMS ERROR: <n>`, and `+CME ERROR: <n>` all terminate a state.

## Manual diagnostics

Send these from a serial passthrough sketch if the board misbehaves:

| Command | Healthy response |
|---|---|
| `AT+CSQ` | `+CSQ: <rssi>,<ber>` with rssi >= 10 |
| `AT+CREG?` | `+CREG: 0,1` (home) or `0,5` (roaming) |
| `AT+CPIN?` | `+CPIN: READY` |
| `AT+COPS?` | Registered operator name |
