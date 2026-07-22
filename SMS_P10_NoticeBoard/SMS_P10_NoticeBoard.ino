/*********************************************************************
 * SMS -> P10 LED SCROLLING NOTICE BOARD
 *
 * Send an SMS from an authorized (admin) phone number and it scrolls
 * on a P10 LED matrix display. Designed to run 24/7 unattended.
 *
 * FEATURES:
 *  - Admin authentication: only whitelisted sender numbers are
 *    displayed (last-10-digit match). Others are deleted & ignored.
 *  - Notification-independent architecture: +CMTI URC is only a
 *    trigger; actual reading is done via AT+CMGL="ALL" polling with
 *    a 10 s fallback poll. A missed notification can never cause a
 *    lost message.
 *  - Message queueing: a new SMS waits until the current message
 *    finishes its scroll pass, then takes over.
 *  - Windowed rendering: only the ~13 visible characters are drawn
 *    each frame -> O(1) frame time regardless of message length.
 *  - EEPROM persistence (optional): last message is restored and
 *    scrolled again after a power cut.
 *  - Robust AT handling: +CMS ERROR / +CME ERROR recognized,
 *    8 s state timeout auto-recovers any stuck transaction.
 *
 * HARDWARE : Arduino Mega 2560 + SIM800/SIM900 + P10 (2x1, HUB12)
 * LIBRARY  : DMD2 (Freetronics) - install via Library Manager
 *
 * P10 wiring (SoftDMD default pins, bit-banged):
 *   A -> 6 | B -> 7 | CLK -> 13 | SCLK(LAT) -> 8 | R -> 11 | OE -> 9
 *   GND -> GND (common with Mega)
 *
 * GSM wiring (Serial2):
 *   SIM800 TX -> Mega pin 17 (RX2)
 *   SIM800 RX -> Mega pin 16 (TX2)
 *
 * POWER (critical):
 *   SIM800 module : separate 4.0-4.2 V / 2 A supply, common GND
 *                   (SIM900 shield with onboard regulator: 5 V is OK)
 *   P10 panel     : separate 5 V / 3 A+ supply, common GND
 *   Never power either from the Arduino 5 V pin.
 *********************************************************************/

#define DEBUG_ENABLE       1
#define PERSIST_SMS_EEPROM 1
#define AUTH_ENABLE        1     // 0 = allow all numbers (for testing)

/* ========= ADMIN NUMBERS - PUT YOUR NUMBER(S) HERE ========= */
/* Matching is done on the last 10 digits, so any format works:
   "+919876543210", "919876543210" or "9876543210".
   Multiple admin numbers are supported.                        */
const char *ADMIN_NUMBERS[] = {
  "+91XXXXXXXXXX",
  // "+91YYYYYYYYYY",
};
#define ADMIN_COUNT (sizeof(ADMIN_NUMBERS) / sizeof(ADMIN_NUMBERS[0]))
/* =========================================================== */

#define SCROLL_SPEED_MS      35    // lower = faster scroll
#define DISPLAY_WIDTH        64    // 2 panels x 32 px
#define CHAR_W               6     // SystemFont5x7: 5 px glyph + 1 px spacing
#define MAX_MSG              300
#define SMS_POLL_MS          10000 // fallback poll interval
#define SMS_STATE_TIMEOUT_MS 8000  // stuck-transaction recovery timeout

#if DEBUG_ENABLE
  #define DBG_BEGIN(x)   Serial.begin(x)
  #define DBG_PRINT(x)   Serial.print(x)
  #define DBG_PRINTLN(x) Serial.println(x)
#else
  #define DBG_BEGIN(x)
  #define DBG_PRINT(x)
  #define DBG_PRINTLN(x)
#endif

#include <SPI.h>
#include <DMD2.h>
#include <fonts/SystemFont5x7.h>

#if PERSIST_SMS_EEPROM
  #include <EEPROM.h>
  #define EE_MAGIC_ADDR 0
  #define EE_LEN_ADDR   1
  #define EE_MSG_ADDR   3
  #define EE_MAGIC      0xA5
#endif

#define GSM      Serial2
#define GSM_BAUD 9600

SoftDMD dmd(2, 1);   // 2 panels wide x 1 high = 64x16 px

/* ================= DISPLAY STATE ================= */
enum DisplayState { SHOW_IDLE_TEXT, SHOW_SCROLLING_SMS };
DisplayState displayState = SHOW_IDLE_TEXT;

char displayMessage[MAX_MSG] = "SYSTEM READY";
int  msgPixelWidth = 0;              // precomputed once per message
int  scrollX = DISPLAY_WIDTH;
unsigned long lastScrollTime = 0;

/* Holding slot for the next message (applied after the current
   scroll pass completes) */
char pendingMsg[MAX_MSG];
bool pendingAvailable = false;

/* ================= GSM / SMS STATE ================= */
char     gsmLine[320];               // fixed line buffer - no String class,
uint16_t gsmLineLen = 0;             // no heap fragmentation (24/7 safe)

enum SmsRxState {
  SMS_IDLE,         // no transaction in progress
  SMS_WAIT_HEADER,  // CMGL sent, waiting for first +CMGL: or OK
  SMS_READ_BODY,    // capturing body of an authorized message
  SMS_SKIP_BODY,    // unauthorized/extra message - ignore until OK
  SMS_WAIT_DEL_OK   // CMGD sent, waiting for OK
};
SmsRxState smsState = SMS_IDLE;
unsigned long smsStateTime = 0;

char smsBuffer[MAX_MSG];
bool smsAvailable = false;
int  readIndex   = -1;      // storage index currently being processed
bool pollDue     = true;    // trigger one poll right after boot
unsigned long lastPollTime = 0;

void setSmsState(SmsRxState s) {
  smsState = s;
  smsStateTime = millis();
}

/* ================= DIAGNOSTICS ================= */
#if DEBUG_ENABLE
int freeRam() {
  extern int __heap_start, *__brkval;
  int v;
  return (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}
#endif

/* ================= AUTHENTICATION ================= */
bool isAdmin(const char *num) {
#if AUTH_ENABLE
  size_t nl = strlen(num);
  for (uint8_t i = 0; i < ADMIN_COUNT; i++) {
    const char *adm = ADMIN_NUMBERS[i];
    size_t al = strlen(adm);
    size_t cmp = 10;                       // compare last 10 digits
    if (nl < cmp) cmp = nl;
    if (al < cmp) cmp = al;
    if (cmp == 0) continue;
    if (strcmp(num + nl - cmp, adm + al - cmp) == 0) return true;
  }
  return false;
#else
  (void)num;
  return true;
#endif
}

/* Extract the n-th quoted field from a line (1-based). Example:
   +CMGL: 1,"REC UNREAD","+919876543210","","date"
   field 1 = REC UNREAD, field 2 = sender number              */
bool extractQuoted(const char *line, uint8_t fieldNum, char *out, size_t outSize) {
  const char *p = line;
  for (uint8_t f = 0; f < fieldNum; f++) {
    p = strchr(p, '"');                    // opening quote
    if (!p) return false;
    const char *start = ++p;
    p = strchr(p, '"');                    // closing quote
    if (!p) return false;
    if (f == fieldNum - 1) {
      size_t len = (size_t)(p - start);
      if (len >= outSize) len = outSize - 1;
      memcpy(out, start, len);
      out[len] = '\0';
      return true;
    }
    p++;
  }
  return false;
}

/* ================= EEPROM PERSISTENCE ================= */
#if PERSIST_SMS_EEPROM
void eepromSaveMsg(const char *msg) {
  int len = strlen(msg);
  if (len > MAX_MSG - 1) len = MAX_MSG - 1;
  EEPROM.update(EE_MAGIC_ADDR, EE_MAGIC);
  EEPROM.update(EE_LEN_ADDR,     len & 0xFF);
  EEPROM.update(EE_LEN_ADDR + 1, (len >> 8) & 0xFF);
  for (int i = 0; i < len; i++) {
    EEPROM.update(EE_MSG_ADDR + i, msg[i]);   // update() writes changed bytes only
    gsmRxProcess();   // drain UART during slow writes - prevents RX overflow
  }
}

bool eepromLoadMsg(char *out, size_t outSize) {
  if (EEPROM.read(EE_MAGIC_ADDR) != EE_MAGIC) return false;
  int len = EEPROM.read(EE_LEN_ADDR) | (EEPROM.read(EE_LEN_ADDR + 1) << 8);
  if (len <= 0 || len >= (int)outSize) return false;
  for (int i = 0; i < len; i++)
    out[i] = EEPROM.read(EE_MSG_ADDR + i);
  out[len] = '\0';
  return true;
}
#endif

/* ===================================================== */
/* ==================== DISPLAY ======================== */
void startScrolling() {
  msgPixelWidth = strlen(displayMessage) * CHAR_W;   // computed ONCE
  scrollX = DISPLAY_WIDTH;
  displayState = SHOW_SCROLLING_SMS;
}

/* Put a new message on the display (copy + persist + reset scroll) */
void showNewMessage(const char *src) {
  strncpy(displayMessage, src, sizeof(displayMessage) - 1);
  displayMessage[sizeof(displayMessage) - 1] = '\0';
#if PERSIST_SMS_EEPROM
  eepromSaveMsg(displayMessage);
#endif
  startScrolling();
  DBG_PRINT(F("NOW SCROLLING: ")); DBG_PRINTLN(displayMessage);
}

void showIdleScreen() {
  dmd.clearScreen();
  dmd.selectFont(SystemFont5x7);
  int msgWidth = dmd.stringWidth(displayMessage);
  int startX = (DISPLAY_WIDTH - msgWidth) / 2;   // center on panel
  if (startX < 0) startX = 0;
  dmd.drawString(startX, 4, displayMessage);
}

/* WINDOWED RENDERING:
   From scrollX, compute which character sits at the left edge and
   draw only the ~13 characters that can be visible. Frame cost is
   independent of message length -> O(1).                          */
void processScrollingText() {
  if (millis() - lastScrollTime < SCROLL_SPEED_MS) return;
  lastScrollTime = millis();

  int firstChar = 0;
  int xOffset   = scrollX;

  if (scrollX < 0) {
    firstChar = (-scrollX) / CHAR_W;          // chars fully scrolled off left
    xOffset   = scrollX + firstChar * CHAR_W; // remaining offset, range (-5..0]
  }

  char window[16];                            // 64/6 = 11 full + 2 partial chars
  int n = 0;
  for (int i = firstChar; displayMessage[i] != '\0' && n < 13; i++, n++)
    window[n] = displayMessage[i];
  window[n] = '\0';

  dmd.clearScreen();
  dmd.selectFont(SystemFont5x7);
  dmd.drawString(xOffset, 4, window);

  scrollX--;

  /* ---- PASS COMPLETE: message fully exited on the left ---- */
  if (scrollX < -msgPixelWidth) {
    if (pendingAvailable) {
      pendingAvailable = false;
      showNewMessage(pendingMsg);   // start the queued message
    } else {
      scrollX = DISPLAY_WIDTH;      // repeat the same message
    }
  }
}

/* ===================================================== */
/* ==================== GSM CORE ======================= */

/* Send a command and keep draining RX (and servicing the display)
   while waiting - the 64-byte Serial2 buffer can never overflow */
void gsmSend(const char *cmd, unsigned long waitMs) {
  DBG_PRINT(F(">> ")); DBG_PRINTLN(cmd);
  GSM.println(cmd);
  unsigned long t0 = millis();
  while (millis() - t0 < waitMs) {
    gsmRxProcess();
    if (displayState == SHOW_SCROLLING_SMS) processScrollingText();
  }
}

void gsmInitModem() {
  delay(5000);                                  // module boot time

  gsmSend("AT", 600);
  gsmSend("AT", 600);                           // autobaud sync
  gsmSend("ATE0", 600);                         // echo OFF (critical for parsing)
  gsmSend("AT+CMGF=1", 600);                    // SMS text mode
  gsmSend("AT+CSCS=\"GSM\"", 600);              // GSM character set
  gsmSend("AT+CPMS=\"SM\",\"SM\",\"SM\"", 1000); // store messages on SIM
  gsmSend("AT+CNMI=2,1,0,0,0", 600);            // new SMS -> +CMTI notification
  gsmSend("AT+CMGDA=\"DEL ALL\"", 2000);        // wipe old SMS (a full SIM
                                                // silently blocks new messages)
}

/* ---- Transaction helpers ---- */
void finishCapturedMsg() {
  if (smsBuffer[0] != '\0') smsAvailable = true;
}

void endTransaction() {
  if (readIndex >= 0) {
    char cmd[20];
    sprintf(cmd, "AT+CMGD=%d", readIndex);      // delete only the index we read -
    GSM.println(cmd);                           // no race with incoming messages
    readIndex = -1;
    setSmsState(SMS_WAIT_DEL_OK);
  } else {
    setSmsState(SMS_IDLE);
  }
  pollDue = true;   // chain-poll: if more messages are stored, fetch them now
}

void handleCmglHeader() {
  readIndex = atoi(gsmLine + 6);
  char sender[24] = "";
  extractQuoted(gsmLine, 2, sender, sizeof(sender));
  smsBuffer[0] = '\0';

  if (isAdmin(sender)) {
    DBG_PRINT(F("SMS from ADMIN: ")); DBG_PRINTLN(sender);
    setSmsState(SMS_READ_BODY);
  } else {
    DBG_PRINT(F("UNAUTHORIZED sender: ")); DBG_PRINT(sender);
    DBG_PRINTLN(F(" -> ignore & delete"));
    setSmsState(SMS_SKIP_BODY);
  }
}

/* Process one complete line from the modem */
void handleGsmLine() {
  if (gsmLine[0] == '\0') return;
  DBG_PRINT(F("<< ")); DBG_PRINTLN(gsmLine);

  /* +CMTI is only a trigger. Actual reading always goes through
     the CMGL poll - a missed notification cannot lose a message. */
  if (strncmp(gsmLine, "+CMTI:", 6) == 0) {
    pollDue = true;
    return;
  }

  bool isOk  = (strcmp(gsmLine, "OK") == 0);
  bool isErr = (strcmp(gsmLine, "ERROR") == 0) ||
               (strncmp(gsmLine, "+CMS ERROR", 10) == 0) ||
               (strncmp(gsmLine, "+CME ERROR", 10) == 0);

  switch (smsState) {

    case SMS_WAIT_HEADER:
      if (strncmp(gsmLine, "+CMGL:", 6) == 0)  handleCmglHeader();
      else if (isOk || isErr)                  setSmsState(SMS_IDLE); // empty list
      break;

    case SMS_READ_BODY:
      if (strncmp(gsmLine, "+CMGL:", 6) == 0) {
        /* Another message follows in the list - finish the first one,
           skip the rest for now; the chain-poll will fetch them.
           readIndex still refers to the first message (to delete). */
        finishCapturedMsg();
        setSmsState(SMS_SKIP_BODY);
      }
      else if (isOk || isErr) {
        finishCapturedMsg();
        endTransaction();
      }
      else {
        /* Body line - multi-line SMS parts are joined with a space */
        size_t cur = strlen(smsBuffer);
        if (cur > 0 && cur < sizeof(smsBuffer) - 2) {
          smsBuffer[cur++] = ' ';
          smsBuffer[cur] = '\0';
        }
        strncat(smsBuffer, gsmLine, sizeof(smsBuffer) - strlen(smsBuffer) - 1);
      }
      break;

    case SMS_SKIP_BODY:
      if (isOk || isErr) endTransaction();
      break;

    case SMS_WAIT_DEL_OK:
      if (isOk || isErr) setSmsState(SMS_IDLE);
      break;

    case SMS_IDLE:
    default:
      break;
  }
}

/* Non-blocking char-by-char RX with a fixed line buffer */
void gsmRxProcess() {
  while (GSM.available()) {
    char c = GSM.read();
    if (c == '\n') {
      gsmLine[gsmLineLen] = '\0';
      handleGsmLine();
      gsmLineLen = 0;
    }
    else if (c != '\r') {
      if (gsmLineLen < sizeof(gsmLine) - 1) gsmLine[gsmLineLen++] = c;
    }
  }
}

/* ===================================================== */
/* ====================== SETUP ======================== */
void setup() {
  DBG_BEGIN(9600);
  GSM.begin(GSM_BAUD);

  dmd.setBrightness(200);
  dmd.selectFont(SystemFont5x7);
  dmd.begin();

#if PERSIST_SMS_EEPROM
  if (eepromLoadMsg(displayMessage, sizeof(displayMessage))) {
    DBG_PRINT(F("RESTORED FROM EEPROM: "));
    DBG_PRINTLN(displayMessage);
    startScrolling();          // last message scrolls again right from boot
  } else {
    showIdleScreen();
  }
#else
  showIdleScreen();            // "SYSTEM READY" shown during modem init
#endif

  DBG_PRINTLN(F("\n--- BOOT: init modem ---"));
  gsmInitModem();
  DBG_PRINTLN(F("--- SYSTEM ONLINE ---"));
#if DEBUG_ENABLE
  DBG_PRINT(F("Free RAM: ")); DBG_PRINTLN(freeRam());
#endif

  lastPollTime = millis();
}

/* ===================================================== */
/* ====================== LOOP ========================= */
void loop() {
  gsmRxProcess();

  /* Stuck-transaction auto-recovery */
  if (smsState != SMS_IDLE && millis() - smsStateTime > SMS_STATE_TIMEOUT_MS) {
    DBG_PRINTLN(F("!! SMS state timeout -> reset"));
    setSmsState(SMS_IDLE);
  }

  /* Poll: +CMTI trigger, chain-poll, or 10 s fallback */
  if (smsState == SMS_IDLE &&
      (pollDue || millis() - lastPollTime >= SMS_POLL_MS)) {
    pollDue = false;
    lastPollTime = millis();
    readIndex = -1;
    GSM.println("AT+CMGL=\"ALL\"");
    setSmsState(SMS_WAIT_HEADER);
  }

  /* A new authorized SMS has been captured */
  if (smsAvailable) {
    smsAvailable = false;

    if (displayState == SHOW_SCROLLING_SMS) {
      /* Let the current message finish its pass - queue the new one */
      strncpy(pendingMsg, smsBuffer, sizeof(pendingMsg) - 1);
      pendingMsg[sizeof(pendingMsg) - 1] = '\0';
      pendingAvailable = true;
      DBG_PRINT(F("QUEUED (after current pass): "));
      DBG_PRINTLN(pendingMsg);
    } else {
      showNewMessage(smsBuffer);   // display was idle - start immediately
    }
  }

  if (displayState == SHOW_SCROLLING_SMS) processScrollingText();
}
