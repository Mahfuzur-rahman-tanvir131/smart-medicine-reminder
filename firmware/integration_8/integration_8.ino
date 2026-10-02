#include <Wire.h>
#include <SPI.h>
#include <RTClib.h>
#include <Preferences.h>
#include <FFat.h>

#include <GxEPD2_BW.h>
#include <DFRobotDFPlayerMini.h>
#include <Adafruit_Fingerprint.h>
#include <esp_task_wdt.h>
#include <esp_partition.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// The Bangla dashboard page, compiled in from the file beside this one.
#include "webpage.h"

// Bangla rendering. These three live in Documents/Arduino/libraries
// and stay OUT of this file on purpose: BanglaFont.cpp is 3.8 MB of
// generated glyph data and would make this sketch unopenable.
#include <BanglaUTF8.h>
#include <BanglaFont.h>
#include <BanglaEPD.h>


// =====================================================
// 1. FINAL PIN MAP
// =====================================================

// Buttons
const int PIN_BTN_UP     = 4;
const int PIN_BTN_DOWN   = 5;
const int PIN_BTN_SELECT = 6;
const int PIN_BTN_BACK   = 7;

// RTC
const int I2C_SDA = 8;
const int I2C_SCL = 9;
const int RTC_INT = 21;

// E-Paper
const int EPD_CS   = 10;
const int EPD_MOSI = 11;
const int EPD_SCK  = 12;
const int EPD_DC   = 13;
const int EPD_RST  = 14;
const int EPD_BUSY = 15;

// SPI clock for the e-paper. The library defaults to 10 MHz, which
// assumes a short, clean bus. This box uses jumper leads, so it runs
// deliberately slow. Raise it only if the wiring is ever made solid.
const uint32_t EPD_SPI_HZ = 2000000;

// Buzzer
const int BUZZER_PIN = 16;

// DFPlayer
const int MP3_TX = 17;
const int MP3_RX = 18;

// Fingerprint
const int FP_TX = 1;
const int FP_RX = 2;

// Locks
const int LOCK_C1 = 39;
const int LOCK_C2 = 40;
const int LOCK_C3 = 41;
const int LOCK_C4 = 42;


// =====================================================
// 2. GENERAL CONFIG
// =====================================================

const uint32_t CONFIG_VERSION = 1;

const int NUM_USERS = 2;

// The caretaker is a third fingerprint identity, not a third resident.
// Fingerprint IDs 20-29. This finger opens ANY compartment that is
// asking for a fingerprint, whoever the dose belongs to.
const uint8_t CARETAKER_USER = 2;
const int NUM_SLOTS = 4;

const int MIN_SCHEDULE_GAP_MIN = 15;


// =====================================================
// 3. EVENT LOG CONFIG
// =====================================================

const char* LOG_FILE =
  "/events.csv";

const char* LOG_OLD_FILE =
  "/events_old.csv";

// More than enough for prototype.
// File rotates automatically.
const size_t MAX_LOG_FILE_BYTES =
  256UL * 1024UL;

bool logStorageOK = false;


// =====================================================
// 4. DATA MODEL
// =====================================================

struct UserProfile
{
  uint8_t userId;
  char name[24];
  uint16_t reminderTrack;
};


struct DoseSlot
{
  uint8_t userId;

  uint8_t hour;
  uint8_t minute;

  uint8_t compartment;

  char medName[24];

  uint32_t startEpoch;
  uint16_t courseDays;

  bool enabled;
};


UserProfile users[NUM_USERS];
DoseSlot slots[NUM_SLOTS];

Preferences preferences;


// =====================================================
// 5. HARDWARE OBJECTS
// =====================================================

RTC_DS3231 rtc;


GxEPD2_BW<
  GxEPD2_154_D67,
  GxEPD2_154_D67::HEIGHT
> display(
  GxEPD2_154_D67(
    EPD_CS,
    EPD_DC,
    EPD_RST,
    EPD_BUSY
  )
);


HardwareSerial mp3Serial(1);
DFRobotDFPlayerMini player;


HardwareSerial fpSerial(2);
Adafruit_Fingerprint finger(&fpSerial);


// =====================================================
// 6. SYSTEM STATUS
// =====================================================

bool rtcOK = false;
bool mp3OK = false;
bool fingerprintOK = false;
bool configOK = false;


// =====================================================
// 7. BUTTON SYSTEM
// =====================================================

const int buttonPins[4] =
{
  PIN_BTN_UP,
  PIN_BTN_DOWN,
  PIN_BTN_SELECT,
  PIN_BTN_BACK
};


enum ButtonEvent
{
  BUTTON_NONE,
  BUTTON_UP,
  BUTTON_DOWN,
  BUTTON_SELECT,
  BUTTON_BACK
};


bool lastRawState[4] =
{
  HIGH, HIGH, HIGH, HIGH
};


bool stableState[4] =
{
  HIGH, HIGH, HIGH, HIGH
};


unsigned long stateChangedAt[4] =
{
  0, 0, 0, 0
};


const unsigned long DEBOUNCE_MS = 40;


// =====================================================
// 8. MENU SYSTEM
// =====================================================

enum MenuState
{
  MENU_BROWSE,
  MENU_EDIT_HOUR,
  MENU_EDIT_MINUTE,
  MENU_EDIT_ENABLED,
  MENU_CONFIRM
};


MenuState menuState = MENU_BROWSE;

int selectedSlot = 0;

// Which screen the box is showing when no dose is running. The
// browser for today's doses lives in section 15H; the state is
// declared here because power saving closes the browser, and that
// runs far earlier in this file.
enum UiMode
{
  UI_IDLE,
  UI_BROWSE,
  UI_FORCE_AUTH,
  UI_FORCE_RESULT,
  UI_VOLUME
};

UiMode uiMode = UI_IDLE;

// ---- enrolling a fingerprint (section 15I) ----
//
// Driven from the dashboard, but carried out here in loop(). It takes
// six deliberate finger presses and the better part of a minute, so it
// cannot run on the web server's task.
enum EnrollState
{
  ENR_IDLE,
  ENR_VERIFY,     // prove who you are with the CURRENT finger
  ENR_PRESS,      // waiting for a finger
  ENR_LIFT,       // waiting for it to come off again
  ENR_DONE,
  ENR_FAIL
};

volatile bool enrollWanted   = false;   // set by the web handler
uint8_t       enrollWantUser = 0;

EnrollState   enrollState = ENR_IDLE;
uint8_t       enrollUser  = 0;
uint8_t       enrollAngle = 0;          // 0 middle, 1 left, 2 right
uint8_t       enrollPress = 0;          // 0 or 1 - two per angle
unsigned long enrollSince = 0;
char          enrollMsg[72] = "";

// How many templates each of the three identities has on file.
// Counting means one UART round trip per template slot, so it is done
// at boot and after an enrolment - never from a web request.
uint8_t enrollCount[3] = { 0, 0, 0 };


int           browseSlot  = 0;
unsigned long uiModeSince = 0;

// One screen past the last dose is the device screen: clock, date and
// volume. It is not a DoseSlot, so every slot-indexed thing has to
// check for it.
const int BROWSE_DEVICE = NUM_SLOTS;
const int BROWSE_COUNT  = NUM_SLOTS + 1;

// E-paper takes over a second to redraw, so holding UP would queue up
// a painful backlog. Apply the volume at once, let the sound be the
// feedback, and let the screen catch up when the pressing stops.
unsigned long volRedrawAt = 0;

// Non-zero while the "wrong person" screen is up during a
// move-forward, so the sensor is ignored until it has been read.
unsigned long forceWrongAt = 0;


uint8_t tempHour = 0;
uint8_t tempMinute = 0;
bool tempEnabled = true;


// =====================================================
// 9. DOSE OUTCOME
// =====================================================

enum DoseOutcome
{
  OUTCOME_NONE,

  OUTCOME_TAKEN,

  OUTCOME_MISSED,

  OUTCOME_UNCONFIRMED,

  OUTCOME_CANCELLED
};


DoseOutcome pendingOutcome =
  OUTCOME_NONE;


// =====================================================
// 10. RTC + SCHEDULER
// =====================================================

unsigned long lastRTCPrint = 0;

const unsigned long RTC_PRINT_INTERVAL =
  15000;


unsigned long lastSchedulerCheck = 0;

const unsigned long SCHEDULER_CHECK_INTERVAL =
  500;


uint32_t lastTriggeredDateKey[NUM_SLOTS] =
{
  0, 0, 0, 0
};


// =====================================================
// 11. REMINDER STATE MACHINE
// =====================================================

enum ReminderState
{
  REMINDER_IDLE,

  REMINDER_BUZZER,

  REMINDER_POWER_SETTLE,

  REMINDER_VOICE_1,

  REMINDER_VOICE_2,

  REMINDER_WAIT_FINGERPRINT,

  REMINDER_PRE_UNLOCK_SETTLE,

  REMINDER_UNLOCK_PULSE,

  REMINDER_POST_UNLOCK_SETTLE,

  REMINDER_WAIT_CONFIRMATION,

  REMINDER_RESULT_AUDIO
};


ReminderState reminderState =
  REMINDER_IDLE;


int activeReminderSlot = -1;

unsigned long reminderStateStarted = 0;

unsigned long authWindowStarted = 0;

unsigned long lastFingerprintPrompt = 0;

unsigned long fingerprintScanBlockedUntil = 0;

unsigned long confirmationWindowStarted = 0;


// =====================================================
// 12. TIMINGS
// =====================================================

const unsigned long BUZZER_DURATION_MS =
  2000;


const unsigned long POWER_SETTLE_MS =
  150;


const unsigned long REMINDER_VOICE_HOLD_MS =
  5000;


const unsigned long FP_PROMPT_INTERVAL_MS =
  20000;


// Production value
const unsigned long AUTH_WINDOW_MS =
  10UL * 60UL * 1000UL;


const unsigned long FP_RETRY_BLOCK_MS =
  3000;


const unsigned long PRE_UNLOCK_SETTLE_MS =
  150;


const unsigned long LOCK_PULSE_MS =
  400;


const unsigned long POST_UNLOCK_SETTLE_MS =
  150;


const unsigned long CONFIRMATION_WINDOW_MS =
  120UL * 1000UL;


const unsigned long RESULT_AUDIO_HOLD_MS =
  5000;


const unsigned long REMINDER_CANCEL_HOLD_MS =
  3000;


// =====================================================
// 13. BACK HOLD
// =====================================================

unsigned long backHoldStarted = 0;

bool backHoldActive = false;

bool backCancelTriggered = false;


// =====================================================
// 14. AUDIO TRACK MAP
// =====================================================

// 0001 -> দাদু reminder
// 0002 -> ঠাকুমা reminder
// 0003 -> আঙুলের ছাপ দিন
// 0004 -> ঔষধ গ্রহণ সম্পন্ন
// 0005 -> ভুল ব্যবহারকারী
// 0007 -> ঔষধ গ্রহণ করা হয়নি
// 0008 -> ঔষধ গ্রহণ নিশ্চিত হয়নি

const uint16_t TRACK_FP_PROMPT =
  3;

const uint16_t TRACK_TAKEN =
  4;

const uint16_t TRACK_WRONG_USER =
  5;

const uint16_t TRACK_MISSED =
  7;

const uint16_t TRACK_UNCONFIRMED =
  8;


// =====================================================
// 15. TIME FORMAT
// =====================================================

void formatTime12(
  uint8_t hour24,
  uint8_t minute,
  char* buffer,
  size_t bufferSize
)
{
  uint8_t hour12 =
    hour24 % 12;

  if (hour12 == 0)
  {
    hour12 = 12;
  }

  const char* period =
    (hour24 < 12)
    ? "AM"
    : "PM";

  snprintf(
    buffer,
    bufferSize,
    "%02u:%02u %s",
    hour12,
    minute,
    period
  );
}


void formatTime12WithSeconds(
  uint8_t hour24,
  uint8_t minute,
  uint8_t second,
  char* buffer,
  size_t bufferSize
)
{
  uint8_t hour12 =
    hour24 % 12;

  if (hour12 == 0)
  {
    hour12 = 12;
  }

  const char* period =
    (hour24 < 12)
    ? "AM"
    : "PM";

  snprintf(
    buffer,
    bufferSize,
    "%02u:%02u:%02u %s",
    hour12,
    minute,
    second,
    period
  );
}

// Forward declarations.
// Arduino's automatic prototype generator skips functions that
// carry default arguments, so these two are declared by hand.
void logEvent(const char* eventName, int slotIndex, int fingerprintID = -1);
void tgOnEvent(const char* eventName, int slotIndex);

// Defined in 15H, called from loop() and from the button dispatch.
bool handleBrowseButton(ButtonEvent button);
void serviceForceSelect();
void serviceEnroll();
void refreshEnrollCounts();
int  nextDoseSlot();

// Defined with the rest of the Telegram block further down. They are
// needed up here because okToSleep() must not doze off in the middle
// of sending an alert.
extern uint8_t       tgQueueCount;
extern unsigned long tgRadioUntil;
void safePlayTrack(uint16_t track);
bool saveConfiguration();
void startAuthorizedUnlock();
void epdClockOnly(bool fullRedraw);
extern bool powerSaveEnabled;

// =====================================================
// 15B. BANGLA E-PAPER USER INTERFACE
//
// Integration-8 drew ONE English splash at boot and never
// touched the screen again, so the whole device was only
// usable through the Serial Monitor. This section puts the
// Bangla interface from the specification onto the e-paper.
//
// Two things the BanglaEPD library does not do for us:
//
//  1. PRECOMPOSE. BanglaFont.cpp has no standalone U+09BC
//     NUKTA glyph and no cluster containing one, but it DOES
//     have precomposed  ড়  ঢ়  য়  . Our text stores য় the
//     decomposed way (য + ়), so সময় / হয়নি / নয় / সক্রিয়
//     would each render a black tofu box without this fix.
//     Note this is NOT Unicode NFC - those three codepoints
//     are composition exclusions, so NFC produces exactly the
//     broken form. It has to be a deliberate re-composition.
//
//  2. WORD WRAP. BanglaEPD only breaks on an explicit '\n'
//     and the font is a fixed 28 px. On a 200 px screen many
//     of our strings overflow, so we measure and wrap.
//
// Every string used below was checked against the real font
// tables before being written here: 100% glyph coverage, and
// no screen needs more than 3 of the 5 available lines.
// =====================================================

BanglaEPD<decltype(display)> bangla(display);

const int16_t EPD_TEXT_W  = 190;
const int16_t EPD_MARGIN  = 6;
const int16_t EPD_SCREEN_H = 200;
const uint8_t EPD_LINE_H  = 34;

// One full refresh every N draws, to clear accumulated ghosting.
// Every Nth draw is a full refresh, to wipe the ghosting that
// partial refreshes leave behind. Was 6, which let browsing through
// five dose screens run entirely on partials and look smeared.
const uint8_t EPD_FULL_EVERY = 3;
uint32_t epdDrawCount = 0;

// True whenever the panel has been hibernated. hibernate() drops the
// controller's image RAM, which a partial update needs, so the next
// draw after one MUST be a full refresh or nothing appears on screen.
bool epdHibernated = true;

// Ask for a full hardware reset of the panel on the next draw: RST
// pulsed, waveform tables reloaded, controller image RAM cleared.
//
// True at boot, because a board that reset in the middle of a refresh
// leaves the panel showing torn rubbish that a normal init cannot
// clear. Press 'E' to demand one at any time.
bool epdNeedsHardInit = true;

// Open the panel for drawing. Always call this instead of
// display.init() so the hard-reset decision lives in one place.
void epdBeginDisplay()
{
  if (epdNeedsHardInit)
  {
    // Same slow clock and long reset as setupEPaper(): a hard reset
    // is exactly when the panel is least likely to be in a good mood.
    display.epd2.selectSPI(
      SPI,
      SPISettings(EPD_SPI_HZ, MSBFIRST, SPI_MODE0)
    );

    display.init(115200, true, 50, false);

    epdNeedsHardInit = false;
    epdHibernated    = true;         // force a full-window redraw too

    Serial.println("[E-PAPER] panel hard reset");
  }
  else
  {
    display.init(115200, false);
  }

  display.setRotation(1);
  display.setTextColor(GxEPD_BLACK);
  bangla.setLineHeight(EPD_LINE_H);
}
String epdLastSignature = "";

// Scrub the panel: drive every pixel fully black, then fully white,
// three times over. Slow on purpose - a gentle refresh is what let
// the ghosting build up in the first place.
void epdDeepClean()
{
  epdNeedsHardInit = true;   // start from a known-good controller
  epdBeginDisplay();

  Serial.print("[E-PAPER] deep clean");

  for (uint8_t i = 0; i < 3; i++)
  {
    display.clearScreen(0x00);   // every pixel black
    display.clearScreen(0xFF);   // every pixel white

    Serial.print(".");
  }

  Serial.println(" done");

  // Nothing on screen now matches any cached signature.
  epdLastSignature = "";
  epdHibernated    = true;
}

// ---- Bangla strings (all font-verified) ----
const char* BN_NEXT        = "পরবর্তী";
const char* BN_DOSE_TIME   = "ঔষধ গ্রহণের সময়";
const char* BN_FP_PROMPT   = "আঙুলের ছাপ দিন";
const char* BN_WRONG_USER  = "ভুল ব্যবহারকারী";
const char* BN_RETRY_FP    = "আবার আঙুলের ছাপ দিন";
const char* BN_OPENED      = "খোপ খুলেছে";
const char* BN_TAKE_MED    = "ঔষধ নিন";
const char* BN_PRESS_OK    = "নিশ্চিত করতে চাপুন";
const char* BN_TAKEN       = "ঔষধ গ্রহণ সম্পন্ন";
const char* BN_MISSED      = "ঔষধ গ্রহণ করা হয়নি";
const char* BN_UNCONFIRMED = "ঔষধ গ্রহণ নিশ্চিত হয়নি";
const char* BN_CANCELLED   = "বাতিল করা হয়েছে";
const char* BN_COURSE_DONE = "ঔষধের কোর্স সম্পন্ন";
const char* BN_LAST_DOSE   = "এটি শেষ ঔষধ ছিল";
const char* BN_CLOCK_BAD   = "সময় সঠিক নয়";
const char* BN_SET_TIME    = "সময় নির্ধারণ করুন";
const char* BN_OVERRIDE    = "বিশেষ অনুমতি সক্রিয়";
const char* BN_CLOSE_DOOR  = "দরজা বন্ধ করুন";
const char* BN_FP_ERROR    = "ছাপ যন্ত্রে সমস্যা";
const char* BN_NO_SCHEDULE = "কোনো সময়সূচি নেই";
const char* BN_DEVICE      = "ঔষধ যন্ত্র";
const char* BN_READY       = "প্রস্তুত";
const char* BN_BOX         = "খোপ";
const char* BN_SLEEPING    = "সাশ্রয়ী মোড";

// 15H. Browsing today's doses and moving one forward.
// Every string here was checked against the real font tables first:
// the font has no fallback, so a cluster it lacks is a visible box.
const char* BN_TODAY        = "আজকের ঔষধ";
const char* BN_WAITING      = "অপেক্ষায়";
const char* BN_DONE_TODAY   = "ইতিমধ্যেই শেষ";
const char* BN_MAKE_NEXT    = "পরবর্তী করতে চাপুন";
const char* BN_MOVED        = "পরবর্তী নির্বাচিত";
const char* BN_CANT_PICK    = "নির্বাচন করা যাবে না";
const char* BN_ALREADY_NEXT = "এটিই পরবর্তী";
const char* BN_OTHER_FIRST  = "আগে অন্যজনের ঔষধ আছে";

// 15J. The device screen at the end of the dose list.
const char* BN_DEVICE_INFO  = "সময় ও শব্দ";
const char* BN_VOLUME       = "শব্দ";
const char* BN_VOLUME_SET   = "শব্দ ঠিক করুন";
const char* BN_VOLUME_HINT  = "কম বা বেশি করুন";
const char* BN_VOLUME_SAVED = "শব্দ সংরক্ষিত";

// 15I. Enrolling a fingerprint from the dashboard.
const char* BN_CARETAKER    = "কেয়ারটেকার";
const char* BN_FP_ENROLL    = "ছাপ নিবন্ধন";
const char* BN_FP_OLD       = "পুরানো ছাপ দিন";
const char* BN_FP_MID       = "মধ্য ভাগের ছাপ দিন";
const char* BN_FP_LEFT      = "বাম পাশের ছাপ দিন";
const char* BN_FP_RIGHT     = "ডান পাশের ছাপ দিন";
const char* BN_FP_AGAIN     = "আবার একই ভাবে";
const char* BN_FP_LIFT      = "আঙুল তুলুন";
const char* BN_FP_SAVED     = "নতুন ছাপ সংরক্ষিত";
const char* BN_FP_RETRY     = "আবার চেষ্টা করুন";

// -----------------------------------------------------
// The nukta fix. Safe on any string, including plain ASCII.
// -----------------------------------------------------
String bnPre(
  const String &in
)
{
  String out = in;
  out.replace("ড়", "ড়");   // ড + ়  ->  ড়
  out.replace("ঢ়", "ঢ়");   // ঢ + ়  ->  ঢ়
  out.replace("য়", "য়");   // য + ়  ->  য়
  return out;
}

// -----------------------------------------------------
// Pixel width, mirroring BanglaEPD::print() exactly:
// a space advances 10 px, an unrenderable codepoint 18 px,
// and the longest shaped cluster always wins.
// -----------------------------------------------------
int16_t bnMeasure(
  const char *utf8
)
{
  const char *p = utf8;
  int16_t x = 0;

  while (*p)
  {
    uint32_t firstCp = 0;
    uint8_t firstBytes = utf8ToCodepoint(p, firstCp);

    if (firstCp == '\n')
    {
      p += firstBytes;
      continue;
    }
    if (firstCp == 0x0020)
    {
      x += 10;
      p += firstBytes;
      continue;
    }

    uint32_t cps[BANGLA_MAX_CLUSTER_CP] = {0};
    uint8_t  lens[BANGLA_MAX_CLUSTER_CP] = {0};
    uint8_t  count = 0;
    const char *q = p;

    while (*q && count < BANGLA_MAX_CLUSTER_CP)
    {
      uint32_t cp = 0;
      uint8_t used = utf8ToCodepoint(q, cp);
      if (count > 0 && (cp == 0x0020 || cp == '\n')) break;
      cps[count] = cp;
      lens[count] = used;
      count++;
      q += used;
    }

    bool matched = false;
    for (int len = count; len >= 2; --len)
    {
      const BanglaClusterGlyph *cl =
        getBanglaClusterGlyph(cps, (uint8_t)len);
      if (cl != nullptr)
      {
        x += cl->xAdvance;
        for (int i = 0; i < len; ++i) p += lens[i];
        matched = true;
        break;
      }
    }
    if (matched) continue;

    const BanglaGlyph *gl = getBanglaGlyph(firstCp);
    x += (gl != nullptr) ? gl->xAdvance : 18;
    p += firstBytes;
  }

  return x;
}

// -----------------------------------------------------
// Greedy word wrap. A single over-long word is left to
// overflow rather than split, because breaking inside a
// Bengali cluster corrupts the shaping.
// -----------------------------------------------------
uint8_t bnWrap(
  const String &text,
  int16_t maxWidth,
  String *out,
  uint8_t maxLines
)
{
  uint8_t n = 0;
  String cur = "";
  int start = 0;

  while (start <= (int)text.length() && n < maxLines)
  {
    int sp = text.indexOf(' ', start);
    String word =
      (sp < 0) ? text.substring(start)
               : text.substring(start, sp);

    if (word.length() > 0)
    {
      String cand = cur.length() ? cur + " " + word : word;
      if (bnMeasure(cand.c_str()) <= maxWidth || cur.length() == 0)
      {
        cur = cand;
      }
      else
      {
        out[n++] = cur;
        cur = word;
      }
    }

    if (sp < 0) break;
    start = sp + 1;
  }

  if (cur.length() && n < maxLines) out[n++] = cur;
  return n;
}

// ---- Bangla digits ----
String bnDigits(
  long value
)
{
  const char* D[10] =
  {
    "০","১","২","৩","৪","৫","৬","৭","৮","৯"
  };
  String in = String(value);
  String out = "";
  for (unsigned int i = 0; i < in.length(); i++)
  {
    char c = in[i];
    if (c >= '0' && c <= '9') out += D[c - '0'];
    else out += c;
  }
  return out;
}

// ---- "সকাল ৯:০০" ----
String bnTimeText(
  uint8_t hour24,
  uint8_t minute
)
{
  const char *part;
  if      (hour24 >= 5  && hour24 < 12) part = "সকাল";
  else if (hour24 >= 12 && hour24 < 16) part = "দুপুর";
  else if (hour24 >= 16 && hour24 < 18) part = "বিকাল";
  else                                  part = "রাত";

  uint8_t h12 = hour24 % 12;
  if (h12 == 0) h12 = 12;

  String mm = bnDigits(minute);
  if (minute < 10) mm = "০" + mm;

  return String(part) + " " + bnDigits(h12) + ":" + mm;
}

// -----------------------------------------------------
// One screen draw. Title, a rule, then up to four body
// lines. The panel is woken and hibernated around every
// draw, so no screen depends on another's teardown -
// that was a real bug in the earlier prototype.
// -----------------------------------------------------
void epdFrame(
  const char *title,
  const String *body,
  uint8_t bodyCount
)
{
  // Skip the draw entirely if the screen already shows exactly
  // this. An e-paper refresh is over a second and visibly
  // flickers, so redrawing identical content is worse than
  // useless - and several code paths legitimately ask for the
  // same screen twice in a row.
  String signature = String(title ? title : "");
  for (uint8_t i = 0; i < bodyCount; i++) signature += "|" + body[i];
  if (signature == epdLastSignature) return;
  epdLastSignature = signature;

  epdBeginDisplay();

  // Partial refresh is roughly 4x faster than a full one
  // (~0.3 s against the ~1.3 s you see as _Update_Full in the
  // log) and it does not flash the screen black. The cost is
  // faint ghosting that accumulates, so every EPD_FULL_EVERY
  // draws we do one full refresh to wipe the panel clean.
  bool doFull =
    epdHibernated
    ||
    (epdDrawCount % EPD_FULL_EVERY) == 0;

  epdDrawCount++;
  epdHibernated = false;

  if (doFull || !display.epd2.hasPartialUpdate)
  {
    display.setFullWindow();
  }
  else
  {
    display.setPartialWindow(
      0,
      0,
      display.width(),
      display.height()
    );
  }

  display.firstPage();

  do
  {
    display.fillScreen(GxEPD_WHITE);
    int16_t y = 30;

    if (title != NULL && strlen(title) > 0)
    {
      String tl[3];
      uint8_t n = bnWrap(bnPre(String(title)), EPD_TEXT_W, tl, 3);
      for (uint8_t i = 0; i < n; i++)
      {
        bangla.setCursor(EPD_MARGIN, y);
        bangla.print(tl[i].c_str());
        y += EPD_LINE_H;
      }
      display.drawLine(
        EPD_MARGIN, y - 24,
        200 - EPD_MARGIN, y - 24,
        GxEPD_BLACK
      );
      y += 6;
    }

    for (uint8_t i = 0; i < bodyCount && y < EPD_SCREEN_H; i++)
    {
      String bl[3];
      uint8_t n = bnWrap(bnPre(body[i]), EPD_TEXT_W, bl, 3);
      for (uint8_t k = 0; k < n && y < EPD_SCREEN_H; k++)
      {
        bangla.setCursor(EPD_MARGIN, y);
        bangla.print(bl[k].c_str());
        y += EPD_LINE_H;
      }
    }
  }
  while (display.nextPage());

  // NOTE: deliberately NOT hibernating here.
  // hibernate() powers the panel controller down, which loses the
  // image RAM that a partial update needs - so hibernating after
  // every frame silently forced every refresh to be a full one
  // (that was the _Update_Full in the log). The panel holds its
  // picture with no power regardless; hibernate only saves the
  // controller's ~1 mA, so we now do it once when going idle.
}

// -----------------------------------------------------
// Screens
// -----------------------------------------------------

void epdMessage(
  const char *title,
  const char *body
)
{
  String lines[1];
  uint8_t n = 0;
  if (body != NULL && strlen(body) > 0) lines[n++] = String(body);
  epdFrame(title, lines, n);
}

void epdClockInvalid()
{
  epdMessage(BN_CLOCK_BAD, BN_SET_TIME);
}

// Which dose is genuinely next?
//
// Nearest enabled slot by the clock, EXCEPT that a slot already dealt
// with today - taken, missed, or deliberately skipped - belongs to
// tomorrow, not to today. Without that second rule the idle screen
// would keep advertising a dose that is already over.
//
// Returns -1 when nothing is scheduled at all.
int nextDoseSlot()
{
  if (!rtcOK) return -1;

  DateTime now = rtc.now();

  uint32_t todayKey = (uint32_t)now.year() * 10000UL +
                      (uint32_t)now.month() * 100UL +
                      (uint32_t)now.day();

  uint16_t nowMin = now.hour() * 60 + now.minute();

  int      best      = -1;
  uint32_t bestDelta = 0xFFFFFFFFUL;

  for (int i = 0; i < NUM_SLOTS; i++)
  {
    if (!slots[i].enabled) continue;

    uint16_t slotMin = slots[i].hour * 60 + slots[i].minute;

    uint32_t delta = (slotMin > nowMin)
                     ? (uint32_t)(slotMin - nowMin)
                     : (uint32_t)(1440 - nowMin + slotMin);

    // Still ahead on the clock, but already handled today.
    if (lastTriggeredDateKey[i] == todayKey && slotMin > nowMin)
    {
      delta += 1440;
    }

    if (delta < bestDelta)
    {
      bestDelta = delta;
      best      = i;
    }
  }

  return best;
}

// Idle screen. E-paper holds an image at zero power, so this
// stays readable even with the CPU asleep - which is exactly
// why it shows the NEXT DOSE rather than a live clock that
// would force a wake-up every minute just to stay honest.
void epdIdle()
{
  if (!rtcOK || !configOK)
  {
    epdClockInvalid();
    return;
  }

  // In power-saver mode the idle screen IS the clock face. Without
  // this, finishing a dose would leave the full next-dose screen up
  // and the device would never look like it went back to sleep.
  if (powerSaveEnabled)
  {
    epdClockOnly(true);
    return;
  }

  int bestSlot = nextDoseSlot();

  if (bestSlot < 0)
  {
    epdMessage(BN_NO_SCHEDULE, "");
    return;
  }

  String lines[3];
  lines[0] = String(getUserName(slots[bestSlot].userId));
  lines[1] = bnTimeText(slots[bestSlot].hour, slots[bestSlot].minute);
  lines[2] = String(slots[bestSlot].medName);
  epdFrame(BN_NEXT, lines, 3);

  // Idle is the long-lived screen, so this is the one place it is
  // worth powering the controller down.
  display.hibernate();
  epdHibernated = true;
}

void epdReminder(
  int slot
)
{
  if (slot < 0 || slot >= NUM_SLOTS) return;
  String lines[2];
  lines[0] = String(getUserName(slots[slot].userId));
  lines[1] = bnTimeText(slots[slot].hour, slots[slot].minute);
  epdFrame(BN_DOSE_TIME, lines, 2);
}

void epdFingerprintPrompt(
  int slot
)
{
  if (slot < 0 || slot >= NUM_SLOTS) return;
  String lines[3];
  lines[0] = String(getUserName(slots[slot].userId));
  lines[1] = bnTimeText(slots[slot].hour, slots[slot].minute);
  lines[2] = String(BN_FP_PROMPT);
  epdFrame(BN_DOSE_TIME, lines, 3);
}

void epdWrongUser()
{
  epdMessage(BN_WRONG_USER, BN_RETRY_FP);
}

void epdUnlocked(
  int slot
)
{
  if (slot < 0 || slot >= NUM_SLOTS) return;
  String lines[3];
  lines[0] = String(slots[slot].medName);
  lines[1] = String(BN_BOX) + " " + bnDigits(slots[slot].compartment + 1);
  lines[2] = String(BN_PRESS_OK);
  epdFrame(BN_OPENED, lines, 3);
}

void epdOverride()
{
  epdMessage(BN_OVERRIDE, "");
}

void epdCourseComplete()
{
  epdMessage(BN_COURSE_DONE, BN_LAST_DOSE);
}

void epdOutcome(
  DoseOutcome outcome
)
{
  switch (outcome)
  {
    case OUTCOME_TAKEN:
      epdMessage(BN_TAKEN, "");
      break;
    case OUTCOME_MISSED:
      epdMessage(BN_MISSED, "");
      break;
    case OUTCOME_UNCONFIRMED:
      epdMessage(BN_UNCONFIRMED, "");
      break;
    case OUTCOME_CANCELLED:
      epdMessage(BN_CANCELLED, "");
      break;
    default:
      break;
  }
}

void epdBootScreen()
{
  epdMessage(BN_DEVICE, BN_READY);
}




// =====================================================
// 15C. COURSE DURATION, OVERRIDE, RECOVERY, DOOR SENSING
//
// Integration-8 already stored startEpoch and courseDays in
// every DoseSlot, but nothing ever read them - so a course
// never ended. This section makes them real, and adds the
// three robustness features the device needs before it can
// be left alone with someone's medication.
// =====================================================

// ---- Reed switches: wired later, code ready now ----
// The chain is on GPIO38 in the as-built drawing but is NOT
// installed yet. Set this to 1 after soldering it; nothing
// else needs to change. Until then the confirmation button
// is the only evidence the door was used.
// How long to wait for a finger to come off before giving up.
const unsigned long FINGER_REMOVE_TIMEOUT_MS = 10000;

// ---- Where the voice files live on the SD card ----
//   0 = card ROOT      0001.mp3 ... 0010.mp3   (what Integration-8 used)
//   1 = /MP3/ folder   MP3/0001.mp3 ...
// If the voice never plays, this is the first thing to flip.
// Runtime, not compile-time, so it can be flipped from the Serial
// Monitor with 'F' and tested without a re-upload. Stored in NVS.
//   false = card ROOT      0001.mp3 ...        (Integration-8 behaviour)
//   true  = /MP3/ folder   MP3/0001.mp3 ...
bool mp3UseFolder = false;

// Playback volume, 0-30. 30 is maximum.
// 25, not 30. At full volume the module draws noticeably more current;
// if the 5 V rail sags it can reset or garble its serial replies.
// 25 is what Gopal's original used, and audio worked with it.
// Adjustable from the box - browse past the four doses to the last
// screen - and remembered across restarts.
//
// 25, not 30, as the starting point. At full volume the module draws
// noticeably more current; if the 5 V rail sags it can reset or
// garble its serial replies. 25 is what Gopal's original used and
// audio worked with it. The dial goes to 30 for anyone hard of
// hearing, with that caveat.
uint8_t mp3Volume = 25;

const uint8_t MP3_VOLUME_MAX     = 30;
const uint8_t MP3_VOLUME_DEFAULT = 25;

uint16_t lastTrackPlayed = 0;

// How many files the card actually holds. Asking the module for a
// higher track number does NOT fail quietly - it plays the last file
// it has, so a missing prompt comes out as some other sentence
// entirely. Never request beyond this.
int mp3FileCount = 0;

// Written only when the user finishes adjusting, not on every button
// press - NVS is flash, and a held button would be hundreds of writes.
void saveVolume()
{
  preferences.begin("smrmp3", false);
  preferences.putUChar("vol", mp3Volume);
  preferences.end();

  Serial.printf("[AUDIO] volume saved at %d\n", mp3Volume);
}


// True only once the module has CONFIRMED it can read the card.
// Different from mp3OK, which merely means the module answered.
bool mp3CardOk = false;

unsigned long lastCardProbe = 0;
const unsigned long CARD_PROBE_INTERVAL_MS = 30000;
uint8_t cardRecoveryAttempts = 0;

#define HAS_REED_SENSORS 0
const int PIN_REED_CHAIN = 38;

// ---- Extra voice tracks that Integration-8 never used ----
const uint16_t TRACK_COURSE_DONE = 6;    // "ঔষধের কোর্স সম্পন্ন হয়েছে।"
const uint16_t TRACK_CLOSE_DOOR  = 9;    // "দরজা বন্ধ করুন।"
const uint16_t TRACK_LAST_DOSE   = 10;   // "এটি এই ঔষধের শেষ গ্রহণ ছিল।"

// ---- added for enrolling a finger and for moving a dose ----
//
// Not on the card in the original ten. safePlayTrack just finds
// nothing if they are missing, and the e-paper carries the same
// instruction, so the box works either way. The exact sentences are
// in audio/VOICE-SCRIPT.md.
const uint16_t TRACK_FP_CHANGE   = 11;   // "আঙুলের ছাপ বদলানো হবে। পুরানো ছাপ দিন।"
const uint16_t TRACK_FP_MID      = 12;   // "মধ্য ভাগের ছাপ দিন।"
const uint16_t TRACK_FP_LEFT     = 13;   // "বাম পাশের ছাপ দিন।"
const uint16_t TRACK_FP_RIGHT    = 14;   // "ডান পাশের ছাপ দিন।"
const uint16_t TRACK_FP_AGAIN    = 15;   // "আবার একই ভাবে দিন।"
const uint16_t TRACK_FP_SAVED    = 16;   // "নতুন ছাপ সংরক্ষিত হয়েছে।"
const uint16_t TRACK_DOSE_MOVED  = 17;   // "পরবর্তী ঔষধ নির্বাচিত হয়েছে।"

// What should be on the card. Checked at boot, so a half-copied
// card is reported then rather than discovered later when a
// prompt simply stays silent.
const int TOTAL_TRACKS = 17;

// ---- Caregiver override ----
// UP + DOWN held together. Never silent: it is always logged
// with a reason, so an override can never be mistaken later
// for a normal fingerprint unlock.
const unsigned long OVERRIDE_HOLD_MS = 3000;
// How long an error message stays on the e-paper before the
// fingerprint prompt comes back.
const unsigned long WRONG_USER_SCREEN_MS = 6000;
unsigned long wrongUserScreenUntil = 0;

unsigned long overrideHoldStarted = 0;
bool overrideAlreadyFired = false;

// ---- Power-outage catch-up ----
// A dose that fell while the device was off is recorded as
// MISSED rather than silently vanishing.
const uint32_t CATCHUP_MAX_LOOKBACK_SEC = 7UL * 86400UL;

// -----------------------------------------------------
// Course rules
// -----------------------------------------------------

// courseDays == 0 (or no start date) means "keep going
// indefinitely", which is what a long-term prescription is.
bool slotInCourse(
  int slotIndex,
  uint32_t atEpoch
)
{
  if (slotIndex < 0 || slotIndex >= NUM_SLOTS) return false;
  if (!slots[slotIndex].enabled) return false;
  if (slots[slotIndex].courseDays == 0) return true;
  if (slots[slotIndex].startEpoch == 0) return true;

  uint32_t endEpoch =
    slots[slotIndex].startEpoch +
    (uint32_t)slots[slotIndex].courseDays * 86400UL;

  return atEpoch < endEpoch;
}

// True when tomorrow's occurrence would fall outside the
// course, i.e. this is the last dose the patient will take.
bool slotIsFinalDose(
  int slotIndex,
  uint32_t doseEpoch
)
{
  if (slotIndex < 0 || slotIndex >= NUM_SLOTS) return false;
  if (slots[slotIndex].courseDays == 0) return false;
  if (slots[slotIndex].startEpoch == 0) return false;

  uint32_t endEpoch =
    slots[slotIndex].startEpoch +
    (uint32_t)slots[slotIndex].courseDays * 86400UL;

  return (doseEpoch + 86400UL) >= endEpoch;
}

// Called after a successful final dose: announce it, then
// switch the slot off so it stops reminding.
void completeCourse(
  int slotIndex
)
{
  if (slotIndex < 0 || slotIndex >= NUM_SLOTS) return;

  Serial.println();
  Serial.println(
    ">>> COURSE COMPLETE - SLOT DISABLED <<<"
  );

  safePlayTrack(TRACK_LAST_DOSE);
  safePlayTrack(TRACK_COURSE_DONE);
  epdCourseComplete();

  slots[slotIndex].enabled = false;
  saveConfiguration();

  logEvent(
    "COURSE_COMPLETE",
    slotIndex
  );
}

// -----------------------------------------------------
// Caregiver override
//
// Returns true once per hold. Only meaningful while a dose
// is waiting for a fingerprint - it is never a general
// "open the box" button.
// -----------------------------------------------------
bool checkOverrideCombo()
{
  bool upHeld   = (stableState[0] == LOW);
  bool downHeld = (stableState[1] == LOW);

  if (!upHeld || !downHeld)
  {
    overrideHoldStarted = 0;
    overrideAlreadyFired = false;
    return false;
  }

  if (overrideHoldStarted == 0)
  {
    overrideHoldStarted = millis();
    return false;
  }

  if (overrideAlreadyFired) return false;

  if (millis() - overrideHoldStarted >= OVERRIDE_HOLD_MS)
  {
    overrideAlreadyFired = true;
    return true;
  }

  return false;
}

// -----------------------------------------------------
// Door sensing (reed chain on GPIO38)
//
// LOW  = every door closed
// HIGH = at least one door open
// With no sensors fitted we report "closed" so the rest of
// the logic behaves exactly as it does today.
// -----------------------------------------------------
void setupReedSensors()
{
#if HAS_REED_SENSORS
  pinMode(PIN_REED_CHAIN, INPUT_PULLUP);
  Serial.println("[REED] Door sensors enabled");
#endif
}

bool allDoorsClosed()
{
#if HAS_REED_SENSORS
  return digitalRead(PIN_REED_CHAIN) == LOW;
#else
  return true;
#endif
}

// -----------------------------------------------------
// Power-outage catch-up
//
// On boot, compare the last processed time against now and
// log every enabled dose that fell in the gap as MISSED.
// Without this a power cut silently erases history, which is
// the one thing an adherence log must never do.
// -----------------------------------------------------
uint32_t lastProcessedEpoch = 0;

void loadLastProcessedEpoch()
{
  preferences.begin("smrtime", true);
  lastProcessedEpoch =
    preferences.getUInt("lastProc", 0);
  preferences.end();
}

void saveLastProcessedEpoch(
  uint32_t epoch
)
{
  lastProcessedEpoch = epoch;
  preferences.begin("smrtime", false);
  preferences.putUInt("lastProc", epoch);
  preferences.end();
}

uint16_t catchUpMissedDoses()
{
  if (!rtcOK || !configOK) return 0;

  DateTime now = rtc.now();
  uint32_t nowEpoch = now.unixtime();

  if (lastProcessedEpoch == 0 || nowEpoch <= lastProcessedEpoch)
  {
    saveLastProcessedEpoch(nowEpoch);
    return 0;
  }

  uint32_t from = lastProcessedEpoch;
  if (nowEpoch - from > CATCHUP_MAX_LOOKBACK_SEC)
  {
    // A flat coin cell could otherwise generate thousands of rows.
    from = nowEpoch - CATCHUP_MAX_LOOKBACK_SEC;
  }

  uint16_t logged = 0;

  for (int i = 0; i < NUM_SLOTS; i++)
  {
    if (!slots[i].enabled) continue;

    for (uint32_t ref = from; ref <= nowEpoch + 86400UL; ref += 86400UL)
    {
      DateTime day(ref);
      DateTime occ(
        day.year(), day.month(), day.day(),
        slots[i].hour, slots[i].minute, 0
      );
      uint32_t occEpoch = occ.unixtime();

      if (occEpoch <= from) continue;
      if (occEpoch > nowEpoch) continue;
      if (!slotInCourse(i, occEpoch)) continue;

      logEvent("MISSED_POWER_OUT", i);
      logged++;
    }
  }

  saveLastProcessedEpoch(nowEpoch);

  if (logged > 0)
  {
    Serial.printf(
      "[RECOVERY] %u dose(s) missed while powered off - logged\n",
      logged
    );
  }

  return logged;
}



// =====================================================
// 15D. DIAGNOSTICS
// =====================================================

// Print the flash partition table. When FFat refuses to mount
// this says why in one glance: if there is no line of type
// "data" subtype "fat", the wrong Partition Scheme is selected
// in Tools and no amount of code will fix it.
void printPartitionTable()
{
  Serial.println();
  Serial.println(
    "---------- FLASH PARTITION TABLE ----------"
  );
  Serial.println(
    "label            type  sub      offset      size"
  );

  bool foundFat = false;

  esp_partition_iterator_t it =
    esp_partition_find(
      ESP_PARTITION_TYPE_ANY,
      ESP_PARTITION_SUBTYPE_ANY,
      NULL
    );

  while (it != NULL)
  {
    const esp_partition_t* part =
      esp_partition_get(it);

    Serial.printf(
      "%-16s %4d  %3d  0x%08X  %8u",
      part->label,
      (int)part->type,
      (int)part->subtype,
      (unsigned int)part->address,
      (unsigned int)part->size
    );

    if (
      part->type == ESP_PARTITION_TYPE_DATA
      &&
      part->subtype == ESP_PARTITION_SUBTYPE_DATA_FAT
    )
    {
      Serial.print("   <-- FATFS");
      foundFat = true;
    }

    Serial.println();
    it = esp_partition_next(it);
  }

  esp_partition_iterator_release(it);

  Serial.println(
    "-------------------------------------------"
  );

  if (!foundFat)
  {
    Serial.println();
    Serial.println(
      "*** NO FATFS PARTITION EXISTS ON THIS BOARD ***"
    );
    Serial.println(
      "    The event log cannot work until this is fixed."
    );
    Serial.println(
      "    In Arduino IDE:  Tools > Partition Scheme >"
    );
    Serial.println(
      "      16M Flash (3MB APP/9.9MB FATFS)"
    );
    Serial.println(
      "    then upload again."
    );
    Serial.println();
  }
  else
  {
    Serial.println(
      "A FATFS partition exists, so the scheme is correct."
    );
    Serial.println();
  }
}



// =====================================================
// 15E. RTC ALARM + POWER MANAGEMENT
//
// The device is battery powered and portable, so it cannot sit
// at full power all day waiting for a dose.
//
// Design note: the DS3231 alarm is used ONLY as a wake source.
// checkScheduler() still decides what actually fires. A 15 s
// timer wake runs alongside it, so even if the alarm is somehow
// missed the dose is still caught within 15 seconds. Belt and
// braces - a medicine reminder that silently skips a dose is
// worse than one that wastes a little power.
//
// Sleep is OFF by default. Light sleep drops the USB serial
// connection, which makes bench work miserable, so it is a
// runtime setting you switch on once the device runs on battery.
// =====================================================

bool powerSaveEnabled = false;

// Wake up at least this often even with nothing else happening.
const uint64_t SLEEP_TICK_US = 15ULL * 1000000ULL;

// Stay awake this long after any button press, so the menu is
// usable without the device dozing off between key presses.
const unsigned long STAY_AWAKE_AFTER_INPUT_MS = 30000;
unsigned long lastInputActivity = 0;

void noteInputActivity()
{
  lastInputActivity = millis();
}

// -----------------------------------------------------
// DS3231 alarm
// -----------------------------------------------------

void setupRtcAlarm()
{
  if (!rtcOK) return;

  // SQW must be OFF for the pin to behave as an alarm interrupt
  // rather than a square-wave output.
  rtc.disable32K();
  rtc.writeSqwPinMode(DS3231_OFF);
  rtc.disableAlarm(2);
  rtc.clearAlarm(1);
  rtc.clearAlarm(2);

  pinMode(RTC_INT, INPUT_PULLUP);

  Serial.println(
    "[RTC] alarm output armed on GPIO21"
  );
}

// Program Alarm1 for the next enabled, in-course dose.
void armNextDoseAlarm()
{
  if (!rtcOK || !configOK) return;

  DateTime now = rtc.now();
  uint32_t nowEpoch = now.unixtime();

  uint32_t best = 0;
  int bestSlot = -1;

  // Today and tomorrow is enough for a daily schedule.
  for (int day = 0; day < 2; day++)
  {
    DateTime ref(nowEpoch + (uint32_t)day * 86400UL);

    for (int i = 0; i < NUM_SLOTS; i++)
    {
      if (!slots[i].enabled) continue;

      DateTime occ(
        ref.year(), ref.month(), ref.day(),
        slots[i].hour, slots[i].minute, 0
      );
      uint32_t occEpoch = occ.unixtime();

      if (occEpoch <= nowEpoch) continue;
      if (!slotInCourse(i, occEpoch)) continue;

      if (bestSlot < 0 || occEpoch < best)
      {
        best = occEpoch;
        bestSlot = i;
      }
    }

    if (bestSlot >= 0) break;
  }

  rtc.clearAlarm(1);

  if (bestSlot < 0)
  {
    Serial.println(
      "[RTC] no active dose - no alarm set"
    );
    return;
  }

  DateTime target(best);

  if (rtc.setAlarm1(target, DS3231_A1_Date))
  {
    Serial.print("[RTC] wake alarm set for slot ");
    Serial.print(bestSlot + 1);
    Serial.print(" at ");
    Serial.print(target.hour());
    Serial.print(":");
    if (target.minute() < 10) Serial.print("0");
    Serial.println(target.minute());
  }
  else
  {
    Serial.println(
      "[RTC] FAILED to set wake alarm"
    );
  }
}

void clearDoseAlarm()
{
  if (!rtcOK) return;

  // Mandatory. If A1F is left set the INT line stays LOW and the
  // device wakes again immediately, forever.
  rtc.clearAlarm(1);
}

// -----------------------------------------------------
// Light sleep
// -----------------------------------------------------

bool okToSleep()
{
  if (!powerSaveEnabled) return false;

  // Never sleep in the middle of a dose.
  if (reminderState != REMINDER_IDLE) return false;

  // Never sleep while someone is using the buttons.
  if (millis() - lastInputActivity < STAY_AWAKE_AFTER_INPUT_MS) return false;

  // Never sleep with a command half-typed on the serial port.
  if (Serial.available()) return false;

  // Never sleep while an alert is still trying to go out. Light sleep
  // suspends the radio, so dozing off here would drop the connection
  // mid-send and the message would never leave. The radio window is
  // capped at two minutes, so this cannot keep the device awake.
  if (tgQueueCount > 0 && tgRadioUntil != 0) return false;

  // Never light-sleep while USB is connected. Sleeping kills the USB
  // CDC peripheral, and the next Serial.print() then blocks forever
  // waiting for a host that has gone away - freezing the whole loop.
  // On battery there is no USB, so this costs nothing in real use.
  if (Serial)
  {
    static bool warned = false;
    if (!warned)
    {
      warned = true;
      Serial.println(
        "[PWR] USB connected - screen saves power, chip stays awake."
      );
      Serial.println(
        "[PWR] Unplug USB and run on battery for real sleep."
      );
    }
    return false;
  }

  return true;
}

// -----------------------------------------------------
// Power-saver display
//
// In power-saver mode the screen shows ONLY the clock. There is no
// point redrawing the next user and medicine every few minutes when
// nobody is looking - e-paper costs real power to change, and the
// full information appears anyway the moment a dose fires.
// -----------------------------------------------------

unsigned long lastClockDraw = 0;
String epdLastClockText = "";

// How often the clock face is redrawn while dozing. Five minutes
// is ~288 refreshes a day, about 90 seconds of panel activity in
// total, which is negligible.
const unsigned long CLOCK_REFRESH_MS = 2UL * 60UL * 1000UL;

// The clock face is drawn by hand rather than through epdFrame(),
// because only the time line changes. Repainting the whole 200x200
// panel every two minutes would flash the screen black and waste
// far more energy than redrawing one 56 px band.
//
// Note: this screen deliberately does NOT hibernate. hibernate()
// drops the controller's image RAM, and a partial update needs it -
// hibernating here would force every refresh back to a slow full one.
const int16_t CLOCK_BAND_H = 56;

void epdClockOnly(bool fullRedraw)
{
  if (!rtcOK)
  {
    epdClockInvalid();
    return;
  }

  DateTime now = rtc.now();
  String timeText = bnPre(bnTimeText(now.hour(), now.minute()));

  // Nothing to do if the displayed minute has not changed.
  if (!fullRedraw && timeText == epdLastClockText)
  {
    lastClockDraw = millis();
    return;
  }
  epdLastClockText = timeText;

  epdBeginDisplay();

  if (fullRedraw || epdHibernated)
  {
    // Whole screen: the time, a rule, and the mode label underneath.
    display.setFullWindow();
    display.firstPage();
    do
    {
      display.fillScreen(GxEPD_WHITE);

      bangla.setCursor(EPD_MARGIN, 40);
      bangla.print(timeText.c_str());

      display.drawLine(
        EPD_MARGIN, 52,
        200 - EPD_MARGIN, 52,
        GxEPD_BLACK
      );

      bangla.setCursor(EPD_MARGIN, 96);
      bangla.print(bnPre(String(BN_SLEEPING)).c_str());
    }
    while (display.nextPage());

    epdHibernated = false;
  }
  else
  {
    // Just the time band. The rule and the label below are untouched.
    display.setPartialWindow(0, 0, 200, CLOCK_BAND_H);
    display.firstPage();
    do
    {
      display.fillRect(0, 0, 200, CLOCK_BAND_H, GxEPD_WHITE);

      bangla.setCursor(EPD_MARGIN, 40);
      bangla.print(timeText.c_str());

      display.drawLine(
        EPD_MARGIN, 52,
        200 - EPD_MARGIN, 52,
        GxEPD_BLACK
      );
    }
    while (display.nextPage());
  }

  // The normal screens use a signature to skip identical redraws.
  // Clear it so the next full screen always paints.
  epdLastSignature = "";

  lastClockDraw = millis();
}

// Refresh the clock face now and then while dozing.
void serviceClockFace()
{
  if (!powerSaveEnabled) return;
  if (reminderState != REMINDER_IDLE) return;

  if (
    lastClockDraw == 0
    ||
    millis() - lastClockDraw >= CLOCK_REFRESH_MS
  )
  {
    // false = partial update of the time band only.
    epdClockOnly(false);
  }
}

// -----------------------------------------------------
// Leaving power-saver mode
//
// A quick BACK press only wakes the chip for a moment and it dozes
// straight off again, which looks like nothing happened. Holding
// BACK for three seconds switches power saving OFF properly and
// puts the device back to full behaviour.
// -----------------------------------------------------

const unsigned long EXIT_POWERSAVE_HOLD_MS = 3000;
unsigned long psExitHoldStarted = 0;

unsigned long psEnterHoldStarted = 0;

void serviceEnterPowerSave()
{
  if (powerSaveEnabled) return;

  // Only when nothing is happening. During a dose this same
  // combination is the caregiver override, so the two can never
  // be mistaken for one another.
  if (reminderState != REMINDER_IDLE)
  {
    psEnterHoldStarted = 0;
    return;
  }

  bool upHeld   = (digitalRead(PIN_BTN_UP)   == LOW);
  bool downHeld = (digitalRead(PIN_BTN_DOWN) == LOW);

  if (!upHeld || !downHeld)
  {
    psEnterHoldStarted = 0;
    return;
  }

  if (psEnterHoldStarted == 0)
  {
    psEnterHoldStarted = millis();
    return;
  }

  if (millis() - psEnterHoldStarted < EXIT_POWERSAVE_HOLD_MS) return;

  psEnterHoldStarted = 0;
  powerSaveEnabled = true;

  preferences.begin("smrpwr", false);
  preferences.putBool("save", true);
  preferences.end();

  Serial.println();
  Serial.println(
    "[PWR] UP+DOWN held - power saving ON"
  );
  Serial.println(
    "[PWR] hold BACK 3s to return to full mode"
  );

  epdLastSignature = "";
  lastClockDraw = 0;
  epdClockOnly(true);
}

void serviceExitPowerSave()
{
  if (!powerSaveEnabled) return;

  if (digitalRead(PIN_BTN_BACK) != LOW)
  {
    psExitHoldStarted = 0;
  uiMode = UI_IDLE;
    return;
  }

  if (psExitHoldStarted == 0)
  {
    psExitHoldStarted = millis();
    return;
  }

  if (millis() - psExitHoldStarted < EXIT_POWERSAVE_HOLD_MS) return;

  psExitHoldStarted = 0;
  powerSaveEnabled = false;

  preferences.begin("smrpwr", false);
  preferences.putBool("save", false);
  preferences.end();

  noteInputActivity();

  Serial.println();
  Serial.println(
    "[PWR] BACK held - power saving OFF, full mode restored"
  );

  // Force a real redraw: the panel has been showing a clock face.
  epdLastSignature = "";
  epdIdle();

  showMenu();
}



void serviceLightSleep()
{
  if (!okToSleep()) return;

  // Wake on: the RTC alarm going LOW, the BACK button going LOW,
  // or the timer - whichever happens first.
  gpio_wakeup_enable((gpio_num_t)RTC_INT, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)PIN_BTN_BACK, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup(SLEEP_TICK_US);

  esp_light_sleep_start();

  // Execution resumes here. RAM is intact and millis() kept running.
  esp_task_wdt_reset();

  if (rtcOK && rtc.alarmFired(1))
  {
    Serial.println();
    Serial.println(
      "[WAKE] RTC alarm - a dose is due"
    );
    clearDoseAlarm();
  }
}


// =====================================================
// 15F. WI-FI + LOCAL CAREGIVER DASHBOARD
//
// Integration 11. Offline-first: the medicine flow never depends
// on the network, and the network never blocks the medicine flow.
//
// Wi-Fi state simply follows the power mode:
//     power saving ON   ->  radio OFF   (clock face, dozing)
//     power saving OFF  ->  radio ON    (dashboard reachable)
//
// So the same two button holds that switch power mode also switch
// Wi-Fi. There is no third thing to learn.
//
// The device runs its OWN access point, so the dashboard works
// with no router and no internet at all. Home Wi-Fi credentials
// are optional and only used for clock sync.
//
// The server is ASYNC on purpose. A blocking server would stall
// loop() while serving a page, and loop() is what runs the dose
// timers, the fingerprint poll and the lock pulse.
//
// There is deliberately NO unlock endpoint. Not disabled - absent.
// Compartment access is physical and biometric, and that is the
// entire point of the product.
// =====================================================

const char* AP_SSID     = "Oshudh-Box";
const char* AP_PASSWORD = "oshudh1234";
const char* MDNS_NAME   = "oshudh";

// Gates every write. Reads stay open - seeing the history is
// harmless, silently retiming someone's medication is not.
char adminPin[8] = "1234";

// Load the saved PIN, or keep the factory one on a new device.
void loadAdminPin()
{
  preferences.begin("smrsec", true);
  String p = preferences.getString("pin", "1234");
  preferences.end();

  if (p.length() >= 4 && p.length() < sizeof(adminPin))
  {
    strncpy(adminPin, p.c_str(), sizeof(adminPin) - 1);
    adminPin[sizeof(adminPin) - 1] = 0;
  }
}

AsyncWebServer webServer(80);
DNSServer dnsServer;

bool wifiUp      = false;
bool routesReady = false;
bool serverBegun  = false;
bool wifiStaOnly = false;

// =====================================================
// 15G. TELEGRAM ALERTS  (optional, outbound only)
//
// The family gets a message when a dose is missed, and a summary of
// the whole day each evening. That replaces the cloud dashboard: the
// history arrives on their phone instead of them going to look for it.
//
// Two things are worth being clear about.
//
// 1. Telegram bots address people by CHAT ID, not by phone number.
//    A phone number cannot be used here at all. The chat ID is a
//    number Telegram assigns, and the device can learn it by itself -
//    see tgLinkChat() - so nobody has to go hunting for it.
//
// 2. Nothing here ever listens. The device only ever makes outgoing
//    requests. There is no path from Telegram back into the box, so a
//    stolen bot token cannot open a compartment. The single inbound
//    read, tgLinkChat(), happens only while somebody is standing at
//    the dashboard pressing the button.
// =====================================================

const uint8_t TG_MAX_RECIPIENTS = 2;
const uint8_t TG_QUEUE_LEN      = 6;

// How long to hold the radio up after a missed dose, purely to get the
// alert out. Long enough to associate, resolve DNS and do a TLS
// handshake on a slow home router; it closes early once the queue is
// empty, so this is a ceiling and not a cost.
const unsigned long TG_RADIO_WINDOW_MS = 120000UL;

bool     tgEnabled = false;
String   tgToken   = "";
String   tgChat[TG_MAX_RECIPIENTS]  = { "", "" };
String   tgLabel[TG_MAX_RECIPIENTS] = { "", "" };
uint8_t  tgSummaryHour   = 21;
uint8_t  tgSummaryMinute = 0;
uint32_t tgSummarySent = 0;        // YYYYMMDD of the last summary sent

String   tgQueue[TG_QUEUE_LEN];
uint8_t  tgQueueCount = 0;

unsigned long tgRadioUntil   = 0;  // millis deadline for the radio window

// Looking up a chat ID means a full HTTPS request, which takes seconds
// and needs several kilobytes of stack. The web server's callback has
// neither to spare - it runs on the AsyncTCP task, whose stack is
// small and which must never block, or every other request stalls
// behind it. So the request only raises a flag here and the work is
// done from loop(); the page polls for the answer.
volatile bool tgLinkWanted = false;   // set by the web handler
volatile uint8_t tgLinkState = 0;     // 0 idle, 1 working, 2 found, 3 failed

// Plain buffer, not a String: this is written by loop() and read by
// the web server task, and a String would move its heap buffer under
// the reader's feet. Always fill this BEFORE publishing tgLinkState.
char tgLinkResult[96] = "";

void tgLinkFinish(uint8_t state, const char* text)
{
  strncpy(tgLinkResult, text, sizeof(tgLinkResult) - 1);
  tgLinkResult[sizeof(tgLinkResult) - 1] = 0;

  tgLinkState = state;   // published last, on purpose
}
unsigned long tgLastAttempt  = 0;
uint8_t       tgFailStreak   = 0;

// Repeated wrong fingers are only worth reporting if they keep
// happening - one fumbled read is normal and must not cry wolf.
uint8_t       tgBadFingerCount = 0;
unsigned long tgBadFingerFirst = 0;
const uint8_t       TG_BAD_FINGER_LIMIT     = 3;
const unsigned long TG_BAD_FINGER_WINDOW_MS = 300000UL;

bool tgConfigured()
{
  if (!tgEnabled) return false;
  if (tgToken.length() < 20) return false;
  return tgChat[0].length() > 0 || tgChat[1].length() > 0;
}

void tgLoad()
{
  preferences.begin("smrtg", true);
  tgEnabled     = preferences.getBool("en", false);
  tgToken       = preferences.getString("tok", "");
  tgChat[0]     = preferences.getString("c1", "");
  tgChat[1]     = preferences.getString("c2", "");
  tgLabel[0]    = preferences.getString("n1", "");
  tgLabel[1]    = preferences.getString("n2", "");
  tgSummaryHour   = preferences.getUChar("sumh", 21);
  tgSummaryMinute = preferences.getUChar("summ", 0);
  tgSummarySent = preferences.getUInt("sumd", 0);
  preferences.end();

  if (tgSummaryHour   > 23) tgSummaryHour   = 21;
  if (tgSummaryMinute > 59) tgSummaryMinute = 0;
}

void tgSave()
{
  preferences.begin("smrtg", false);
  preferences.putBool("en", tgEnabled);
  preferences.putString("tok", tgToken);
  preferences.putString("c1", tgChat[0]);
  preferences.putString("c2", tgChat[1]);
  preferences.putString("n1", tgLabel[0]);
  preferences.putString("n2", tgLabel[1]);
  preferences.putUChar("sumh", tgSummaryHour);
  preferences.putUChar("summ", tgSummaryMinute);
  preferences.end();
}

// Percent-encode a UTF-8 string byte by byte. Bangla goes over the
// wire as multi-byte UTF-8, so this has to work on bytes, not chars.
String tgUrlEncode(const String &in)
{
  String out;
  out.reserve(in.length() * 2);

  for (size_t i = 0; i < in.length(); i++)
  {
    uint8_t c = (uint8_t)in[i];

    if (
      (c >= 'A' && c <= 'Z') ||
      (c >= 'a' && c <= 'z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == '~'
    )
    {
      out += (char)c;
    }
    else
    {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }

  return out;
}

bool tgOnline()
{
  return WiFi.status() == WL_CONNECTED;
}

// One HTTPS POST to one chat. Blocks for as long as the handshake
// takes, which is why every caller checks first that no dose is in
// progress.
bool tgPost(const String &chatId, const String &text)
{
  if (chatId.length() == 0) return true;   // nothing to do, not a failure

  WiFiClientSecure client;

  // No certificate pinning. The device has no reliable wall clock at
  // boot and no way to be handed a new root when one expires, and a
  // pinned certificate that silently stops working would be worse
  // than this: everything sent here is a status message the family
  // already knows, and nothing sensitive travels in either direction.
  client.setInsecure();
  client.setTimeout(10);              // seconds
  client.setHandshakeTimeout(10);    // seconds

  HTTPClient https;
  String url = "https://api.telegram.org/bot" + tgToken + "/sendMessage";

  if (!https.begin(client, url)) return false;

  https.setTimeout(10000);           // milliseconds
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "chat_id=" + tgUrlEncode(chatId) +
                "&text="   + tgUrlEncode(text);

  int code = https.POST(body);
  https.end();

  if (code != 200)
  {
    Serial.printf("[TG] send failed, HTTP %d\n", code);
    return false;
  }

  return true;
}

// Which recipient of the message at the head of the queue is next.
// Deliberately NOT a loop over both: see serviceTelegram().
uint8_t tgSendIdx = 0;

// Index of the next configured recipient at or after `from`,
// or TG_MAX_RECIPIENTS when there are none left.
uint8_t tgNextRecipient(uint8_t from)
{
  for (uint8_t i = from; i < TG_MAX_RECIPIENTS; i++)
  {
    if (tgChat[i].length() > 0) return i;
  }

  return TG_MAX_RECIPIENTS;
}

// Put a message in line and make sure the radio comes up for it. The
// queue is deliberately small: if alerts are piling up faster than
// they can be sent, the newest ones matter and the oldest do not.
void tgQueueMessage(const String &text)
{
  if (!tgConfigured()) return;

  // Already waiting to go out? Then this is a double press, or a
  // retry racing the original. Either way, do not send it twice.
  for (uint8_t i = 0; i < tgQueueCount; i++)
  {
    if (tgQueue[i] == text)
    {
      Serial.println("[TG] identical message already queued - ignored");
      return;
    }
  }

  if (tgQueueCount >= TG_QUEUE_LEN)
  {
    for (uint8_t i = 1; i < TG_QUEUE_LEN; i++) tgQueue[i - 1] = tgQueue[i];
    tgQueueCount = TG_QUEUE_LEN - 1;
    tgSendIdx = 0;
  }

  tgQueue[tgQueueCount++] = text;

  // This is the radio window. In power-saving mode the Wi-Fi is off,
  // so a missed dose would otherwise sit unreported until somebody
  // walked over to the box - which defeats the point of alerting.
  tgRadioUntil = millis() + TG_RADIO_WINDOW_MS;

  Serial.printf("[TG] queued (%d waiting), radio window open\n", tgQueueCount);
}

// Build the end-of-day summary straight off the event log.
String tgBuildSummary(const DateTime &now)
{
  char today[12];
  snprintf(today, sizeof(today), "%04d-%02d-%02d",
           now.year(), now.month(), now.day());

  uint16_t nT = 0, nM = 0, nU = 0;
  String detail = "";

  if (logStorageOK)
  {
    File f = FFat.open(LOG_FILE, FILE_READ);

    if (f)
    {
      while (f.available())
      {
        String line = f.readStringUntil('\n');
        if (!line.startsWith(today)) continue;

        bool taken  = line.indexOf(",TAKEN,") >= 0;
        bool missed = line.indexOf(",MISSED") >= 0;
        bool unconf = line.indexOf(",UNCONFIRMED,") >= 0;

        if (!taken && !missed && !unconf) continue;

        if (taken) nT++; else if (missed) nM++; else nU++;

        // "YYYY-MM-DD HH:MM:SS,EVENT,uid,name,slot,comp,med,fp"
        String hhmm = line.substring(11, 16);

        int f3 = -1, f4 = -1, f6 = -1, f7 = -1, seen = 0;
        for (size_t i = 0; i < line.length(); i++)
        {
          if (line[i] != ',') continue;
          seen++;
          if (seen == 3) f3 = i;
          else if (seen == 4) f4 = i;
          else if (seen == 6) f6 = i;
          else if (seen == 7) { f7 = i; break; }
        }

        String who = (f3 >= 0 && f4 > f3) ? line.substring(f3 + 1, f4) : "-";
        String med = (f6 >= 0 && f7 > f6) ? line.substring(f6 + 1, f7) : "-";

        detail += taken  ? "✅ " : (missed ? "❌ " : "⚠️ ");
        detail += hhmm + " - " + who + " - " + med;
        detail += taken ? "\n" : (missed ? " (গ্রহণ "
                                           "করা হয়নি)\n"
                                         : " (নিশ্চিত "
                                           "হয়নি)\n");
      }

      f.close();
    }
  }

  String m = "\U0001F4CB আজকের হিসাব  ";
  m += String(now.day()) + "/" + String(now.month()) + "/" + String(now.year()) + "\n\n";

  if (nT + nM + nU == 0)
  {
    m += "আজ কোনো ডোজ ছিল না।";
    return m;
  }

  m += detail;
  m += "\nগৃহীত: " + String(nT);
  m += "   বাদ: " + String(nM);
  m += "   অনিশ্চিত: " + String(nU);

  return m;
}

// Decide whether an event deserves a message. Called from logEvent so
// that every place that records something is covered automatically,
// rather than five call sites each having to remember.
void tgOnEvent(const char* eventName, int slotIndex)
{
  if (!tgConfigured()) return;

  // Cheap check first. This also runs on the web server's task when a
  // schedule edit is logged, and most events are not worth reporting.
  String e = String(eventName);

  if (e != "MISSED" && e != "MISSED_POWER_OUT" && e != "UNCONFIRMED" &&
      e != "OVERRIDE" && e != "COURSE_COMPLETE" && e != "CARETAKER_OPEN" &&
      e != "FINGERPRINT_CHANGED" &&
      e != "UNKNOWN_FINGER" && e != "WRONG_USER")
  {
    return;
  }

  String who = "-", med = "-", at = "";

  if (slotIndex >= 0 && slotIndex < NUM_SLOTS)
  {
    who = String(getUserName(slots[slotIndex].userId));
    med = String(slots[slotIndex].medName);

    char t[12];
    formatTime12(slots[slotIndex].hour, slots[slotIndex].minute, t, sizeof(t));
    at = String(t);
  }

  String m = "";

  if (e == "MISSED")
  {
    m  = "❌ ঔষধ গ্রহণ "
         "করা হয়নি\n\n";
    m += who + " — " + med + "\n" + at;
  }
  else if (e == "MISSED_POWER_OUT")
  {
    m  = "❌ বিদ্যুৎ ছিল "
         "না, ডোজ বাদ গেছে\n\n";
    m += who + " — " + med + "\n" + at;
  }
  else if (e == "UNCONFIRMED")
  {
    m  = "⚠️ ঔষধ গ্রহণ "
         "নিশ্চিত হয়নি\n\n";
    m += who + " — " + med + "\n" + at;
    m += "\nবাক্স খুলেছিল, "
         "কিন্তু নিশ্চিত "
         "করা হয়নি।";
  }
  else if (e == "OVERRIDE")
  {
    m  = "\U0001F513 বিশেষ অনুমতি"
         "তে খোলা হয়েছে\n\n";
    m += who + " — " + med + "\n" + at;
  }
  else if (e == "CARETAKER_OPEN")
  {
    m  = "\U0001F513 কেয়ারটেকার "
         "খুলেছে\n\n";
    m += who + " \u2014 " + med + "\n" + at;
  }
  else if (e == "FINGERPRINT_CHANGED")
  {
    m  = "\U0001F510 আঙুলের ছাপ "
         "বদলানো হয়েছে";
  }
  else if (e == "COURSE_COMPLETE")
  {
    m  = "\U0001F389 ঔষধের কোর্স "
         "শেষ হয়েছে\n\n";
    m += who + " — " + med;
  }
  else if (e == "UNKNOWN_FINGER" || e == "WRONG_USER")
  {
    unsigned long nowMs = millis();

    if (tgBadFingerCount == 0 ||
        nowMs - tgBadFingerFirst > TG_BAD_FINGER_WINDOW_MS)
    {
      tgBadFingerFirst = nowMs;
      tgBadFingerCount = 0;
    }

    tgBadFingerCount++;

    if (tgBadFingerCount < TG_BAD_FINGER_LIMIT) return;

    tgBadFingerCount = 0;

    m  = "⚠️ বারবার ভুল "
         "আঙুলের ছাপ\n\n";
    m += String(TG_BAD_FINGER_LIMIT) +
         " বার চেষ্টা হয়েছে।";
  }

  if (m.length() > 0) tgQueueMessage(m);
}

// Ask Telegram who has messaged the bot, and adopt that chat ID. This
// is what saves the family from having to find a numeric ID by hand:
// they message the bot, press the button, and the device fills it in.
String tgLinkChat()
{
  if (tgToken.length() < 20) return "";
  if (!tgOnline()) return "";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10);              // seconds
  client.setHandshakeTimeout(10);    // seconds

  HTTPClient https;
  // No offset on purpose: passing one tells Telegram those updates
  // have been dealt with and it throws them away.
  String url = "https://api.telegram.org/bot" + tgToken +
               "/getUpdates?limit=20";

  if (!https.begin(client, url)) return "";

  https.setTimeout(10000);           // milliseconds
  int code = https.GET();

  if (code != 200) { https.end(); return ""; }

  String body = https.getString();
  https.end();

  // Take the LAST chat id in the response - whoever messaged most
  // recently is the person standing at the dashboard right now.
  String found = "";
  int at = 0;

  while (true)
  {
    int k = body.indexOf("\"chat\":{\"id\":", at);
    if (k < 0) break;

    k += 13;
    int end = k;
    if (end < (int)body.length() && body[end] == '-') end++;
    while (end < (int)body.length() && isDigit(body[end])) end++;

    found = body.substring(k, end);
    at = end;
  }

  return found;
}

// Carry out a chat-ID lookup the dashboard asked for. Called from
// loop(), where blocking for a few seconds is safe.
void serviceTelegramLink()
{
  if (!tgLinkWanted) return;

  // Same rule as sending: the reminder always comes first.
  if (reminderState != REMINDER_IDLE) return;

  tgLinkWanted = false;
  tgLinkState  = 1;

  if (!tgOnline())
  {
    tgLinkFinish(3, "ইন্টারনেট নেই। আগে হোম ওয়াই-ফাই যুক্ত করুন।");
    return;
  }

  esp_task_wdt_reset();
  String id = tgLinkChat();
  esp_task_wdt_reset();

  if (id.length() == 0)
  {
    tgLinkFinish(3, "কোনো মেসেজ পাওয়া যায়নি। আগে বটকে /start লিখে পাঠান।");
    return;
  }

  tgLinkFinish(2, id.c_str());

  Serial.print("[TG] chat id found: ");
  Serial.println(id);
}

// Drop the message at the head of the queue and start the next one
// from the first recipient again.
void tgPopMessage()
{
  if (tgQueueCount == 0) return;

  for (uint8_t i = 1; i < tgQueueCount; i++) tgQueue[i - 1] = tgQueue[i];

  tgQueue[tgQueueCount - 1] = "";
  tgQueueCount--;
  tgSendIdx = 0;
  tgFailStreak = 0;
}

// Drain the queue, and send the daily summary when its hour arrives.
void serviceTelegram()
{
  if (!tgConfigured()) return;

  // Never run a TLS handshake while a dose is in progress. It blocks
  // for seconds at a time and the reminder comes first, always.
  if (reminderState != REMINDER_IDLE) return;

  // --- end-of-day summary ---------------------------------------
  if (rtcOK)
  {
    DateTime now = rtc.now();
    uint32_t stamp = (uint32_t)now.year() * 10000 +
                     (uint32_t)now.month() * 100 +
                     (uint32_t)now.day();

    uint16_t nowMins    = now.hour() * 60 + now.minute();
    uint16_t targetMins = tgSummaryHour * 60 + tgSummaryMinute;

    if (nowMins >= targetMins && tgSummarySent != stamp)
    {
      tgSummarySent = stamp;

      preferences.begin("smrtg", false);
      preferences.putUInt("sumd", stamp);
      preferences.end();

      tgQueueMessage(tgBuildSummary(now));
      Serial.println("[TG] daily summary queued");
    }
  }

  if (tgQueueCount == 0)
  {
    // Nothing left to send - let the radio go back down early rather
    // than burning the rest of the window.
    if (tgRadioUntil != 0) tgRadioUntil = 0;
    return;
  }

  if (!tgOnline()) return;

  // Back off after failures instead of hammering a dead link.
  unsigned long wait = tgFailStreak == 0 ? 1000UL : 15000UL;
  if (millis() - tgLastAttempt < wait) return;
  tgLastAttempt = millis();

  uint8_t who = tgNextRecipient(tgSendIdx);

  if (who >= TG_MAX_RECIPIENTS)
  {
    // Everybody has had this one.
    tgPopMessage();
    return;
  }

  // Feed the watchdog on both sides of the handshake. A slow TLS
  // negotiation is the longest single thing this firmware ever does.
  esp_task_wdt_reset();
  bool ok = tgPost(tgChat[who], tgQueue[0]);
  esp_task_wdt_reset();

  if (ok)
  {
    tgFailStreak = 0;
    tgSendIdx = who + 1;

    if (tgNextRecipient(tgSendIdx) >= TG_MAX_RECIPIENTS)
    {
      tgPopMessage();
      Serial.printf("[TG] sent, %d left\n", tgQueueCount);
    }

    return;
  }

  tgFailStreak++;

  // Give up on a message that will clearly never go. Holding the
  // radio up forever for it would flatten the battery.
  if (tgFailStreak >= 6)
  {
    tgPopMessage();
    Serial.println("[TG] giving up on that message");
    return;
  }

  // Still trying - keep the window open.
  tgRadioUntil = millis() + 30000UL;
}



// Set when the dashboard asks for a reboot. The actual restart happens
// from loop(), a moment later, so the reply reaches the browser first.
unsigned long restartRequestedAt = 0;

// ---- helpers -------------------------------------------------

bool pinOk(AsyncWebServerRequest *req)
{
  if (!req->hasParam("pin", true)) return false;
  return req->getParam("pin", true)->value() == String(adminPin);
}

String paramStr(
  AsyncWebServerRequest *req,
  const char *name,
  const String &fallback
)
{
  if (req->hasParam(name, true)) return req->getParam(name, true)->value();
  return fallback;
}

// ---- API -----------------------------------------------------

void apiStatus(AsyncWebServerRequest *req)
{
  String j = "{";

  DateTime now = rtc.now();
  char clk[24];
  formatTime12WithSeconds(
    now.hour(), now.minute(), now.second(), clk, sizeof(clk)
  );

  j += "\"clock\":\"" + String(clk) + "\",";
  j += "\"rtc\":"  + String(rtcOK ? "true" : "false") + ",";
  j += "\"fp\":"   + String(fingerprintOK ? "true" : "false") + ",";
  j += "\"mp3\":"  + String(mp3CardOk ? "true" : "false") + ",";

  // Reported separately so the dashboard can say WHICH half is
  // broken: the module not answering is wiring or power, the card
  // not reading is the socket or the card.
  j += "\"mp3mod\":" + String(mp3OK ? "true" : "false") + ",";
  j += "\"log\":"  + String(logStorageOK ? "true" : "false") + ",";
  j += "\"tpl\":"  + String(finger.templateCount) + ",";
  j += "\"ip\":\"" + WiFi.softAPIP().toString() + "\",";

  // The home network, so the family can see whether the box actually
  // has internet - without that, Telegram silently does nothing.
  bool sta = (WiFi.status() == WL_CONNECTED);
  j += "\"sta\":"     + String(sta ? "true" : "false") + ",";
  j += "\"staIp\":\"" + (sta ? WiFi.localIP().toString() : String("")) + "\",";
  j += "\"staSsid\":\"" + (sta ? WiFi.SSID() : String("")) + "\",";
  j += "\"tg\":"      + String(tgConfigured() ? "true" : "false") + ",";
  j += "\"tgQueued\":" + String(tgQueueCount) + ",";

  // Next dose
  int best = -1;
  uint16_t bestDelta = 0xFFFF;
  uint16_t nowMin = now.hour() * 60 + now.minute();

  for (int i = 0; i < NUM_SLOTS; i++)
  {
    if (!slots[i].enabled) continue;
    uint16_t sm = slots[i].hour * 60 + slots[i].minute;
    uint16_t d = (sm > nowMin) ? (sm - nowMin) : (uint16_t)(1440 - nowMin + sm);
    if (d < bestDelta) { bestDelta = d; best = i; }
  }

  if (best >= 0)
  {
    j += "\"nextTime\":\"" + bnTimeText(slots[best].hour, slots[best].minute) + "\",";
    j += "\"nextUser\":\"" + String(getUserName(slots[best].userId)) + "\",";
    j += "\"nextMed\":\""  + String(slots[best].medName) + "\",";
  }

  // Adherence counts, straight off the event log
  uint16_t nT = 0, nM = 0, nU = 0;
  if (logStorageOK)
  {
    File f = FFat.open(LOG_FILE, FILE_READ);
    if (f)
    {
      while (f.available())
      {
        String line = f.readStringUntil('\n');
        if (line.indexOf(",TAKEN,") >= 0) nT++;
        else if (line.indexOf(",MISSED") >= 0) nM++;
        else if (line.indexOf(",UNCONFIRMED,") >= 0) nU++;
      }
      f.close();
    }
  }

  j += "\"taken\":"  + String(nT) + ",";
  j += "\"missed\":" + String(nM) + ",";
  j += "\"unconf\":" + String(nU);
  j += "}";

  req->send(200, "application/json", j);
}

void apiSlots(AsyncWebServerRequest *req)
{
  String j = "[";
  for (int i = 0; i < NUM_SLOTS; i++)
  {
    if (i) j += ",";
    j += "{\"i\":" + String(i);
    j += ",\"h\":" + String(slots[i].hour);
    j += ",\"m\":" + String(slots[i].minute);
    j += ",\"c\":" + String(slots[i].compartment);
    j += ",\"on\":" + String(slots[i].enabled ? "true" : "false");
    j += ",\"user\":\"" + String(getUserName(slots[i].userId)) + "\"";
    j += ",\"med\":\"" + String(slots[i].medName) + "\"}";
  }
  j += "]";
  req->send(200, "application/json", j);
}

// The raw CSV goes to the browser and is parsed there. Building
// JSON here would mean holding the whole log in RAM.
void apiHistory(AsyncWebServerRequest *req)
{
  if (!logStorageOK)
  {
    req->send(200, "text/csv", "timestamp,event,user_id,user_name,slot,compartment,medicine,fingerprint_id\n");
    return;
  }
  req->send(FFat, LOG_FILE, "text/csv");
}

void apiSaveSlot(AsyncWebServerRequest *req)
{
  if (!pinOk(req)) { req->send(401, "text/plain", "ভুল পিন"); return; }

  int i = paramStr(req, "i", "-1").toInt();
  if (i < 0 || i >= NUM_SLOTS) { req->send(400, "text/plain", "bad slot"); return; }

  int h  = paramStr(req, "h",  String(slots[i].hour)).toInt();
  int m  = paramStr(req, "m",  String(slots[i].minute)).toInt();
  bool en = paramStr(req, "en", slots[i].enabled ? "1" : "0") == "1";

  if (h < 0 || h > 23 || m < 0 || m > 59)
  { req->send(400, "text/plain", "bad time"); return; }

  // Same 15-minute rule the buttons enforce. The dashboard must not
  // be a way around a safety rule.
  if (!scheduleGapValid(i, (uint8_t)h, (uint8_t)m, en))
  {
    req->send(409, "text/plain", "সময় খুব কাছাকাছি - অন্তত ১৫ মিনিট ফাঁক দিন");
    return;
  }

  slots[i].hour    = (uint8_t)h;
  slots[i].minute  = (uint8_t)m;
  slots[i].enabled = en;

  if (req->hasParam("med", true))
  {
    String med = req->getParam("med", true)->value();
    strncpy(slots[i].medName, med.c_str(), sizeof(slots[i].medName) - 1);
    slots[i].medName[sizeof(slots[i].medName) - 1] = 0;
  }

  saveConfiguration();
  armNextDoseAlarm();
  resetSchedulerTriggerMemory();

  logEvent("SCHEDULE_EDIT", i);

  // The screen must follow the change.
  epdLastSignature = "";
  epdIdle();

  req->send(200, "text/plain", "ok");
}

void apiSetTime(AsyncWebServerRequest *req)
{
  if (!pinOk(req)) { req->send(401, "text/plain", "ভুল পিন"); return; }
  if (!rtcOK)      { req->send(503, "text/plain", "no rtc"); return; }

  rtc.adjust(
    DateTime(
      (uint16_t)paramStr(req, "y",  "2026").toInt(),
      (uint8_t) paramStr(req, "mo", "1").toInt(),
      (uint8_t) paramStr(req, "d",  "1").toInt(),
      (uint8_t) paramStr(req, "h",  "0").toInt(),
      (uint8_t) paramStr(req, "mi", "0").toInt(),
      (uint8_t) paramStr(req, "s",  "0").toInt()
    )
  );

  armNextDoseAlarm();
  resetSchedulerTriggerMemory();

  epdLastSignature = "";
  epdIdle();

  req->send(200, "text/plain", "ok");
}

void apiSetWifi(AsyncWebServerRequest *req)
{
  if (!pinOk(req)) { req->send(401, "text/plain", "ভুল পিন"); return; }

  String ssid = paramStr(req, "ssid", "");
  String pass = paramStr(req, "pass", "");

  preferences.begin("smrnet", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.end();

  // Join straight away rather than at the next restart. The dashboard
  // polls /api/status, so the page shows whether it worked within a
  // few seconds - which is the only way to catch a typed password.
  if (ssid.length() > 0)
  {
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
  }

  req->send(200, "text/plain", "ok");
}


void apiTelegram(AsyncWebServerRequest *req)
{
  if (!pinOk(req)) { req->send(401, "text/plain", "ভুল পিন"); return; }

  tgEnabled  = paramStr(req, "en", "0") == "1";

  // The page shows the saved token masked and posts the field back
  // empty, so an empty value means "leave it alone" and NOT "erase
  // it". Without this, saving any other setting would wipe the token
  // and silently stop every alert.
  String newTok = paramStr(req, "token", "");
  newTok.trim();
  if (newTok.length() > 0) tgToken = newTok;

  tgChat[0]  = paramStr(req, "chat1", tgChat[0]);  tgChat[0].trim();
  tgChat[1]  = paramStr(req, "chat2", tgChat[1]);  tgChat[1].trim();
  tgLabel[0] = paramStr(req, "name1", tgLabel[0]);
  tgLabel[1] = paramStr(req, "name2", tgLabel[1]);

  int h = paramStr(req, "hour", String(tgSummaryHour)).toInt();
  if (h >= 0 && h <= 23) tgSummaryHour = (uint8_t)h;

  int mi = paramStr(req, "min", String(tgSummaryMinute)).toInt();
  if (mi >= 0 && mi <= 59) tgSummaryMinute = (uint8_t)mi;

  tgSave();

  req->send(200, "text/plain", "ok");
}

// Read back the current settings so the page can show them. The token
// is deliberately masked - it is a credential, and the dashboard is
// reachable by anyone on the Wi-Fi.
void apiTelegramGet(AsyncWebServerRequest *req)
{
  String j = "{";
  j += "\"en\":"     + String(tgEnabled ? "true" : "false") + ",";
  j += "\"hasTok\":" + String(tgToken.length() > 20 ? "true" : "false") + ",";
  // Build the masked token in a plain String first. Chaining a
  // ternary into Arduino's operator+ works, but it is the kind of
  // expression that resolves differently between core versions.
  String shown = "";
  if (tgToken.length() > 20) shown = tgToken.substring(0, 6) + String("...");

  j += "\"tok\":\"" + shown + "\",";
  j += "\"chat1\":\"" + tgChat[0]  + "\",";
  j += "\"chat2\":\"" + tgChat[1]  + "\",";
  j += "\"name1\":\"" + tgLabel[0] + "\",";
  j += "\"name2\":\"" + tgLabel[1] + "\",";
  j += "\"hour\":"    + String(tgSummaryHour) + ",";
  j += "\"min\":"     + String(tgSummaryMinute) + ",";
  j += "\"queued\":"  + String(tgQueueCount);
  j += "}";

  req->send(200, "application/json", j);
}

// Only raises the flag. The lookup itself happens in loop() - see
// serviceTelegramLink() for why it cannot happen here.
void apiTelegramLink(AsyncWebServerRequest *req)
{
  if (!pinOk(req)) { req->send(401, "text/plain", "ভুল পিন"); return; }

  tgLinkResult[0] = 0;
  tgLinkState  = 1;
  tgLinkWanted = true;

  req->send(200, "text/plain", "pending");
}

// The page polls this until the state stops being 1.
void apiTelegramLinkResult(AsyncWebServerRequest *req)
{
  String j = "{\"state\":" + String((int)tgLinkState) +
             ",\"value\":\"" + String(tgLinkResult) + "\"}";

  req->send(200, "application/json", j);
}

void apiTelegramTest(AsyncWebServerRequest *req)
{
  if (!pinOk(req)) { req->send(401, "text/plain", "ভুল পিন"); return; }

  if (!tgConfigured())
  {
    req->send(400, "text/plain",
      "টেলিগ্রাম চালু "
      "করা নেই।");
    return;
  }

  tgQueueMessage(
    "✅ ঔষধ বাক্স যুক্ত "
    "হয়েছে।\n\n"
    "এই মেসেজটি পেলে "
    "সব ঠিক আছে।");

  req->send(200, "text/plain", "ok");
}


// Ask for a fingerprint to be re-enrolled. Like the Telegram chat
// lookup, this only raises a flag - the work takes six finger presses
// and the better part of a minute, which the web server's task cannot
// sit through. The page polls the GET below.
void apiEnroll(AsyncWebServerRequest *req)
{
  if (!pinOk(req)) { req->send(401, "text/plain", "ভুল পিন"); return; }

  int u = paramStr(req, "user", "-1").toInt();

  if (u != 0 && u != 1 && u != (int)CARETAKER_USER)
  {
    req->send(400, "text/plain", "কার ছাপ?");
    return;
  }

  if (!fingerprintOK)
  {
    req->send(400, "text/plain",
      "ছাপ যন্ত্রে সমস্যা");
    return;
  }

  if (reminderState != REMINDER_IDLE)
  {
    req->send(409, "text/plain",
      "এখন ডোজ চলছে");
    return;
  }

  enrollWantUser = (uint8_t)u;
  enrollWanted   = true;

  req->send(200, "text/plain", "ok");
}

void apiEnrollStatus(AsyncWebServerRequest *req)
{
  String j = "{";
  j += "\"state\":"  + String((int)enrollState) + ",";
  j += "\"user\":"   + String((int)enrollUser)  + ",";
  j += "\"angle\":"  + String((int)enrollAngle) + ",";
  j += "\"press\":"  + String((int)enrollPress) + ",";
  j += "\"msg\":\""  + String(enrollMsg) + "\",";
  j += "\"tpl\":"    + String(finger.templateCount);
  j += "}";

  req->send(200, "application/json", j);
}

void apiEnrollCancel(AsyncWebServerRequest *req)
{
  if (!pinOk(req)) { req->send(401, "text/plain", "ভুল পিন"); return; }

  enrollWanted = false;
  enrollState  = ENR_IDLE;

  req->send(200, "text/plain", "ok");
}

// Who currently has a finger on file. Read-only, so no PIN - the same
// rule the rest of the status endpoints follow.
void apiUsers(AsyncWebServerRequest *req)
{
  String j = "[";

  for (uint8_t u = 0; u <= CARETAKER_USER; u++)
  {
    if (u) j += ",";

    j += "{\"id\":" + String((int)u);
    j += ",\"name\":\"" + String(getUserName(u)) + "\"";
    j += ",\"boss\":" + String(u == CARETAKER_USER ? "true" : "false");
    j += ",\"prints\":" + String((int)enrollCount[u]) + "}";
  }

  j += "]";

  req->send(200, "application/json", j);
}

void apiSetPin(AsyncWebServerRequest *req)
{
  // Changing the PIN requires the CURRENT PIN. Otherwise anyone on the
  // Wi-Fi could lock the family out of their own device.
  if (!pinOk(req)) { req->send(401, "text/plain", "ভুল পিন"); return; }

  String np = paramStr(req, "newpin", "");

  if (np.length() < 4 || np.length() > 6)
  {
    req->send(400, "text/plain", "পিন ৪ থেকে ৬ সংখ্যার হতে হবে");
    return;
  }

  for (unsigned i = 0; i < np.length(); i++)
  {
    if (np[i] < '0' || np[i] > '9')
    {
      req->send(400, "text/plain", "শুধু সংখ্যা ব্যবহার করুন");
      return;
    }
  }

  strncpy(adminPin, np.c_str(), sizeof(adminPin) - 1);
  adminPin[sizeof(adminPin) - 1] = 0;

  preferences.begin("smrsec", false);
  preferences.putString("pin", np);
  preferences.end();

  logEvent("PIN_CHANGED", -1);

  req->send(200, "text/plain", "ok");
}

// Lets the family restart the device from the dashboard, so nobody
// ever has to reach inside the enclosure for the RST button.
void apiRestart(AsyncWebServerRequest *req)
{
  if (!pinOk(req)) { req->send(401, "text/plain", "ভুল পিন"); return; }

  logEvent("RESTART_REQUEST", -1);
  req->send(200, "text/plain", "ok");

  restartRequestedAt = millis();
}

// ---- bring the radio up and down ------------------------------

void setupWebRoutes()
{
  if (routesReady) return;

  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *r){
    r->send_P(200, "text/html", INDEX_HTML);
  });

  webServer.on("/api/status",  HTTP_GET,  apiStatus);
  webServer.on("/api/slots",   HTTP_GET,  apiSlots);
  webServer.on("/api/history", HTTP_GET,  apiHistory);

  webServer.on("/api/slot", HTTP_POST, apiSaveSlot);
  webServer.on("/api/time", HTTP_POST, apiSetTime);
  webServer.on("/api/wifi", HTTP_POST, apiSetWifi);
  webServer.on("/api/telegram", HTTP_GET,  apiTelegramGet);
  webServer.on("/api/telegram", HTTP_POST, apiTelegram);
  // NOT "/api/telegram/..." - see the note above wifiStart(). A route
  // on "/api/telegram" would swallow anything nested under it.
  webServer.on("/api/tglink", HTTP_POST, apiTelegramLink);
  webServer.on("/api/tglink", HTTP_GET,  apiTelegramLinkResult);
  webServer.on("/api/tgtest", HTTP_POST, apiTelegramTest);
  webServer.on("/api/users",  HTTP_GET,  apiUsers);
  webServer.on("/api/enroll", HTTP_POST, apiEnroll);
  webServer.on("/api/enroll", HTTP_GET,  apiEnrollStatus);
  webServer.on("/api/enrollstop", HTTP_POST, apiEnrollCancel);
  webServer.on("/api/pin",  HTTP_POST, apiSetPin);
  webServer.on("/api/restart", HTTP_POST, apiRestart);

  // NOTE: there is no /api/unlock, and there never will be.

  // Captive portal. Phones test whether a network has internet by
  // fetching one of these URLs. Redirecting them to our page is what
  // makes the dashboard pop up by itself on joining the Wi-Fi.
  const char* probes[] = {
    "/generate_204",            // Android
    "/gen_204",                 // Android
    "/hotspot-detect.html",     // iOS / macOS
    "/library/test/success.html",
    "/connecttest.txt",         // Windows
    "/ncsi.txt",                // Windows
    "/fwlink",                  // Windows
    "/redirect"
  };

  for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++)
  {
    webServer.on(probes[i], HTTP_GET, [](AsyncWebServerRequest *r){
      r->redirect("http://192.168.4.1/");
    });
  }

  // Anything else also goes to the dashboard.
  webServer.onNotFound([](AsyncWebServerRequest *r){
    if (r->method() == HTTP_GET) r->redirect("http://192.168.4.1/");
    else r->send(404, "text/plain", "no");
  });

  routesReady = true;
}

// staOnly brings up the home-network side alone: no access point, no
// captive portal, no dashboard. That is all a Telegram alert needs,
// and it keeps the radio window as cheap as possible.
void wifiStart(bool staOnly)
{
  if (wifiUp) return;

  WiFi.persistent(false);

  String ssid, pass;
  preferences.begin("smrnet", true);
  ssid = preferences.getString("ssid", "");
  pass = preferences.getString("pass", "");
  preferences.end();

  // The access point exists whether or not the home network does.
  // It must never depend on STA succeeding.
  if (staOnly)
  {
    if (ssid.length() == 0)
    {
      // No home network saved, so there is nothing to connect to.
      // Close the window rather than retrying every pass for two
      // minutes - the message stays queued for whenever Wi-Fi is set
      // up, or for the next time the box is taken out of power saving.
      Serial.println("[NET] no saved Wi-Fi - cannot send alerts yet");
      tgRadioUntil = 0;
      return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    wifiUp      = true;
    wifiStaOnly = true;

    Serial.println("[NET] short radio window open (sending alert)");
    return;
  }

  if (ssid.length() > 0)
  {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    WiFi.begin(ssid.c_str(), pass.c_str());
  }
  else
  {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
  }

  delay(120);

  dnsServer.start(53, "*", WiFi.softAPIP());
  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);

  // Routes and listener are created once, on the first start, and
  // then left alone for the lifetime of the device.
  if (!serverBegun)
  {
    setupWebRoutes();
    webServer.begin();
    serverBegun = true;
  }

  wifiUp      = true;
  wifiStaOnly = false;

  Serial.println();
  Serial.println("========== DASHBOARD ONLINE ==========");
  Serial.print("Wi-Fi name : "); Serial.println(AP_SSID);
  Serial.print("Password   : "); Serial.println(AP_PASSWORD);
  Serial.print("Open       : http://");
  Serial.println(WiFi.softAPIP().toString());
  Serial.print("or         : http://"); Serial.print(MDNS_NAME); Serial.println(".local");
  Serial.println("======================================");
}

void wifiStop()
{
  if (!wifiUp) return;

  // NOTE: webServer.end() is deliberately NOT called. Stopping and
  // restarting an AsyncWebServer leaves its socket in a state it does
  // not recover from, which is what used to require a hardware reset.
  if (!wifiStaOnly)
  {
    dnsServer.stop();
    MDNS.end();
    WiFi.softAPdisconnect(true);
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  wifiUp      = false;
  wifiStaOnly = false;

  Serial.println("[NET] Wi-Fi off (power saving)");
}

// Carry out a reboot the dashboard asked for, once the reply has had
// time to reach the browser.
void serviceRestartRequest()
{
  if (restartRequestedAt == 0) return;
  if (millis() - restartRequestedAt < 800) return;

  Serial.println();
  Serial.println("[NET] restart requested from the dashboard");
  Serial.flush();
  delay(50);
  ESP.restart();
}

// Wi-Fi follows the power mode, with one exception: a queued Telegram
// alert opens a short window of its own, so a missed dose gets
// reported even while the box is asleep.
void serviceWifi()
{
  bool wantFull  = !powerSaveEnabled;
  bool wantAlert = (tgRadioUntil != 0) && ((long)(millis() - tgRadioUntil) < 0);

  if (tgRadioUntil != 0 && !wantAlert)
  {
    tgRadioUntil = 0;

    if (tgQueueCount > 0)
      Serial.println("[TG] radio window expired with messages still waiting");
  }

  if (wantFull)
  {
    // Coming out of power saving while a bare STA link is up: tear it
    // down first, because the access point and the dashboard cannot be
    // added to an already-running STA-only session.
    if (wifiUp && wifiStaOnly) wifiStop();
    if (!wifiUp) wifiStart(false);
  }
  else if (wantAlert)
  {
    if (!wifiUp) wifiStart(true);
  }
  else if (wifiUp)
  {
    wifiStop();
  }

  if (wifiUp && !wifiStaOnly) dnsServer.processNextRequest();
}

// =====================================================
// 16. LOG STORAGE INITIALIZATION
// =====================================================

void setupEventLog()
{
  Serial.print(
    "[LOG] Mounting FFat... "
  );

  // true = format on mount failure.
  // Useful on first use of this FATFS partition.
  if (!FFat.begin(true))
  {
    Serial.println("FAILED");

    logStorageOK = false;

    // Say WHY, instead of leaving a bare FAILED on screen.
    printPartitionTable();

    return;
  }

  logStorageOK = true;

  Serial.println("OK");
  // NOTE: FFat.totalBytes()/usedBytes() return size_t, which is
  // 32-bit on the ESP32. Printing them with %llu made printf read
  // 8 bytes from a 4-byte value - that is why this reported a
  // nonsensical ~43 GB on a 9.9 MB partition. Cast explicitly.

  Serial.printf(
    "      Total: %lu bytes (%.1f MB)\n",
    (unsigned long)FFat.totalBytes(),
    (double)FFat.totalBytes() / 1048576.0
  );

  Serial.printf(
    "      Used : %lu bytes\n",
    (unsigned long)FFat.usedBytes()
  );

  // The FATFS partition is about 9.9 MB. Anything wildly different
  // means the filesystem geometry is corrupt - it will still mount,
  // but writes then fail in confusing ways. Say so loudly.
  if (
    FFat.totalBytes() > 64ULL * 1024ULL * 1024ULL
    ||
    FFat.totalBytes() < 1ULL * 1024ULL * 1024ULL
  )
  {
    Serial.println();
    Serial.println(
      "*** FFat GEOMETRY LOOKS WRONG ***"
    );
    Serial.println(
      "    This partition should be about 9.9 MB."
    );
    Serial.println(
      "    The filesystem is corrupt - press 'W' to reformat."
    );
    Serial.println();
  }



  if (!FFat.exists(LOG_FILE))
  {
    File file =
      FFat.open(
        LOG_FILE,
        FILE_WRITE
      );

    if (!file)
    {
      Serial.println(
        "[LOG] Could not create events.csv"
      );

      logStorageOK = false;

      return;
    }

    file.println(
      "timestamp,event,user_id,user_name,slot,compartment,medicine,fingerprint_id"
    );

    file.flush();
    file.close();

    Serial.println(
      "[LOG] New events.csv created."
    );
  }

  else
  {
    Serial.println(
      "[LOG] Existing events.csv found."
    );
  }
}


// =====================================================
// 17. LOG FILE ROTATION
// =====================================================

void rotateLogIfNeeded()
{
  if (!logStorageOK)
  {
    return;
  }

  File file =
    FFat.open(
      LOG_FILE,
      FILE_READ
    );

  if (!file)
  {
    return;
  }

  size_t fileSize =
    file.size();

  file.close();


  if (
    fileSize <
    MAX_LOG_FILE_BYTES
  )
  {
    return;
  }


  Serial.println(
    "[LOG] Maximum log size reached."
  );

  Serial.println(
    "[LOG] Rotating log file..."
  );


  if (
    FFat.exists(
      LOG_OLD_FILE
    )
  )
  {
    FFat.remove(
      LOG_OLD_FILE
    );
  }


  FFat.rename(
    LOG_FILE,
    LOG_OLD_FILE
  );


  File newFile =
    FFat.open(
      LOG_FILE,
      FILE_WRITE
    );

  if (!newFile)
  {
    Serial.println(
      "[LOG] Rotation failed."
    );

    return;
  }


  newFile.println(
    "timestamp,event,user_id,user_name,slot,compartment,medicine,fingerprint_id"
  );

  newFile.flush();
  newFile.close();


  Serial.println(
    "[LOG] New events.csv created."
  );
}


// =====================================================
// 18. WRITE EVENT LOG
// =====================================================

void logEvent(
  const char* eventName,
  int slotIndex,
  int fingerprintID
)
{
  if (!logStorageOK)
  {
    Serial.printf(
      "[LOG SKIPPED] %s\n",
      eventName
    );

    return;
  }


  rotateLogIfNeeded();


  char timestamp[24];


  if (rtcOK)
  {
    DateTime now =
      rtc.now();

    snprintf(
      timestamp,
      sizeof(timestamp),
      "%04d-%02d-%02d %02d:%02d:%02d",
      now.year(),
      now.month(),
      now.day(),
      now.hour(),
      now.minute(),
      now.second()
    );
  }

  else
  {
    snprintf(
      timestamp,
      sizeof(timestamp),
      "NO_RTC"
    );
  }


  int userId = -1;
  int slotNumber = 0;
  int compartment = 0;

  const char* userName =
    "-";

  const char* medicine =
    "-";


  if (
    slotIndex >= 0
    &&
    slotIndex < NUM_SLOTS
  )
  {
    DoseSlot &slot =
      slots[slotIndex];

    userId =
      slot.userId;

    slotNumber =
      slotIndex + 1;

    compartment =
      slot.compartment;

    userName =
      getUserName(
        slot.userId
      );

    medicine =
      slot.medName;
  }


  File file =
    FFat.open(
      LOG_FILE,
      FILE_APPEND
    );


  if (!file)
  {
    Serial.println(
      "[LOG] Could not open events.csv"
    );

    return;
  }


  file.printf(
    "%s,%s,%d,%s,%d,%d,%s,%d\n",
    timestamp,
    eventName,
    userId,
    userName,
    slotNumber,
    compartment,
    medicine,
    fingerprintID
  );


  file.flush();
  file.close();


  Serial.printf(
    "[LOG] %s",
    eventName
  );


  if (
    slotIndex >= 0
    &&
    slotIndex < NUM_SLOTS
  )
  {
    Serial.printf(
      " | Slot %d | %s | C%d",
      slotNumber,
      userName,
      compartment
    );
  }


  if (
    fingerprintID >= 0
  )
  {
    Serial.printf(
      " | FP ID=%d",
      fingerprintID
    );
  }


  Serial.println();


  // Every recorded event passes through here, so the alert decision
  // lives in one place instead of being sprinkled over the state
  // machine. tgOnEvent only queues - it never sends from this call.
  tgOnEvent(
    eventName,
    slotIndex
  );
}


// =====================================================
// 19. PRINT EVENT LOG
// =====================================================

void printEventLog()
{
  Serial.println();

  Serial.println(
    "================ EVENT LOG ================"
  );


  if (!logStorageOK)
  {
    Serial.println(
      "FFat / log storage unavailable."
    );

    Serial.println(
      "==========================================="
    );

    return;
  }


  File file =
    FFat.open(
      LOG_FILE,
      FILE_READ
    );


  if (!file)
  {
    Serial.println(
      "Could not open events.csv"
    );

    Serial.println(
      "==========================================="
    );

    return;
  }


  while (
    file.available()
  )
  {
    Serial.write(
      file.read()
    );
  }


  file.close();


  Serial.println(
    "==========================================="
  );
}


// =====================================================
// SETUP
// =====================================================

void setup()
{
  // ---- FIRST. Before the serial port, before any delay. ----
  //
  // GPIO 39-42 drive the solenoids AND are the chip's JTAG pins. Until
  // they are outputs held LOW, nothing but the board's 10k pulldowns
  // is keeping the compartments shut - and after a fresh flash the ROM
  // does not leave them the way a plain reset does. Anything placed
  // above this line puts that window back.
  pinMode(LOCK_C1, OUTPUT); digitalWrite(LOCK_C1, LOW);
  pinMode(LOCK_C2, OUTPUT); digitalWrite(LOCK_C2, LOW);
  pinMode(LOCK_C3, OUTPUT); digitalWrite(LOCK_C3, LOW);
  pinMode(LOCK_C4, OUTPUT); digitalWrite(LOCK_C4, LOW);

  Serial.begin(115200);

  delay(2000);


  Serial.println();

  Serial.println(
    "========================================"
  );

  Serial.println(
    " SMART MEDICINE REMINDER"
  );

  Serial.println(
    " INTEGRATION-9  (Bangla UI)"
  );

  Serial.println(
    " PERSISTENT EVENT LOGGING"
  );

  Serial.println(
    "========================================"
  );


  setupLocks();

  setupButtons();

  setupBuzzer();

  setupRTC();

  setupEPaper();

  setupMP3();

  setupFingerprint();


  configOK =
    loadOrCreateConfiguration();


  setupReedSensors();
  loadAdminPin();
  refreshEnrollCounts();
  tgLoad();
  setupRtcAlarm();

  // Restore whether power saving was left switched on.
  preferences.begin("smrpwr", true);
  powerSaveEnabled = preferences.getBool("save", false);
  preferences.end();

  setupEventLog();

  // Anything we slept through while powered off is recorded,
  // not silently lost.
  loadLastProcessedEpoch();
  catchUpMissedDoses();


  // Persistent boot record
  logEvent(
    "SYSTEM_BOOT",
    -1
  );


  printConfiguration();

  printSystemStatus();


  Serial.println();

  // Watchdog. 30 s is generous - an e-paper refresh takes about
  // 2 s and an FFat write far less - but it will recover the
  // device if the fingerprint sensor or MP3 module ever stalls.
  // The Arduino core already starts the task watchdog for loopTask,
  // so calling esp_task_wdt_init() again just logs
  // "TWDT already initialized". Reconfigure the timeout instead,
  // and only subscribe if we are not already subscribed.
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = 30000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };

  if (esp_task_wdt_reconfigure(&wdtConfig) != ESP_OK)
  {
    esp_task_wdt_init(&wdtConfig);
  }

  if (esp_task_wdt_status(NULL) != ESP_OK)
  {
    esp_task_wdt_add(NULL);
  }

  Serial.println("[WDT] watchdog armed (30 s)");

  Serial.println(
    "INTEGRATION-9 READY"
  );

  // Arm the hardware wake alarm for the first dose.
  armNextDoseAlarm();

  Serial.print(
    "[PWR] power saving: "
  );
  Serial.println(
    powerSaveEnabled ? "ON" : "OFF  (press S to enable)"
  );

  // Leave the screen on the next-dose view. E-paper holds an image
  // with no power at all, so this stays readable while idle.
  epdIdle();

  showMenu();


  if (
    rtcOK
    &&
    configOK
  )
  {
    printNextDose();
  }


  Serial.println();

  Serial.println(
    "Debug commands:"
  );

  Serial.println(
    "P = Print configuration"
  );

  Serial.println(
    "N = Print next dose"
  );

  Serial.println(
    "L = Print persistent event log"
  );

  Serial.println(
    "R = Reload NVS"
  );

  Serial.println(
    "D = Restore default configuration"
  );

  Serial.println(
    "1-9,0 = Play a voice track (0 = track 10)"
  );

  Serial.println(
    "E = Redraw e-paper idle screen"
  );

  Serial.println(
    "F = Toggle SD layout (card root <-> /MP3/ folder)"
  );

  Serial.println(
    "C = Check whether the DFPlayer can read the SD card"
  );

  Serial.println(
    "W = Wipe/reformat the event log filesystem"
  );

  Serial.println(
    "S = Battery power saving on/off (hold BACK 3s to exit)"
  );

  Serial.println();
}


// =====================================================
// LOOP
// =====================================================

void loop()
{
  // Feed the watchdog. If the main loop ever stops running,
  // the device reboots instead of sitting there dead.
  esp_task_wdt_reset();

  // Power-saver housekeeping: let a long BACK press bring the
  // device back to full mode, and keep the clock face current.
  serviceEnterPowerSave();
  serviceExitPowerSave();
  serviceClockFace();

  // Wi-Fi simply follows the power mode - nothing else decides it.
  serviceWifi();
  serviceRestartRequest();
  serviceMp3Recovery();
  serviceTelegram();
  serviceTelegramLink();

  // Any button press keeps the device awake for a while, so the
  // menu does not doze off between key presses.
  for (int b = 0; b < 4; b++)
  {
    if (digitalRead(buttonPins[b]) == LOW) { noteInputActivity(); break; }
  }

  ButtonEvent event =
    readButtonEvent();


  if (
    event !=
    BUTTON_NONE
  )
  {
    if (
      reminderState ==
      REMINDER_IDLE
    )
    {
      // Browsing today's doses owns UP/DOWN/SELECT/BACK on the
      // idle screen in full mode. It says so itself when it has
      // taken the press.
      if (!handleBrowseButton(event))
      {
        handleMenuButton(
          event
        );
      }
    }


    else if (
      reminderState ==
      REMINDER_WAIT_CONFIRMATION
      &&
      event ==
      BUTTON_SELECT
    )
    {
      confirmDoseTaken();
    }


    else if (
      event !=
      BUTTON_BACK
    )
    {
      Serial.println(
        "[REMINDER] Menu temporarily locked."
      );
    }
  }


  serviceForceSelect();
  serviceEnroll();

  serviceBackHoldCancel();

  checkScheduler();

  serviceReminderStateMachine();

  printRTCTimePeriodically();

  checkDebugSerial();


  delay(10);

  // Last thing in the loop: doze if nothing is happening.
  serviceLightSleep();
}


// =====================================================
// LOCK INITIALIZATION
// =====================================================

void setupLocks()
{
  Serial.print(
    "[LOCK] Initializing... "
  );

  // setup() already did this as its very first action. Repeating it
  // here is deliberate: this function stays correct on its own, and
  // costs nothing.


  pinMode(
    LOCK_C1,
    OUTPUT
  );

  pinMode(
    LOCK_C2,
    OUTPUT
  );

  pinMode(
    LOCK_C3,
    OUTPUT
  );

  pinMode(
    LOCK_C4,
    OUTPUT
  );


  forceAllLocksLow();


  Serial.println("OK");

  Serial.println(
    "       C1/C2/C3/C4 forced LOW"
  );
}


// =====================================================
// FORCE ALL LOCKS LOW
// =====================================================

void forceAllLocksLow()
{
  digitalWrite(
    LOCK_C1,
    LOW
  );

  digitalWrite(
    LOCK_C2,
    LOW
  );

  digitalWrite(
    LOCK_C3,
    LOW
  );

  digitalWrite(
    LOCK_C4,
    LOW
  );
}


// =====================================================
// COMPARTMENT -> LOCK GPIO
// =====================================================

int getLockPinForCompartment(
  uint8_t compartment
)
{
  switch (compartment)
  {
    case 1:
      return LOCK_C1;

    case 2:
      return LOCK_C2;

    case 3:
      return LOCK_C3;

    case 4:
      return LOCK_C4;

    default:
      return -1;
  }
}


// =====================================================
// BUTTON INITIALIZATION
// =====================================================

void setupButtons()
{
  Serial.print(
    "[BUTTON] Initializing... "
  );


  for (
    int i = 0;
    i < 4;
    i++
  )
  {
    pinMode(
      buttonPins[i],
      INPUT_PULLUP
    );


    bool state =
      digitalRead(
        buttonPins[i]
      );


    lastRawState[i] =
      state;

    stableState[i] =
      state;

    stateChangedAt[i] =
      millis();
  }


  Serial.println("OK");
}


// =====================================================
// BUZZER
// =====================================================

void setupBuzzer()
{
  Serial.print(
    "[BUZZER] Initializing... "
  );


  ledcAttach(
    BUZZER_PIN,
    2000,
    8
  );


  ledcWrite(
    BUZZER_PIN,
    0
  );


  Serial.println("OK");
}


// =====================================================
// RTC
// =====================================================

void setupRTC()
{
  Serial.print(
    "[RTC] Initializing... "
  );


  Wire.begin(
    I2C_SDA,
    I2C_SCL
  );


  if (
    !rtc.begin()
  )
  {
    Serial.println(
      "FAILED"
    );

    rtcOK =
      false;

    return;
  }


  rtcOK =
    true;


  Serial.println("OK");


  if (
    rtc.lostPower()
  )
  {
    Serial.println(
      "       WARNING: RTC lost power"
    );
  }


  DateTime now =
    rtc.now();


  char timeText[20];


  formatTime12WithSeconds(
    now.hour(),
    now.minute(),
    now.second(),
    timeText,
    sizeof(timeText)
  );


  Serial.printf(
    "       Time: %04d-%02d-%02d %s\n",
    now.year(),
    now.month(),
    now.day(),
    timeText
  );
}


// =====================================================
// E-PAPER
// =====================================================

void setupEPaper()
{
  Serial.print(
    "[E-PAPER] Initializing... "
  );


  SPI.begin(
    EPD_SCK,
    -1,
    EPD_MOSI,
    EPD_CS
  );


  // 2 MHz, not the library's default 10 MHz.
  //
  // The panel is wired with jumper leads, and at 10 MHz a slightly
  // loose or long MOSI/SCK/DC lead corrupts the image data while the
  // panel still reports a clean refresh - which looks exactly like
  // random noise on screen. Slower clocking costs a few milliseconds
  // per frame and buys a lot of margin.
  display.epd2.selectSPI(
    SPI,
    SPISettings(EPD_SPI_HZ, MSBFIRST, SPI_MODE0)
  );


  // 50 ms reset pulse rather than the default 10 ms, for the same
  // reason: give the panel time to come up cleanly.
  display.init(115200, true, 50, false);


  display.setRotation(1);

  display.setFullWindow();

  display.setTextColor(
    GxEPD_BLACK
  );


  // A loose BUSY wire is the usual reason an e-paper shows torn or
  // half-drawn frames: the driver stops waiting for a refresh to
  // finish and starts the next one on top of it.
  //
  // This panel is GxEPD2_154_D67, built with busy_level = HIGH, so
  // BUSY is HIGH while busy and LOW when idle. LOW here is healthy.
  // Stuck HIGH means the driver will wait for a refresh that never
  // reports finished.
  pinMode(EPD_BUSY, INPUT);

  bool busyIdle = (digitalRead(EPD_BUSY) == LOW);

  Serial.printf(
    "\n[E-PAPER] BUSY (GPIO%d) reads %s while idle%s\n",
    EPD_BUSY,
    busyIdle ? "LOW - normal" : "HIGH",
    busyIdle ? "" : "  <-- stuck busy, check this wire"
  );


  // The Bangla boot screen. Nothing English is ever shown to the
  // user - code identifiers stay English, pixels do not.
  bangla.setLineHeight(EPD_LINE_H);
  epdBootScreen();


  Serial.println("OK");
}


// =====================================================
// MP3
// =====================================================

void setupMP3()
{
  Serial.print(
    "[MP3] Initializing... "
  );


  mp3Serial.begin(
    9600,
    SERIAL_8N1,
    MP3_RX,
    MP3_TX
  );


  delay(1500);


  if (
    !player.begin(
      mp3Serial,
      false,
      true
    )
  )
  {
    Serial.println(
      "FAILED"
    );

    mp3OK =
      false;

    return;
  }


  delay(1500);


  // Restore which card layout worked last time.
  preferences.begin("smrmp3", true);
  mp3Volume = preferences.getUChar("vol", MP3_VOLUME_DEFAULT);
  if (mp3Volume > MP3_VOLUME_MAX) mp3Volume = MP3_VOLUME_DEFAULT;
  mp3UseFolder = preferences.getBool("useFolder", false);
  preferences.end();

  player.volume(mp3Volume);
  delay(200);

  Serial.print(
    "       Volume: "
  );
  Serial.print(mp3Volume);
  Serial.print(
    "/30   layout: "
  );
  Serial.println(
    mp3UseFolder ? "/MP3/ folder" : "card root"
  );

  mp3OK =
    true;


  Serial.println("OK");


  int count =
    -1;


  for (
    int i = 0;
    i < 3;
    i++
  )
  {
    count =
      player.readFileCounts();


    if (
      count >= 0
    )
    {
      break;
    }


    delay(700);
  }


  Serial.printf(
    "       Files detected: %d\n",
    count
  );

  // The seven tracks added for enrolling a finger and moving a
  // dose are easy to forget when copying the card, and a missing
  // one is otherwise invisible until a prompt fails to speak.
  if (count > 0 && count < TOTAL_TRACKS)
  {
    Serial.printf(
      "       NOTE: %d track(s) missing - copy audio/MP3/ to the card.\n",
      TOTAL_TRACKS - count
    );
  }

  mp3CardOk    = (count > 0);
  mp3FileCount = count;

  if (!mp3CardOk)
  {
    Serial.println(
      "       card not seen yet - will keep retrying in background"
    );
  }
}


// -----------------------------------------------------
// Background SD-card recovery
//
// The card has been seen working on one boot and missing on the
// next with identical firmware, which points at a marginal socket
// rather than software. Deciding once at boot that audio is dead
// is therefore wrong - keep trying.
//
// Runs only while the card is missing, once every 30 s, and never
// during a dose: a module reset takes ~3 s and must not collide
// with a reminder that is trying to speak.
// -----------------------------------------------------
void serviceMp3Recovery()
{
  if (!mp3OK) return;
  if (mp3CardOk) return;
  if (reminderState != REMINDER_IDLE) return;

  if (millis() - lastCardProbe < CARD_PROBE_INTERVAL_MS) return;
  lastCardProbe = millis();

  int count = player.readFileCounts();

  if (count > 0)
  {
    mp3CardOk    = true;
    mp3FileCount = count;
    player.volume(mp3Volume);

    Serial.println();
    Serial.print("[AUDIO] SD card appeared - ");
    Serial.print(count);
    Serial.println(" files. Voice is working again.");

    logEvent("AUDIO_RECOVERED", -1);
    return;
  }

  // Every third failed probe, reset the module. That forces it to
  // re-mount the card, which is what actually recovers a marginal
  // socket - simply asking again never will.
  cardRecoveryAttempts++;

  if (cardRecoveryAttempts >= 3)
  {
    cardRecoveryAttempts = 0;
    Serial.println("[AUDIO] resetting the DFPlayer to retry the card...");
    player.begin(mp3Serial, false, true);
    delay(1200);
    player.volume(mp3Volume);
  }
}


// =====================================================
// FINGERPRINT
// =====================================================

void setupFingerprint()
{
  Serial.print(
    "[R307S] Initializing... "
  );


  fpSerial.begin(
    57600,
    SERIAL_8N1,
    FP_RX,
    FP_TX
  );


  delay(1000);


  if (
    !finger.verifyPassword()
  )
  {
    Serial.println(
      "FAILED"
    );

    fingerprintOK =
      false;

    return;
  }


  fingerprintOK =
    true;


  finger.getTemplateCount();


  Serial.println("OK");


  Serial.printf(
    "       Stored templates: %d\n",
    finger.templateCount
  );
}


// =====================================================
// SLOT CONFIG
// =====================================================

void configureSlot(
  int index,
  uint8_t userId,
  uint8_t hour,
  uint8_t minute,
  uint8_t compartment,
  const char* medicine,
  uint32_t startEpoch,
  uint16_t courseDays,
  bool enabled
)
{
  if (
    index < 0
    ||
    index >= NUM_SLOTS
  )
  {
    return;
  }


  slots[index].userId =
    userId;

  slots[index].hour =
    hour;

  slots[index].minute =
    minute;

  slots[index].compartment =
    compartment;


  snprintf(
    slots[index].medName,
    sizeof(
      slots[index].medName
    ),
    "%s",
    medicine
  );


  slots[index].startEpoch =
    startEpoch;

  slots[index].courseDays =
    courseDays;

  slots[index].enabled =
    enabled;
}


// =====================================================
// DEFAULT CONFIGURATION
// =====================================================

void loadDefaultConfiguration()
{
  memset(
    users,
    0,
    sizeof(users)
  );


  memset(
    slots,
    0,
    sizeof(slots)
  );


  users[0].userId =
    0;

  snprintf(
    users[0].name,
    sizeof(
      users[0].name
    ),
    "%s",
    "দাদু"
  );

  users[0].reminderTrack =
    1;


  users[1].userId =
    1;

  snprintf(
    users[1].name,
    sizeof(
      users[1].name
    ),
    "%s",
    "ঠাকুমা"
  );

  users[1].reminderTrack =
    2;


  configureSlot(
    0,
    0,
    9,
    0,
    1,
    "ঔষধ-১",
    0,
    0,
    true
  );


  configureSlot(
    1,
    0,
    20,
    30,
    2,
    "ঔষধ-২",
    0,
    0,
    true
  );


  configureSlot(
    2,
    1,
    9,
    30,
    3,
    "ঔষধ-৩",
    0,
    0,
    true
  );


  configureSlot(
    3,
    1,
    21,
    0,
    4,
    "ঔষধ-৪",
    0,
    0,
    true
  );
}


// =====================================================
// NVS SAVE
// =====================================================

bool saveConfiguration()
{
  Serial.print(
    "[NVS] Saving configuration... "
  );


  if (
    !preferences.begin(
      "medcfg",
      false
    )
  )
  {
    Serial.println(
      "FAILED"
    );

    return false;
  }


  preferences.putUInt(
    "version",
    CONFIG_VERSION
  );


  size_t usersWritten =
    preferences.putBytes(
      "users",
      users,
      sizeof(users)
    );


  size_t slotsWritten =
    preferences.putBytes(
      "slots",
      slots,
      sizeof(slots)
    );


  preferences.end();


  if (
    usersWritten !=
    sizeof(users)
    ||
    slotsWritten !=
    sizeof(slots)
  )
  {
    Serial.println(
      "FAILED"
    );

    return false;
  }


  Serial.println("OK");

  return true;
}


// =====================================================
// NVS LOAD
// =====================================================

bool loadConfiguration()
{
  if (
    !preferences.begin(
      "medcfg",
      true
    )
  )
  {
    return false;
  }


  uint32_t version =
    preferences.getUInt(
      "version",
      0
    );


  size_t userSize =
    preferences.getBytesLength(
      "users"
    );


  size_t slotSize =
    preferences.getBytesLength(
      "slots"
    );


  if (
    version !=
    CONFIG_VERSION
    ||
    userSize !=
    sizeof(users)
    ||
    slotSize !=
    sizeof(slots)
  )
  {
    preferences.end();

    return false;
  }


  size_t usersRead =
    preferences.getBytes(
      "users",
      users,
      sizeof(users)
    );


  size_t slotsRead =
    preferences.getBytes(
      "slots",
      slots,
      sizeof(slots)
    );


  preferences.end();


  if (
    usersRead !=
    sizeof(users)
    ||
    slotsRead !=
    sizeof(slots)
  )
  {
    return false;
  }


  return
    configurationLooksValid();
}


// =====================================================
// LOAD / CREATE CONFIG
// =====================================================

bool loadOrCreateConfiguration()
{
  Serial.println();

  Serial.println(
    "========== CONFIGURATION =========="
  );


  Serial.print(
    "[NVS] Looking for saved configuration... "
  );


  if (
    loadConfiguration()
  )
  {
    Serial.println(
      "FOUND"
    );


    Serial.println(
      "[NVS] Valid configuration loaded."
    );


    return true;
  }


  Serial.println(
    "NOT FOUND"
  );


  loadDefaultConfiguration();


  if (
    !saveConfiguration()
  )
  {
    return false;
  }


  Serial.println(
    "[NVS] Defaults created."
  );


  return true;
}


// =====================================================
// CONFIG VALIDATION
// =====================================================

bool configurationLooksValid()
{
  for (
    int i = 0;
    i < NUM_USERS;
    i++
  )
  {
    if (
      users[i].userId >=
      NUM_USERS
    )
    {
      return false;
    }


    if (
      users[i].name[0]
      ==
      '\0'
    )
    {
      return false;
    }
  }


  for (
    int i = 0;
    i < NUM_SLOTS;
    i++
  )
  {
    if (
      slots[i].userId >=
      NUM_USERS
    )
    {
      return false;
    }


    if (
      slots[i].hour >
      23
    )
    {
      return false;
    }


    if (
      slots[i].minute >
      59
    )
    {
      return false;
    }


    if (
      slots[i].compartment <
      1
      ||
      slots[i].compartment >
      4
    )
    {
      return false;
    }
  }


  return true;
}


// =====================================================
// RESET SCHEDULER MEMORY
// =====================================================

void resetSchedulerTriggerMemory()
{
  for (
    int i = 0;
    i < NUM_SLOTS;
    i++
  )
  {
    lastTriggeredDateKey[i] =
      0;
  }
}


// =====================================================
// DATE KEY
// =====================================================

uint32_t makeDateKey(
  const DateTime &dt
)
{
  return
    (uint32_t)dt.year()
    *
    10000UL
    +
    (uint32_t)dt.month()
    *
    100UL
    +
    (uint32_t)dt.day();
}


// =====================================================
// FIND NEXT DOSE
// =====================================================

int findNextDose(
  const DateTime &now,
  DateTime &nextDateTime
)
{
  int bestSlot =
    -1;


  uint32_t bestDelta =
    0xFFFFFFFFUL;


  uint32_t todayKey =
    makeDateKey(
      now
    );


  for (
    int i = 0;
    i < NUM_SLOTS;
    i++
  )
  {
    if (
      !slots[i].enabled
    )
    {
      continue;
    }


    DateTime candidate(
      now.year(),
      now.month(),
      now.day(),
      slots[i].hour,
      slots[i].minute,
      0
    );


    bool sameMinute =
      (
        now.hour()
        ==
        slots[i].hour
      )
      &&
      (
        now.minute()
        ==
        slots[i].minute
      );


    uint32_t delta =
      0;


    if (sameMinute)
    {
      if (
        lastTriggeredDateKey[i]
        ==
        todayKey
      )
      {
        candidate =
          candidate
          +
          TimeSpan(
            1,
            0,
            0,
            0
          );


        delta =
          candidate.unixtime()
          -
          now.unixtime();
      }

      else
      {
        delta =
          0;
      }
    }


    else if (
      candidate.unixtime()
      <
      now.unixtime()
    )
    {
      candidate =
        candidate
        +
        TimeSpan(
          1,
          0,
          0,
          0
        );


      delta =
        candidate.unixtime()
        -
        now.unixtime();
    }


    else
    {
      delta =
        candidate.unixtime()
        -
        now.unixtime();
    }


    if (
      delta <
      bestDelta
    )
    {
      bestDelta =
        delta;

      bestSlot =
        i;

      nextDateTime =
        candidate;
    }
  }


  return bestSlot;
}


// =====================================================
// PRINT NEXT DOSE
// =====================================================

void printNextDose()
{
  // Keep the e-paper in step with the serial output: whenever
  // the next dose is recalculated, the idle screen follows.
  epdIdle();

  if (!rtcOK)
  {
    Serial.println(
      "[SCHEDULER] RTC unavailable."
    );

    return;
  }


  DateTime now =
    rtc.now();


  DateTime next(
    2000,
    1,
    1,
    0,
    0,
    0
  );


  int slotIndex =
    findNextDose(
      now,
      next
    );


  Serial.println();

  Serial.println(
    "========== NEXT DOSE =========="
  );


  if (
    slotIndex <
    0
  )
  {
    Serial.println(
      "No enabled dose slots."
    );

    Serial.println(
      "==============================="
    );

    return;
  }


  DoseSlot &slot =
    slots[slotIndex];


  uint32_t secondsAway =
    0;


  if (
    next.unixtime()
    >
    now.unixtime()
  )
  {
    secondsAway =
      next.unixtime()
      -
      now.unixtime();
  }


  uint32_t totalMinutes =
    secondsAway
    /
    60;


  uint32_t hoursAway =
    totalMinutes
    /
    60;


  uint32_t minutesAway =
    totalMinutes
    %
    60;


  char timeText[16];


  formatTime12(
    slot.hour,
    slot.minute,
    timeText,
    sizeof(timeText)
  );


  Serial.printf(
    "Slot        : %d\n",
    slotIndex + 1
  );


  Serial.printf(
    "User        : %s\n",
    getUserName(
      slot.userId
    )
  );


  Serial.printf(
    "Date        : %04d-%02d-%02d\n",
    next.year(),
    next.month(),
    next.day()
  );


  Serial.printf(
    "Time        : %s\n",
    timeText
  );


  Serial.printf(
    "Compartment : C%d\n",
    slot.compartment
  );


  Serial.printf(
    "Medicine    : %s\n",
    slot.medName
  );


  Serial.printf(
    "In          : %lu h %lu min\n",
    hoursAway,
    minutesAway
  );


  Serial.println(
    "==============================="
  );
}


// =====================================================
// SCHEDULER
// =====================================================

void checkScheduler()
{
  if (
    !rtcOK
    ||
    !configOK
  )
  {
    return;
  }


  if (
    millis()
    -
    lastSchedulerCheck
    <
    SCHEDULER_CHECK_INTERVAL
  )
  {
    return;
  }


  lastSchedulerCheck =
    millis();


  DateTime now =
    rtc.now();


  uint32_t todayKey =
    makeDateKey(
      now
    );


  for (
    int i = 0;
    i < NUM_SLOTS;
    i++
  )
  {
    if (
      !slots[i].enabled
    )
    {
      continue;
    }

    // Course finished? Then this slot must stop reminding.
    if (
      !slotInCourse(
        i,
        now.unixtime()
      )
    )
    {
      continue;
    }


    if (
      now.hour()
      !=
      slots[i].hour
    )
    {
      continue;
    }


    if (
      now.minute()
      !=
      slots[i].minute
    )
    {
      continue;
    }


    if (
      lastTriggeredDateKey[i]
      ==
      todayKey
    )
    {
      continue;
    }


    lastTriggeredDateKey[i] =
      todayKey;


    triggerScheduleEvent(
      i,
      now
    );
  }
}


// =====================================================
// SCHEDULE EVENT
// =====================================================

void triggerScheduleEvent(
  int slotIndex,
  const DateTime &now
)
{
  DoseSlot &slot =
    slots[slotIndex];


  char scheduledTime[16];

  char triggerTime[20];


  formatTime12(
    slot.hour,
    slot.minute,
    scheduledTime,
    sizeof(scheduledTime)
  );


  formatTime12WithSeconds(
    now.hour(),
    now.minute(),
    now.second(),
    triggerTime,
    sizeof(triggerTime)
  );


  Serial.println();

  Serial.println(
    "****************************************"
  );


  Serial.println(
    "*** SCHEDULE EVENT TRIGGERED ***"
  );


  Serial.printf(
    "Slot        : %d\n",
    slotIndex + 1
  );


  Serial.printf(
    "User        : %s\n",
    getUserName(
      slot.userId
    )
  );


  Serial.printf(
    "Scheduled   : %s\n",
    scheduledTime
  );


  Serial.printf(
    "Triggered   : %s\n",
    triggerTime
  );


  Serial.printf(
    "Compartment : C%d\n",
    slot.compartment
  );


  Serial.printf(
    "Medicine    : %s\n",
    slot.medName
  );


  Serial.println(
    "****************************************"
  );


  logEvent(
    "SCHEDULED",
    slotIndex
  );


  if (
    reminderState !=
    REMINDER_IDLE
  )
  {
    Serial.println(
      "[REMINDER] Another reminder active."
    );

    return;
  }


  startReminder(
    slotIndex
  );
}


// =====================================================
// START REMINDER
// =====================================================

void startReminder(
  int slotIndex
)
{
  forceAllLocksLow();


  ledcWrite(
    BUZZER_PIN,
    0
  );


  pendingOutcome =
    OUTCOME_NONE;


  confirmationWindowStarted =
    0;


  activeReminderSlot =
    slotIndex;


  reminderState =
    REMINDER_BUZZER;

  // Bangla screen: dose reminder
  epdReminder(activeReminderSlot);


  reminderStateStarted =
    millis();


  logEvent(
    "REMINDER_STARTED",
    slotIndex
  );


  Serial.println();

  Serial.println(
    "========== REMINDER START =========="
  );


  Serial.printf(
    "User        : %s\n",
    getUserName(
      slots[
        slotIndex
      ].userId
    )
  );


  Serial.printf(
    "Compartment : C%d\n",
    slots[
      slotIndex
    ].compartment
  );


  Serial.printf(
    "Medicine    : %s\n",
    slots[
      slotIndex
    ].medName
  );


  Serial.println(
    "Step        : BUZZER"
  );


  Serial.println(
    "ALL LOCKS   : LOW"
  );


  Serial.println(
    "===================================="
  );


  ledcWrite(
    BUZZER_PIN,
    128
  );
}


// =====================================================
// REMINDER STATE MACHINE
// =====================================================

void serviceReminderStateMachine()
{
  if (
    reminderState ==
    REMINDER_IDLE
  )
  {
    return;
  }


  unsigned long now =
    millis();


  // ---------------------------------------------------
  // BUZZER
  // ---------------------------------------------------

  if (
    reminderState ==
    REMINDER_BUZZER
  )
  {
    if (
      now
      -
      reminderStateStarted
      >=
      BUZZER_DURATION_MS
    )
    {
      ledcWrite(
        BUZZER_PIN,
        0
      );


      reminderState =
        REMINDER_POWER_SETTLE;


      reminderStateStarted =
        now;
    }


    return;
  }


  // ---------------------------------------------------
  // POWER SETTLE
  // ---------------------------------------------------

  if (
    reminderState ==
    REMINDER_POWER_SETTLE
  )
  {
    if (
      now
      -
      reminderStateStarted
      >=
      POWER_SETTLE_MS
    )
    {
      uint16_t track =
        getReminderTrackForUser(
          slots[
            activeReminderSlot
          ].userId
        );


      safePlayTrack(
        track
      );


      reminderState =
        REMINDER_VOICE_1;


      reminderStateStarted =
        now;
    }


    return;
  }


  // ---------------------------------------------------
  // VOICE #1
  // ---------------------------------------------------

  if (
    reminderState ==
    REMINDER_VOICE_1
  )
  {
    if (
      now
      -
      reminderStateStarted
      >=
      REMINDER_VOICE_HOLD_MS
    )
    {
      uint16_t track =
        getReminderTrackForUser(
          slots[
            activeReminderSlot
          ].userId
        );


      safePlayTrack(
        track
      );


      reminderState =
        REMINDER_VOICE_2;


      reminderStateStarted =
        now;
    }


    return;
  }


  // ---------------------------------------------------
  // VOICE #2
  // ---------------------------------------------------

  if (
    reminderState ==
    REMINDER_VOICE_2
  )
  {
    if (
      now
      -
      reminderStateStarted
      >=
      REMINDER_VOICE_HOLD_MS
    )
    {
      safePlayTrack(
        TRACK_FP_PROMPT
      );


      authWindowStarted =
        now;


      lastFingerprintPrompt =
        now;


      fingerprintScanBlockedUntil =
        now;


      reminderState =
        REMINDER_WAIT_FINGERPRINT;

  // Bangla screen: ask for the fingerprint
  epdFingerprintPrompt(activeReminderSlot);


      Serial.println();

      Serial.println(
        "========== WAITING FOR FINGERPRINT =========="
      );


      Serial.printf(
        "Expected user : %s\n",
        getUserName(
          slots[
            activeReminderSlot
          ].userId
        )
      );


      Serial.println(
        "Auth timeout  : 10 minutes"
      );


      Serial.println(
        "============================================="
      );
    }


    return;
  }


  // ---------------------------------------------------
  // WAIT FINGERPRINT
  // ---------------------------------------------------

  if (
    reminderState ==
    REMINDER_WAIT_FINGERPRINT
  )
  {
    // Caregiver override: UP + DOWN held together. This is the
    // fallback when the sensor will not read a finger at all.
    // It is never silent - it is logged with its own event type
    // so it can never be mistaken for a normal unlock later.
    if (checkOverrideCombo())
    {
      Serial.println();
      Serial.println(
        ">>> CAREGIVER OVERRIDE <<<"
      );
      logEvent(
        "OVERRIDE",
        activeReminderSlot
      );
      epdOverride();
      startAuthorizedUnlock();
      return;
    }

    // An error message has been on screen long enough:
    // put the fingerprint prompt back so the user is not
    // left staring at an error while the dose still waits.
    if (
      wrongUserScreenUntil != 0
      &&
      millis() > wrongUserScreenUntil
    )
    {
      wrongUserScreenUntil = 0;
      epdFingerprintPrompt(activeReminderSlot);
    }

    if (
      now
      -
      authWindowStarted
      >=
      AUTH_WINDOW_MS
    )
    {
      forceAllLocksLow();


      startDoseOutcome(
        OUTCOME_MISSED
      );


      return;
    }


    if (
      now
      -
      lastFingerprintPrompt
      >=
      FP_PROMPT_INTERVAL_MS
    )
    {
      safePlayTrack(
        TRACK_FP_PROMPT
      );


      lastFingerprintPrompt =
        now;
    }


    if (
      now <
      fingerprintScanBlockedUntil
    )
    {
      return;
    }


    uint8_t scannedUser =
      255;


    uint8_t fingerprintID =
      0;


    uint16_t confidence =
      0;


    int result =
      scanFingerprint(
        scannedUser,
        fingerprintID,
        confidence
      );

    if (result != 0)
    {
      Serial.print("[FP] scan result=");
      Serial.print(result);
      Serial.println(
        result ==  1 ? "  (matched a known user)" :
        result == -2 ? "  (UNKNOWN finger - not enrolled)" :
                       "  (bad read - try again)"
      );
    }


    if (
      result ==
      0
    )
    {
      return;
    }


    if (
      result ==
      -1
    )
    {
      safePlayTrack(
        TRACK_FP_PROMPT
      );


      lastFingerprintPrompt =
        millis();


      fingerprintScanBlockedUntil =
        millis()
        +
        FP_RETRY_BLOCK_MS;


      return;
    }
    // A clean read that matches nobody, or an enrolled template
    // outside both users' ID ranges.
    //
    // This used to fall into the same branch as a smudged read and
    // simply replay "আঙুলের ছাপ দিন", which was indistinguishable
    // from the device ignoring you. Now it says so.
    if (
      result ==
      -2
    )
    {
      forceAllLocksLow();

      Serial.println();
      Serial.println(
        ">>> UNKNOWN FINGERPRINT - LOCK NOT FIRED <<<"
      );

      logEvent(
        "UNKNOWN_FINGER",
        activeReminderSlot,
        fingerprintID
      );

      safePlayTrack(
        TRACK_WRONG_USER
      );

      epdWrongUser();

      // Put the fingerprint prompt back up after the message has
      // had time to be read, so the screen does not get stuck on
      // an error while the dose is still waiting.
      wrongUserScreenUntil =
        millis()
        +
        WRONG_USER_SCREEN_MS;

      lastFingerprintPrompt =
        millis();

      fingerprintScanBlockedUntil =
        millis()
        +
        FP_RETRY_BLOCK_MS;

      return;
    }




    Serial.println();

    Serial.printf(
      "[FP] ID=%d User=%s Confidence=%d\n",
      fingerprintID,
      getUserName(
        scannedUser
      ),
      confidence
    );


    uint8_t expectedUser =
      slots[
        activeReminderSlot
      ].userId;


    // The caretaker's finger opens any compartment that is asking for
    // one. That is the whole point of it - but it is privileged, so it
    // is written to the log and sent out on Telegram rather than being
    // treated as an ordinary unlock.
    bool caretaker =
      (scannedUser == CARETAKER_USER)
      &&
      (expectedUser != CARETAKER_USER);


    if (caretaker)
    {
      Serial.println(
        "[FP] caretaker finger - opening on their authority"
      );

      logEvent(
        "CARETAKER_OPEN",
        activeReminderSlot,
        fingerprintID
      );
    }


    if (
      scannedUser
      !=
      expectedUser
      &&
      !caretaker
    )
    {
      forceAllLocksLow();


      logEvent(
        "WRONG_USER",
        activeReminderSlot,
        fingerprintID
      );


      Serial.println(
        ">>> WRONG USER - LOCK NOT FIRED <<<"
      );

      // Bangla screen: wrong person, nothing opened
      epdWrongUser();

      wrongUserScreenUntil =
        millis()
        +
        WRONG_USER_SCREEN_MS;


      safePlayTrack(
        TRACK_WRONG_USER
      );


      lastFingerprintPrompt =
        millis();


      fingerprintScanBlockedUntil =
        millis()
        +
        FP_RETRY_BLOCK_MS;


      return;
    }


    logEvent(
      "AUTH_SUCCESS",
      activeReminderSlot,
      fingerprintID
    );


    Serial.println();

    Serial.println(
      ">>> AUTHENTICATION SUCCESSFUL <<<"
    );


    startAuthorizedUnlock();


    return;
  }


  // ---------------------------------------------------
  // PRE UNLOCK
  // ---------------------------------------------------

  if (
    reminderState ==
    REMINDER_PRE_UNLOCK_SETTLE
  )
  {
    if (
      now
      -
      reminderStateStarted
      >=
      PRE_UNLOCK_SETTLE_MS
    )
    {
      if (
        activeReminderSlot < 0
        ||
        activeReminderSlot >=
        NUM_SLOTS
      )
      {
        abortReminderSafely(
          "Invalid active slot"
        );

        return;
      }


      uint8_t compartment =
        slots[
          activeReminderSlot
        ].compartment;


      int lockPin =
        getLockPinForCompartment(
          compartment
        );


      if (
        lockPin < 0
      )
      {
        abortReminderSafely(
          "Invalid compartment"
        );

        return;
      }


      forceAllLocksLow();


      Serial.printf(
        "[LOCK] C%d GPIO%d HIGH for %lu ms\n",
        compartment,
        lockPin,
        LOCK_PULSE_MS
      );


      digitalWrite(
        lockPin,
        HIGH
      );


      reminderState =
        REMINDER_UNLOCK_PULSE;


      reminderStateStarted =
        now;
    }


    return;
  }


  // ---------------------------------------------------
  // UNLOCK PULSE
  // ---------------------------------------------------

  if (
    reminderState ==
    REMINDER_UNLOCK_PULSE
  )
  {
    if (
      now
      -
      reminderStateStarted
      >=
      LOCK_PULSE_MS
    )
    {
      forceAllLocksLow();


      logEvent(
        "UNLOCKED",
        activeReminderSlot
      );


      Serial.println(
        "[LOCK] Pulse complete. ALL LOCKS LOW."
      );


      reminderState =
        REMINDER_POST_UNLOCK_SETTLE;


      reminderStateStarted =
        now;
    }


    return;
  }


  // ---------------------------------------------------
  // POST UNLOCK
  // ---------------------------------------------------

  if (
    reminderState ==
    REMINDER_POST_UNLOCK_SETTLE
  )
  {
    if (
      now
      -
      reminderStateStarted
      >=
      POST_UNLOCK_SETTLE_MS
    )
    {
      confirmationWindowStarted =
        now;


      reminderState =
        REMINDER_WAIT_CONFIRMATION;


      Serial.println();

      Serial.println(
        "========================================"
      );


      Serial.println(
        ">>> DOSE CONFIRMATION REQUIRED <<<"
      );


      Serial.println(
        "Medicine taken হলে SELECT চাপুন."
      );


      Serial.println(
        "Confirmation window : 120 seconds"
      );


      Serial.println(
        "Reed verification    : Deferred to 7B"
      );


      Serial.println(
        "========================================"
      );
    }


    return;
  }


  // ---------------------------------------------------
  // WAIT CONFIRMATION
  // ---------------------------------------------------

  if (
    reminderState ==
    REMINDER_WAIT_CONFIRMATION
  )
  {
    if (
      now
      -
      confirmationWindowStarted
      >=
      CONFIRMATION_WINDOW_MS
    )
    {
      startDoseOutcome(
        OUTCOME_UNCONFIRMED
      );


      return;
    }


    return;
  }


  // ---------------------------------------------------
  // RESULT AUDIO
  // ---------------------------------------------------

  if (
    reminderState ==
    REMINDER_RESULT_AUDIO
  )
  {
    if (
      now
      -
      reminderStateStarted
      >=
      RESULT_AUDIO_HOLD_MS
    )
    {
      finishReminder();

      return;
    }
  }
}


// =====================================================
// AUTHORIZED UNLOCK
// =====================================================

void startAuthorizedUnlock()
{
  if (
    activeReminderSlot < 0
    ||
    activeReminderSlot >=
    NUM_SLOTS
  )
  {
    abortReminderSafely(
      "Invalid active reminder slot"
    );

    return;
  }


  if (mp3OK)
  {
    player.stop();
  }


  ledcWrite(
    BUZZER_PIN,
    0
  );


  forceAllLocksLow();


  reminderState =
    REMINDER_PRE_UNLOCK_SETTLE;

  // Bangla screen: compartment opened
  epdUnlocked(activeReminderSlot);


  reminderStateStarted =
    millis();
}


// =====================================================
// SELECT -> TAKEN
// =====================================================

void confirmDoseTaken()
{
  if (
    reminderState !=
    REMINDER_WAIT_CONFIRMATION
  )
  {
    return;
  }


  Serial.println(
    "[CONFIRM] SELECT pressed."
  );


  startDoseOutcome(
    OUTCOME_TAKEN
  );
}


// =====================================================
// OUTCOME
// =====================================================

void startDoseOutcome(
  DoseOutcome outcome
)
{
  // Say WHY the dose ended, and how long it ran. Without this
  // a dose that finishes unexpectedly is impossible to explain
  // after the fact.
  Serial.println();
  Serial.print("[DOSE END] reason=");
  Serial.print(
    outcome == OUTCOME_TAKEN       ? "TAKEN (SELECT pressed)" :
    outcome == OUTCOME_MISSED      ? "MISSED (auth window expired)" :
    outcome == OUTCOME_UNCONFIRMED ? "UNCONFIRMED (no SELECT after unlock)" :
    outcome == OUTCOME_CANCELLED   ? "CANCELLED (BACK held)" : "?"
  );
  Serial.print("  slot=");
  Serial.print(activeReminderSlot);
  Serial.print("  state=");
  Serial.print((int)reminderState);
  Serial.print("  elapsed=");
  Serial.print((millis() - authWindowStarted) / 1000);
  Serial.println("s since auth window opened");

  pendingOutcome =
    outcome;


  forceAllLocksLow();


  ledcWrite(
    BUZZER_PIN,
    0
  );


  uint16_t track =
    0;


  if (
    outcome ==
    OUTCOME_TAKEN
  )
  {
    Serial.println(
      "STATUS : TAKEN"
    );


    logEvent(
      "TAKEN",
      activeReminderSlot
    );


    track =
      TRACK_TAKEN;
  }


  else if (
    outcome ==
    OUTCOME_MISSED
  )
  {
    Serial.println(
      "STATUS : MISSED"
    );


    logEvent(
      "MISSED",
      activeReminderSlot
    );


    track =
      TRACK_MISSED;
  }


  else if (
    outcome ==
    OUTCOME_UNCONFIRMED
  )
  {
    Serial.println(
      "STATUS : UNCONFIRMED"
    );


    logEvent(
      "UNCONFIRMED",
      activeReminderSlot
    );


    track =
      TRACK_UNCONFIRMED;
  }


  if (
    track !=
    0
  )
  {
    safePlayTrack(
      track
    );
  }


  reminderState =
    REMINDER_RESULT_AUDIO;

  // Bangla screen: the outcome of this dose
  epdOutcome(pendingOutcome);


  reminderStateStarted =
    millis();
}


// =====================================================
// BACK HOLD CANCEL
// =====================================================

void serviceBackHoldCancel()
{
  if (
    reminderState ==
    REMINDER_IDLE
  )
  {
    backHoldActive =
      false;

    backCancelTriggered =
      false;

    backHoldStarted =
      0;

    return;
  }


  bool pressed =
    (
      digitalRead(
        PIN_BTN_BACK
      )
      ==
      LOW
    );


  if (pressed)
  {
    if (
      !backHoldActive
    )
    {
      backHoldActive =
        true;


      backHoldStarted =
        millis();


      backCancelTriggered =
        false;
    }


    else if (
      !backCancelTriggered
      &&
      millis()
      -
      backHoldStarted
      >=
      REMINDER_CANCEL_HOLD_MS
    )
    {
      backCancelTriggered =
        true;


      cancelActiveReminder();
    }
  }


  else
  {
    backHoldActive =
      false;

    backCancelTriggered =
      false;

    backHoldStarted =
      0;
  }
}


// =====================================================
// CANCEL
// =====================================================

void cancelActiveReminder()
{
  pendingOutcome =
    OUTCOME_CANCELLED;


  ledcWrite(
    BUZZER_PIN,
    0
  );


  if (mp3OK)
  {
    player.stop();
  }


  forceAllLocksLow();


  logEvent(
    "CANCELLED",
    activeReminderSlot
  );


  Serial.println();

  Serial.println(
    ">>> REMINDER CANCELLED <<<"
  );


  Serial.println(
    "ALL LOCKS LOW"
  );


  clearReminderState();


  printNextDose();

  showMenu();
}


// =====================================================
// ABORT SAFELY
// =====================================================

void abortReminderSafely(
  const char* reason
)
{
  ledcWrite(
    BUZZER_PIN,
    0
  );


  if (mp3OK)
  {
    player.stop();
  }


  forceAllLocksLow();


  Serial.println();

  Serial.println(
    "REMINDER ABORTED SAFELY"
  );


  Serial.printf(
    "Reason: %s\n",
    reason
  );


  clearReminderState();


  printNextDose();

  showMenu();
}


// =====================================================
// FINISH REMINDER
// =====================================================

void finishReminder()
{
  DoseOutcome completedOutcome =
    pendingOutcome;


  ledcWrite(
    BUZZER_PIN,
    0
  );


  if (mp3OK)
  {
    player.stop();
  }


  forceAllLocksLow();


  Serial.println();

  Serial.println(
    "========== DOSE CYCLE COMPLETE =========="
  );


  if (
    completedOutcome ==
    OUTCOME_TAKEN
  )
  {
    Serial.println(
      "Final status : TAKEN"
    );

    // Was that the last dose of the course?
    if (
      slotIsFinalDose(
        activeReminderSlot,
        rtc.now().unixtime()
      )
    )
    {
      completeCourse(
        activeReminderSlot
      );
    }
  }


  else if (
    completedOutcome ==
    OUTCOME_MISSED
  )
  {
    Serial.println(
      "Final status : MISSED"
    );
  }


  else if (
    completedOutcome ==
    OUTCOME_UNCONFIRMED
  )
  {
    Serial.println(
      "Final status : UNCONFIRMED"
    );
  }


  Serial.println(
    "All locks   : LOW"
  );


  Serial.println(
    "========================================="
  );


  clearReminderState();


  printNextDose();

  showMenu();
}


// =====================================================
// CLEAR REMINDER STATE
// =====================================================

void clearReminderState()
{
  // Point the wake alarm at whatever dose comes next.
  armNextDoseAlarm();
  wrongUserScreenUntil = 0;
  // Return the screen to the next-dose view.
  epdIdle();
  activeReminderSlot =
    -1;


  reminderState =
    REMINDER_IDLE;


  reminderStateStarted =
    0;


  authWindowStarted =
    0;


  lastFingerprintPrompt =
    0;


  fingerprintScanBlockedUntil =
    0;


  confirmationWindowStarted =
    0;


  pendingOutcome =
    OUTCOME_NONE;


  backHoldActive =
    false;


  backCancelTriggered =
    false;


  backHoldStarted =
    0;


  menuState =
    MENU_BROWSE;
}


// =====================================================
// AUDIO
// =====================================================

void safePlayTrack(
  uint16_t track
)
{
  if (!mp3OK)
  {
    Serial.printf(
      "[AUDIO] MP3 unavailable. Track %04d skipped.\n",
      track
    );

    return;
  }

  // The card has never been seen and something wants to speak. Try
  // once more now rather than staying silent through a whole
  // reminder - the buzzer has already sounded, so a short pause here
  // costs nothing.
  if (!mp3CardOk)
  {
    int count = player.readFileCounts();

    if (count > 0)
    {
      mp3CardOk    = true;
      mp3FileCount = count;
      player.volume(mp3Volume);

      Serial.println("[AUDIO] card found on demand - playing");
    }
    else
    {
      Serial.printf(
        "[AUDIO] no SD card. Track %04d skipped (buzzer still alerts).\n",
        track
      );

      return;
    }
  }


  // Never ask for a track the card does not have. The module answers
  // an out-of-range number by playing its LAST file, so a missing
  // prompt comes out as some unrelated sentence - which is how asking
  // for 17 announced that the course of medicine had finished.
  if (mp3FileCount > 0 && (int)track > mp3FileCount)
  {
    Serial.printf(
      "[AUDIO] track %04d is not on the card (%d files). Staying silent\n"
      "        rather than playing the wrong message. Copy audio/MP3/ over.\n",
      track,
      mp3FileCount
    );

    return;
  }


  if (mp3UseFolder)
  {
    // Files in /MP3/, addressed by FILENAME.
    player.playMp3Folder(
      track
    );
  }
  else
  {
    // Files in the card ROOT, addressed by INDEX - i.e. the
    // order they were written to the card.
    player.play(
      track
    );
  }

  lastTrackPlayed = track;
}


// =====================================================
// USER REMINDER TRACK
// =====================================================

uint16_t getReminderTrackForUser(
  uint8_t userId
)
{
  for (
    int i = 0;
    i < NUM_USERS;
    i++
  )
  {
    if (
      users[i].userId
      ==
      userId
    )
    {
      return
        users[i].reminderTrack;
    }
  }


  return 1;
}


// =====================================================
// FINGERPRINT SCAN
// =====================================================

int scanFingerprint(
  uint8_t &userId,
  uint8_t &fingerprintID,
  uint16_t &confidence
)
{
  if (!fingerprintOK)
  {
    return 0;
  }


  int p =
    finger.getImage();


  if (
    p ==
    FINGERPRINT_NOFINGER
  )
  {
    return 0;
  }


  if (
    p !=
    FINGERPRINT_OK
  )
  {
    return -1;
  }


  p =
    finger.image2Tz();


  if (
    p !=
    FINGERPRINT_OK
  )
  {
    waitFingerRemoved();

    return -1;
  }


  p =
    finger.fingerSearch();


  if (
    p !=
    FINGERPRINT_OK
  )
  {
    waitFingerRemoved();

    return -2;   // real finger, matches nothing we know
  }


  fingerprintID =
    finger.fingerID;


  confidence =
    finger.confidence;


  // দাদু
  if (
    fingerprintID >= 1
    &&
    fingerprintID <= 9
  )
  {
    userId =
      0;
  }


  // ঠাকুমা
  else if (
    fingerprintID >= 10
    &&
    fingerprintID <= 19
  )
  {
    userId =
      1;
  }


  // কেয়ারটেকার
  else if (
    fingerprintID >= 20
    &&
    fingerprintID <= 29
  )
  {
    userId =
      CARETAKER_USER;
  }


  else
  {
    waitFingerRemoved();

    return -2;   // enrolled, but outside every known ID range
  }


  waitFingerRemoved();


  return 1;
}


// =====================================================
// WAIT FINGER REMOVAL
// =====================================================

void waitFingerRemoved()
{
  // Bounded on purpose. This used to be an unbounded while()
  // loop: a finger left resting on the sensor - or a sensor
  // returning anything other than NOFINGER - would hang the
  // whole device forever, with no reminders and no buttons.
  unsigned long startedAt = millis();

  while (
    finger.getImage()
    !=
    FINGERPRINT_NOFINGER
  )
  {
    if (millis() - startedAt > FINGER_REMOVE_TIMEOUT_MS)
    {
      Serial.println(
        "[FP] finger-removal wait timed out - continuing"
      );
      break;
    }
    delay(80);
  }

  delay(250);
}


// =====================================================
// BUTTON READ
// =====================================================


// =====================================================
// 15H. BROWSING TODAY'S DOSES, AND MOVING ONE FORWARD
//
// On the idle screen UP/DOWN step through today's four doses, so the
// household can see the whole day on the box itself instead of having
// to open the dashboard.
//
// From that view, SELECT moves the shown dose forward to be the next
// one - which means giving up the doses in between. That is a real
// decision about somebody's medicine, so it is gated:
//
//   * Only in full mode. In power saving the screen is a clock and
//     the radio is off; this stays out of the way entirely.
//   * Only while no dose is actually running.
//   * Only a dose still ahead today, that has not already been dealt
//     with. You cannot "move forward" something already over.
//   * It needs the FINGERPRINT OF THE SLOT'S OWNER. দাদু can reorder
//     দাদু's doses and nothing else. This is the same rule the dose
//     machine enforces, and it is why an unattended box cannot be
//     talked into skipping somebody's medicine.
//   * Only doses belonging to that same person are skipped. Choosing
//     a late dose never silently drops the other person's.
//
// Every skipped dose is logged, and one Telegram message goes out
// naming what was given up and what is now next. Skipping is never
// silent - that is the whole point of logging it.
// =====================================================

const unsigned long BROWSE_TIMEOUT_MS     = 30000;
const unsigned long FORCE_WRONG_MS        = 3500;
const unsigned long FORCE_AUTH_TIMEOUT_MS = 25000;
const unsigned long FORCE_RESULT_MS       = 6000;

// Browsing is a full-mode, nothing-else-happening affair.
bool browseAllowed()
{
  if (powerSaveEnabled) return false;
  if (enrollState != ENR_IDLE) return false;
  if (reminderState != REMINDER_IDLE) return false;
  if (!rtcOK || !configOK) return false;
  return true;
}

uint32_t todayKeyNow()
{
  DateTime now = rtc.now();
  return (uint32_t)now.year() * 10000UL +
         (uint32_t)now.month() * 100UL +
         (uint32_t)now.day();
}

// Can this slot be moved forward right now?
//   0 = yes
//   1 = no (disabled, already over, or already dealt with today)
//   2 = pointless, it is already the next dose
//   3 = the OTHER person has a dose due before this one
uint8_t forceSelectable(int slot)
{
  // The device screen is not a dose and can never be "made next".
  if (slot < 0 || slot >= NUM_SLOTS)   return 1;
  if (!slots[slot].enabled)            return 1;

  DateTime now = rtc.now();
  uint16_t nowMin  = now.hour() * 60 + now.minute();
  uint16_t slotMin = slots[slot].hour * 60 + slots[slot].minute;

  if (slotMin <= nowMin)                             return 1;
  if (lastTriggeredDateKey[slot] == todayKeyNow())   return 1;
  if (nextDoseSlot() == slot)                        return 2;

  // Somebody else's dose is due between now and this one. Only its
  // owner may give that up, so this selection cannot take effect -
  // and pretending otherwise is what made it look broken.
  uint32_t todayKey = todayKeyNow();

  for (int i = 0; i < NUM_SLOTS; i++)
  {
    if (i == slot)                            continue;
    if (!slots[i].enabled)                    continue;
    if (slots[i].userId == slots[slot].userId) continue;
    if (lastTriggeredDateKey[i] == todayKey)  continue;

    uint16_t sm = slots[i].hour * 60 + slots[i].minute;

    if (sm > nowMin && sm < slotMin) return 3;
  }

  return 0;
}


// The screen one past the last dose: what time it is, what day it is,
// and how loud the box is. Read-only until SELECT is pressed.
void epdDeviceInfo()
{
  String lines[3];

  DateTime now = rtc.now();

  lines[0] = bnTimeText(now.hour(), now.minute());

  // Numerals and the slash both exist in the font; Bangla month names
  // would need conjuncts it does not have.
  lines[1] = bnDigits(now.day()) + "/" +
             bnDigits(now.month()) + "/" +
             bnDigits(now.year());

  lines[2] = String(BN_VOLUME) + " " + bnDigits(mp3Volume) +
             " / " + bnDigits(MP3_VOLUME_MAX);

  epdFrame(BN_DEVICE_INFO, lines, 3);
}

// Remembered so a partial redraw can skip an unchanged number.
String epdLastVolText = "";

// fullRedraw paints the whole screen - number, rule and label.
// Otherwise only the band holding the number is repainted, which is
// the same trick the power-saver clock uses and takes a fraction of
// the time.
void epdVolume(bool fullRedraw)
{
  String volText = bnPre(bnDigits(mp3Volume) + " / " + bnDigits(MP3_VOLUME_MAX));

  if (!fullRedraw && volText == epdLastVolText) return;

  epdLastVolText = volText;

  epdBeginDisplay();

  // A partial update needs the controller to still hold the previous
  // image. After a hibernate it does not, so fall back to a full one.
  if (fullRedraw || epdHibernated)
  {
    display.setFullWindow();
    display.firstPage();
    do
    {
      display.fillScreen(GxEPD_WHITE);

      bangla.setCursor(EPD_MARGIN, 40);
      bangla.print(volText.c_str());

      display.drawLine(
        EPD_MARGIN, 52,
        200 - EPD_MARGIN, 52,
        GxEPD_BLACK
      );

      bangla.setCursor(EPD_MARGIN, 96);
      bangla.print(bnPre(String(BN_VOLUME_SET)).c_str());
    }
    while (display.nextPage());

    epdHibernated = false;
  }
  else
  {
    display.setPartialWindow(0, 0, 200, CLOCK_BAND_H);
    display.firstPage();
    do
    {
      display.fillRect(0, 0, 200, CLOCK_BAND_H, GxEPD_WHITE);

      bangla.setCursor(EPD_MARGIN, 40);
      bangla.print(volText.c_str());

      display.drawLine(
        EPD_MARGIN, 52,
        200 - EPD_MARGIN, 52,
        GxEPD_BLACK
      );
    }
    while (display.nextPage());
  }

  // The ordinary screens skip identical redraws by signature. Clear
  // it so whatever comes next always paints.
  epdLastSignature = "";
}

// Nudge the volume and make it audible straight away. The sound is
// the real feedback here - waiting a second and a half for e-paper to
// catch up before you can hear what you changed would be useless.
void volumeStep(int delta)
{
  int v = (int)mp3Volume + delta;

  if (v < 0)                 v = 0;
  if (v > MP3_VOLUME_MAX)    v = MP3_VOLUME_MAX;

  if ((uint8_t)v == mp3Volume) return;

  mp3Volume = (uint8_t)v;

  if (mp3OK) player.volume(mp3Volume);

  // Deliberately silent. There is no track that means "this is how
  // loud I am", and borrowing one that says something else is worse
  // than saying nothing.
  volRedrawAt = millis() + 300;

  Serial.printf("[AUDIO] volume %d\n", mp3Volume);
}

void epdBrowse(int slot)
{
  if (slot == BROWSE_DEVICE)
  {
    epdDeviceInfo();
    return;
  }

  String lines[3];

  // bnDigits takes a number, not a String - it renders Bangla
  // numerals, and the e-paper font has no Latin fallback at all.
  lines[0] = bnDigits(slot + 1) + " / " + bnDigits(NUM_SLOTS) + "  " +
             String(getUserName(slots[slot].userId));

  lines[1] = bnTimeText(slots[slot].hour, slots[slot].minute) +
             "  " + String(slots[slot].medName);

  if (!slots[slot].enabled)
  {
    lines[2] = String(BN_CANT_PICK);
  }
  else if (lastTriggeredDateKey[slot] == todayKeyNow())
  {
    lines[2] = String(BN_DONE_TODAY);
  }
  else
  {
    uint8_t why = forceSelectable(slot);

    lines[2] = (why == 0) ? String(BN_MAKE_NEXT)
             : (why == 2) ? String(BN_ALREADY_NEXT)
             : (why == 3) ? String(BN_OTHER_FIRST)
                          : String(BN_WAITING);
  }

  epdFrame(BN_TODAY, lines, 3);
}

void enterBrowse()
{
  int n = nextDoseSlot();
  browseSlot  = (n >= 0) ? n : 0;
  uiMode      = UI_BROWSE;
  uiModeSince = millis();

  epdBrowse(browseSlot);
}

void leaveBrowse()
{
  uiMode       = UI_IDLE;
  forceWrongAt = 0;
  volRedrawAt  = 0;

  // The idle screen dedupes on content, and it may well be showing
  // exactly what it showed before we started browsing.
  epdLastSignature = "";
  epdIdle();
}

// Actually move the chosen dose forward. Returns how many doses were
// given up to do it.
int applyForceSelect(int slot, int fpId)
{
  uint32_t todayKey = todayKeyNow();

  DateTime now = rtc.now();
  uint16_t nowMin = now.hour() * 60 + now.minute();
  uint16_t tgtMin = slots[slot].hour * 60 + slots[slot].minute;

  uint8_t owner = slots[slot].userId;
  int     given = 0;

  String lost = "";

  for (int i = 0; i < NUM_SLOTS; i++)
  {
    if (i == slot)                               continue;
    if (!slots[i].enabled)                       continue;
    if (slots[i].userId != owner)                continue;   // never theirs
    if (lastTriggeredDateKey[i] == todayKey)     continue;   // already done

    uint16_t sm = slots[i].hour * 60 + slots[i].minute;

    if (sm <= nowMin)  continue;    // already past today
    if (sm >= tgtMin)  continue;    // not between now and the target

    // Marking it as "already triggered today" is exactly what the
    // scheduler checks, so this is all it takes to skip it - and it
    // clears by itself at midnight.
    lastTriggeredDateKey[i] = todayKey;

    logEvent("DOSE_SKIPPED", i, fpId);
    given++;

    if (lost.length()) lost += ", ";
    lost += bnTimeText(slots[i].hour, slots[i].minute);
  }

  logEvent("DOSE_MOVED_UP", slot, fpId);

  armNextDoseAlarm();

  // One message naming both halves of the decision, rather than one
  // per skipped dose.
  if (given > 0)
  {
    String m = "⏭️ ডোজ এগিয়ে "
               "নেওয়া হয়েছে\n\n";

    m += String(getUserName(owner)) + "\n";
    m += "বাদ: " + lost + "\n";
    m += "এখন পরবর্তী: " +
         bnTimeText(slots[slot].hour, slots[slot].minute) + " — " +
         String(slots[slot].medName);

    tgQueueMessage(m);
  }

  // Spoken whenever the selection succeeds, not only when something
  // was given up: the dose was still chosen, and the confirmation is
  // what tells them it worked.
  safePlayTrack(TRACK_DOSE_MOVED);

  Serial.printf(
    "[DOSE] moved slot %d forward, %d given up - playing track %d\n",
    slot + 1,
    given,
    TRACK_DOSE_MOVED
  );

  return given;
}

// Fingerprint step. Runs from loop() while the prompt is on screen.
void serviceForceSelect()
{
  // A dose has started, or the box went into power saving, while the
  // browser was open. Drop the browser silently: whatever took over
  // has already drawn its own screen, and a timeout firing later
  // would paint the idle screen straight over the top of it.
  if (uiMode != UI_IDLE && !browseAllowed())
  {
    uiMode       = UI_IDLE;
    forceWrongAt = 0;
    return;
  }

  if (uiMode == UI_BROWSE)
  {
    if (millis() - uiModeSince > BROWSE_TIMEOUT_MS) leaveBrowse();
    return;
  }

  if (uiMode == UI_FORCE_RESULT)
  {
    if (millis() - uiModeSince > FORCE_RESULT_MS) leaveBrowse();
    return;
  }

  if (uiMode == UI_VOLUME)
  {
    // Catch the screen up once the pressing stops.
    if (volRedrawAt != 0 && millis() >= volRedrawAt)
    {
      volRedrawAt = 0;
      epdVolume(false);
    }

    // Walking away must not leave the dial open, and must not lose
    // the change either.
    if (millis() - uiModeSince > BROWSE_TIMEOUT_MS)
    {
      saveVolume();
      leaveBrowse();
    }

    return;
  }

  if (uiMode != UI_FORCE_AUTH) return;

  // Holding the "wrong person" message on screen. Do not read the
  // sensor - the same finger is probably still on it.
  if (forceWrongAt != 0)
  {
    if (millis() - forceWrongAt < FORCE_WRONG_MS) return;

    forceWrongAt     = 0;
    uiModeSince      = millis();
    epdLastSignature = "";

    epdFingerprintPrompt(browseSlot);
    return;
  }

  if (millis() - uiModeSince > FORCE_AUTH_TIMEOUT_MS)
  {
    Serial.println("[DOSE] move-forward timed out");
    leaveBrowse();
    return;
  }

  uint8_t  userId = 0;
  uint8_t  fpId   = 0;
  uint16_t conf   = 0;

  int r = scanFingerprint(userId, fpId, conf);

  if (r == 0 || r == -1) return;        // nothing there, or a poor read

  bool allowed =
    (r == 1)
    &&
    (userId == slots[browseSlot].userId || userId == CARETAKER_USER);

  if (!allowed)
  {
    // The owner of the slot, or the caretaker, and nobody else.
    Serial.println("[DOSE] wrong person - move-forward refused");

    logEvent("WRONG_USER", browseSlot, (r == -2) ? -1 : (int)fpId);

    epdWrongUser();
    safePlayTrack(TRACK_WRONG_USER);

    // The prompt goes back up a few seconds from now, once they have
    // had a chance to read this.
    forceWrongAt = millis();
    return;
  }

  int given = applyForceSelect(browseSlot, (int)fpId);

  String lines[2];
  lines[0] = bnTimeText(slots[browseSlot].hour, slots[browseSlot].minute) +
             "  " + String(slots[browseSlot].medName);
  lines[1] = String(getUserName(slots[browseSlot].userId));

  epdLastSignature = "";
  epdFrame(given > 0 ? BN_MOVED : BN_ALREADY_NEXT, lines, 2);

  uiMode      = UI_FORCE_RESULT;
  uiModeSince = millis();
}

// Buttons, while the idle screen or the browser is showing.
// Returns true when it has dealt with the press.
bool handleBrowseButton(ButtonEvent button)
{
  if (!browseAllowed())
  {
    if (uiMode != UI_IDLE) uiMode = UI_IDLE;
    return false;
  }

  // From the idle screen, UP, DOWN or SELECT all open the browser.
  //
  // Every press is swallowed here, including BACK. The old button
  // menu underneath would otherwise catch SELECT and quietly drop
  // into "edit the hour" with nothing shown on the e-paper, and the
  // next UP/DOWN would change a dose time instead of browsing.
  // Times are edited on the dashboard now.
  if (uiMode == UI_IDLE)
  {
    if (button == BUTTON_UP ||
        button == BUTTON_DOWN ||
        button == BUTTON_SELECT)
    {
      enterBrowse();
    }

    return true;
  }

  if (uiMode == UI_FORCE_RESULT)
  {
    leaveBrowse();
    return true;
  }

  if (uiMode == UI_VOLUME)
  {
    uiModeSince = millis();

    if (button == BUTTON_UP)   { volumeStep(+1); return true; }
    if (button == BUTTON_DOWN) { volumeStep(-1); return true; }

    // SELECT and BACK both save and come out - confirming the volume
    // and backing out of it amount to the same thing here.
    saveVolume();

    String lines[1];
    lines[0] = bnDigits(mp3Volume) + " / " + bnDigits(MP3_VOLUME_MAX);

    epdLastSignature = "";
    epdFrame(BN_VOLUME_SAVED, lines, 1);

    uiMode      = UI_FORCE_RESULT;
    uiModeSince = millis();
    return true;
  }

  if (uiMode == UI_FORCE_AUTH)
  {
    if (button == BUTTON_BACK)
    {
      Serial.println("[DOSE] move-forward cancelled");
      leaveBrowse();
    }
    return true;
  }

  // ---- UI_BROWSE ----
  uiModeSince = millis();

  if (button == BUTTON_BACK)
  {
    leaveBrowse();
    return true;
  }

  if (button == BUTTON_UP || button == BUTTON_DOWN)
  {
    browseSlot += (button == BUTTON_DOWN) ? 1 : -1;

    if (browseSlot >= BROWSE_COUNT) browseSlot = 0;
    if (browseSlot < 0)             browseSlot = BROWSE_COUNT - 1;

    epdBrowse(browseSlot);
    return true;
  }

  if (button == BUTTON_SELECT)
  {
    if (browseSlot == BROWSE_DEVICE)
    {
      uiMode      = UI_VOLUME;
      uiModeSince = millis();
      volRedrawAt = 0;

      epdLastSignature = "";
      epdLastVolText   = "";
      epdVolume(true);

      Serial.println("[AUDIO] adjusting the volume");
      return true;
    }

    uint8_t why = forceSelectable(browseSlot);

    if (why != 0)
    {
      String lines[1];
      lines[0] = bnTimeText(slots[browseSlot].hour, slots[browseSlot].minute);

      epdFrame(why == 2 ? BN_ALREADY_NEXT
             : why == 3 ? BN_OTHER_FIRST
                        : BN_CANT_PICK, lines, 1);

      uiMode      = UI_FORCE_RESULT;
      uiModeSince = millis();
      return true;
    }

    Serial.printf(
      "[DOSE] move slot %d forward - waiting for %s's finger\n",
      browseSlot + 1,
      getUserName(slots[browseSlot].userId)
    );

    uiMode      = UI_FORCE_AUTH;
    uiModeSince = millis();

    epdLastSignature = "";
    epdFingerprintPrompt(browseSlot);
    return true;
  }

  return true;
}


// =====================================================
// 15I. CHANGING A FINGERPRINT
//
// Started from the dashboard, carried out here. Three angles of the
// same finger - middle, left edge, right edge - each enrolled twice,
// because the sensor builds one template from two impressions and
// will refuse two that differ too much. Three templates per person
// is what makes a slightly rolled finger still open the box.
//
// Before anything is changed, the person must present the finger they
// already have. Otherwise anyone standing at the dashboard with the
// PIN could quietly replace দাদু's fingerprint with their own and
// walk away with access to his medicine. The caretaker may authorise
// in their place - that is what the caretaker is for.
//
// A person with nothing enrolled yet skips that step. There is
// nothing to prove, and demanding it would make a new device
// impossible to set up.
// =====================================================

const unsigned long ENROLL_STEP_TIMEOUT_MS = 30000;
const unsigned long ENROLL_RESULT_MS       = 8000;

uint8_t fpIdFirst(uint8_t u)
{
  return (u == 0) ? 1 : (u == 1) ? 10 : 20;
}

uint8_t fpIdLast(uint8_t u)
{
  return (u == 0) ? 9 : (u == 1) ? 19 : 29;
}

void enrollSay(const char *msg)
{
  strncpy(enrollMsg, msg, sizeof(enrollMsg) - 1);
  enrollMsg[sizeof(enrollMsg) - 1] = 0;
}

// Slow: one UART exchange per slot. Call it from setup() and after an
// enrolment, never from a web handler.
void refreshEnrollCounts()
{
  if (!fingerprintOK) return;

  // This asks the sensor about thirty template slots, one UART
  // exchange each. A slot that is empty answers at once, but a sensor
  // that is not answering at all costs the library's full one second
  // timeout per slot - up to thirty seconds.
  //
  // That runs inside setup(), where the task watchdog is already
  // watching loopTask, so the unguarded version could reset the board
  // before setup() ever finished: a boot loop with no obvious cause.
  //
  // So: feed the watchdog every slot, and give up on the whole count
  // the moment the sensor looks unresponsive. The count is only used
  // to label buttons on the dashboard - it is never worth a reboot.
  const uint8_t MAX_SILENT = 3;
  uint8_t silent = 0;

  for (uint8_t u = 0; u < 3; u++)
  {
    uint8_t n = 0;

    for (uint8_t id = fpIdFirst(u); id <= fpIdLast(u); id++)
    {
      esp_task_wdt_reset();

      uint8_t r = finger.loadModel(id);

      if (r == FINGERPRINT_OK)
      {
        n = n + 1;
        silent = 0;
      }
      else if (r == FINGERPRINT_PACKETRECIEVEERR || r == FINGERPRINT_TIMEOUT)
      {
        // No reply at all, as opposed to "that slot is empty".
        silent = silent + 1;

        if (silent >= MAX_SILENT)
        {
          Serial.println(
            "[FP] sensor stopped answering - skipping the rest of the count"
          );

          enrollCount[u] = n;
          return;
        }
      }

      delay(0);   // let the watchdog task and Wi-Fi breathe
    }

    enrollCount[u] = n;
  }

  Serial.printf(
    "[FP] prints on file - %s:%d  %s:%d  %s:%d\n",
    getUserName(0), enrollCount[0],
    getUserName(1), enrollCount[1],
    getUserName(CARETAKER_USER), enrollCount[2]
  );
}

bool userHasTemplate(uint8_t u)
{
  return enrollCount[u] > 0;
}

const char *enrollAnglePrompt(uint8_t angle)
{
  return (angle == 0) ? BN_FP_MID
       : (angle == 1) ? BN_FP_LEFT
                      : BN_FP_RIGHT;
}

uint16_t enrollAngleTrack(uint8_t angle)
{
  return (angle == 0) ? TRACK_FP_MID
       : (angle == 1) ? TRACK_FP_LEFT
                      : TRACK_FP_RIGHT;
}

// Spoken on the way INTO a step, never from the redraw - enrollScreen
// runs every pass through loop() and would otherwise stammer.
void enrollSpeakStep()
{
  if (enrollState == ENR_VERIFY)      { safePlayTrack(TRACK_FP_CHANGE); return; }
  if (enrollState == ENR_DONE)        { safePlayTrack(TRACK_FP_SAVED);  return; }

  if (enrollState != ENR_PRESS) return;

  safePlayTrack(enrollPress == 0 ? enrollAngleTrack(enrollAngle)
                                 : TRACK_FP_AGAIN);
}

void enrollScreen()
{
  String lines[2];

  if (enrollState == ENR_VERIFY)
  {
    lines[0] = String(getUserName(enrollUser));
    lines[1] = String(BN_FP_OLD);
    epdFrame(BN_FP_ENROLL, lines, 2);
    return;
  }

  if (enrollState == ENR_PRESS)
  {
    lines[0] = String(enrollAnglePrompt(enrollAngle));
    lines[1] = (enrollPress == 0) ? String(getUserName(enrollUser))
                                  : String(BN_FP_AGAIN);
    epdFrame(BN_FP_ENROLL, lines, 2);
    return;
  }

  if (enrollState == ENR_LIFT)
  {
    lines[0] = String(BN_FP_LIFT);
    epdFrame(BN_FP_ENROLL, lines, 1);
    return;
  }

  if (enrollState == ENR_DONE)
  {
    lines[0] = String(getUserName(enrollUser));
    lines[1] = String(BN_FP_SAVED);
    epdFrame(BN_FP_ENROLL, lines, 2);
    return;
  }

  lines[0] = String(BN_FP_RETRY);
  epdFrame(BN_FP_ENROLL, lines, 1);
}

void enrollFinish(bool ok, const char *msg)
{
  enrollState = ok ? ENR_DONE : ENR_FAIL;
  enrollSince = millis();

  enrollSay(msg);
  epdLastSignature = "";
  enrollScreen();

  if (ok) enrollSpeakStep();

  Serial.printf("[FP] enrolment %s - %s\n", ok ? "done" : "failed", msg);
}

void enrollBegin(uint8_t user)
{
  enrollUser  = user;
  enrollAngle = 0;
  enrollPress = 0;
  enrollSince = millis();

  Serial.printf("[FP] changing the fingerprint for %s\n", getUserName(user));

  // Verification is skipped only on a device with no prints at all -
  // there is nothing to prove yet, and demanding it would make a new
  // box impossible to set up.
  bool anyEnrolled = userHasTemplate(0) ||
                     userHasTemplate(1) ||
                     userHasTemplate(CARETAKER_USER);

  if (anyEnrolled)
  {
    enrollState = ENR_VERIFY;
    enrollSay("পুরানো ছাপ দিন");
  }
  else
  {
    // Nothing on file, so there is nothing to prove.
    enrollState = ENR_PRESS;
    enrollSay("মধ্য ভাগের ছাপ দিন");
  }

  epdLastSignature = "";
  enrollScreen();
  enrollSpeakStep();
}

// Called only AFTER all three new templates are safely stored.
//
// The new ones land on base+0..2, overwriting whatever was there. Any
// further templates this person had beyond those three are leftovers
// from an older enrolment and must go, or the old finger would still
// open the box and "changed" would be a lie.
//
// Deliberately not done up front: a failure halfway through would
// then have left them with no fingerprint at all.
void enrollTrimExtras(uint8_t u)
{
  for (uint8_t id = fpIdFirst(u) + 3; id <= fpIdLast(u); id++)
  {
    finger.deleteModel(id);
  }
}

void serviceEnroll()
{
  // A dose always wins. If one starts mid-enrolment, drop it rather
  // than fighting over the sensor and the screen.
  if (enrollState != ENR_IDLE &&
      enrollState != ENR_DONE &&
      enrollState != ENR_FAIL &&
      reminderState != REMINDER_IDLE)
  {
    enrollState = ENR_IDLE;
    enrollSay("ডোজ শুরু হওয়ায় "
              "বাতিল");
    Serial.println("[FP] enrolment abandoned - a dose started");
    return;
  }

  // A request from the dashboard.
  if (enrollWanted)
  {
    enrollWanted = false;

    if (reminderState != REMINDER_IDLE || !fingerprintOK)
    {
      enrollState = ENR_FAIL;
      enrollSince = millis();
      enrollSay("এখন সম্ভব নয়");
      return;
    }

    enrollBegin(enrollWantUser);
    return;
  }

  if (enrollState == ENR_IDLE) return;

  // Clear the finished screen after a while and give the box back.
  if (enrollState == ENR_DONE || enrollState == ENR_FAIL)
  {
    if (millis() - enrollSince < ENROLL_RESULT_MS) return;

    enrollState      = ENR_IDLE;
    epdLastSignature = "";
    epdIdle();
    return;
  }

  if (millis() - enrollSince > ENROLL_STEP_TIMEOUT_MS)
  {
    enrollFinish(false, "সময় শেষ");
    return;
  }

  enrollScreen();

  // ---- prove who you are with the finger already on file ----
  if (enrollState == ENR_VERIFY)
  {
    uint8_t  uid = 0, fid = 0;
    uint16_t conf = 0;

    int r = scanFingerprint(uid, fid, conf);

    if (r == 0 || r == -1) return;

    // Your own finger, or the caretaker's.
    bool ok = (r == 1) && (uid == enrollUser || uid == CARETAKER_USER);

    // Adding the FIRST caretaker is the one case where neither of
    // those can exist yet. A resident may authorise it - somebody
    // trusted still has to be standing at the box.
    if (!ok && r == 1 &&
        enrollUser == CARETAKER_USER &&
        !userHasTemplate(CARETAKER_USER))
    {
      ok = (uid == 0 || uid == 1);
    }

    if (!ok)
    {
      epdWrongUser();
      safePlayTrack(TRACK_WRONG_USER);

      enrollFinish(false,
        "ভুল ব্যবহারকারী");
      return;
    }

    Serial.println("[FP] identity confirmed - taking the new print");

    enrollState = ENR_PRESS;
    enrollAngle = 0;
    enrollPress = 0;
    enrollSince = millis();

    enrollSay("মধ্য ভাগের ছাপ দিন");

    epdLastSignature = "";
    enrollSpeakStep();
    return;
  }

  // ---- wait for the finger to come off between impressions ----
  if (enrollState == ENR_LIFT)
  {
    if (finger.getImage() != FINGERPRINT_NOFINGER) return;

    enrollState = ENR_PRESS;
    enrollSince = millis();

    enrollSay(enrollPress == 0
              ? "একই ভাবে আবার দিন"
              : "পরের দিক দিয় দিন");

    epdLastSignature = "";
    enrollSpeakStep();
    return;
  }

  // ---- take an impression ----
  if (enrollState != ENR_PRESS) return;

  if (finger.getImage() != FINGERPRINT_OK) return;

  // Buffer 1 for the first impression, buffer 2 for the second.
  if (finger.image2Tz(enrollPress + 1) != FINGERPRINT_OK)
  {
    // A smudged read. Lift and try the same impression again.
    enrollState = ENR_LIFT;
    enrollSince = millis();
    return;
  }

  if (enrollPress == 0)
  {
    enrollPress = 1;
    enrollState = ENR_LIFT;
    enrollSince = millis();
    return;
  }

  // Second impression of this angle - build and store the template.
  if (finger.createModel() != FINGERPRINT_OK)
  {
    // The two impressions did not agree. Start this angle over rather
    // than storing something that will not match reliably.
    enrollPress = 0;
    enrollState = ENR_LIFT;
    enrollSince = millis();

    enrollSay("মিলল না, আবার");
    return;
  }

  uint8_t id = fpIdFirst(enrollUser) + enrollAngle;

  if (finger.storeModel(id) != FINGERPRINT_OK)
  {
    enrollFinish(false, "সংরক্ষণ ব্যর্থ");
    return;
  }

  Serial.printf("[FP] stored template %d (angle %d)\n", id, enrollAngle);

  enrollAngle++;
  enrollPress = 0;

  if (enrollAngle >= 3)
  {
    // Now, and only now, is it safe to drop the old extras.
    enrollTrimExtras(enrollUser);
    refreshEnrollCounts();

    logEvent("FINGERPRINT_CHANGED", -1, (int)id);

    finger.getTemplateCount();          // refresh the count we report

    enrollFinish(true, "সংরক্ষিত হয়েছে");
    return;
  }

  enrollState = ENR_LIFT;
  enrollSince = millis();
}

ButtonEvent readButtonEvent()
{
  unsigned long now =
    millis();


  for (
    int i = 0;
    i < 4;
    i++
  )
  {
    bool raw =
      digitalRead(
        buttonPins[i]
      );


    if (
      raw !=
      lastRawState[i]
    )
    {
      lastRawState[i] =
        raw;


      stateChangedAt[i] =
        now;
    }


    if (
      now
      -
      stateChangedAt[i]
      >=
      DEBOUNCE_MS
      &&
      raw !=
      stableState[i]
    )
    {
      stableState[i] =
        raw;


      if (
        raw ==
        LOW
      )
      {
        if (i == 0)
          return BUTTON_UP;

        if (i == 1)
          return BUTTON_DOWN;

        if (i == 2)
          return BUTTON_SELECT;

        if (i == 3)
          return BUTTON_BACK;
      }
    }
  }


  return BUTTON_NONE;
}


// =====================================================
// MENU HANDLER
// =====================================================

void handleMenuButton(
  ButtonEvent button
)
{
  switch (
    menuState
  )
  {
    case MENU_BROWSE:
    {
      if (
        button ==
        BUTTON_UP
      )
      {
        selectedSlot--;


        if (
          selectedSlot < 0
        )
        {
          selectedSlot =
            NUM_SLOTS - 1;
        }


        showMenu();
      }


      else if (
        button ==
        BUTTON_DOWN
      )
      {
        selectedSlot++;


        if (
          selectedSlot >=
          NUM_SLOTS
        )
        {
          selectedSlot =
            0;
        }


        showMenu();
      }


      else if (
        button ==
        BUTTON_SELECT
      )
      {
        tempHour =
          slots[
            selectedSlot
          ].hour;


        tempMinute =
          slots[
            selectedSlot
          ].minute;


        tempEnabled =
          slots[
            selectedSlot
          ].enabled;


        menuState =
          MENU_EDIT_HOUR;


        showMenu();
      }


      break;
    }


    case MENU_EDIT_HOUR:
    {
      if (
        button ==
        BUTTON_UP
      )
      {
        tempHour =
          (
            tempHour + 1
          )
          %
          24;


        showMenu();
      }


      else if (
        button ==
        BUTTON_DOWN
      )
      {
        tempHour =
          (
            tempHour == 0
          )
          ?
          23
          :
          tempHour - 1;


        showMenu();
      }


      else if (
        button ==
        BUTTON_SELECT
      )
      {
        menuState =
          MENU_EDIT_MINUTE;


        showMenu();
      }


      else if (
        button ==
        BUTTON_BACK
      )
      {
        menuState =
          MENU_BROWSE;


        showMenu();
      }


      break;
    }


    case MENU_EDIT_MINUTE:
    {
      if (
        button ==
        BUTTON_UP
      )
      {
        tempMinute =
          (
            tempMinute + 1
          )
          %
          60;


        showMenu();
      }


      else if (
        button ==
        BUTTON_DOWN
      )
      {
        tempMinute =
          (
            tempMinute == 0
          )
          ?
          59
          :
          tempMinute - 1;


        showMenu();
      }


      else if (
        button ==
        BUTTON_SELECT
      )
      {
        menuState =
          MENU_EDIT_ENABLED;


        showMenu();
      }


      else if (
        button ==
        BUTTON_BACK
      )
      {
        menuState =
          MENU_EDIT_HOUR;


        showMenu();
      }


      break;
    }


    case MENU_EDIT_ENABLED:
    {
      if (
        button ==
        BUTTON_UP
        ||
        button ==
        BUTTON_DOWN
      )
      {
        tempEnabled =
          !tempEnabled;


        showMenu();
      }


      else if (
        button ==
        BUTTON_SELECT
      )
      {
        menuState =
          MENU_CONFIRM;


        showMenu();
      }


      else if (
        button ==
        BUTTON_BACK
      )
      {
        menuState =
          MENU_EDIT_MINUTE;


        showMenu();
      }


      break;
    }


    case MENU_CONFIRM:
    {
      if (
        button ==
        BUTTON_SELECT
      )
      {
        saveEditedSlot();
      }


      else if (
        button ==
        BUTTON_BACK
      )
      {
        menuState =
          MENU_EDIT_ENABLED;


        showMenu();
      }


      break;
    }
  }
}


// =====================================================
// SHOW MENU
// =====================================================

void showMenu()
{
  Serial.println();

  Serial.println(
    "========== SCHEDULE MENU =========="
  );


  DoseSlot &slot =
    slots[
      selectedSlot
    ];


  char timeText[16];


  switch (
    menuState
  )
  {
    case MENU_BROWSE:
    {
      formatTime12(
        slot.hour,
        slot.minute,
        timeText,
        sizeof(timeText)
      );


      Serial.printf(
        "Slot %d/%d\n",
        selectedSlot + 1,
        NUM_SLOTS
      );


      Serial.printf(
        "User        : %s\n",
        getUserName(
          slot.userId
        )
      );


      Serial.printf(
        "Time        : %s\n",
        timeText
      );


      Serial.printf(
        "Compartment : C%d\n",
        slot.compartment
      );


      Serial.printf(
        "Medicine    : %s\n",
        slot.medName
      );


      Serial.printf(
        "Enabled     : %s\n",
        slot.enabled
          ?
          "YES"
          :
          "NO"
      );


      Serial.println(
        "UP/DOWN = Slot"
      );


      Serial.println(
        "SELECT  = Edit"
      );


      break;
    }


    case MENU_EDIT_HOUR:
    {
      formatTime12(
        tempHour,
        tempMinute,
        timeText,
        sizeof(timeText)
      );


      Serial.printf(
        "Editing Slot %d | Hour | %s\n",
        selectedSlot + 1,
        timeText
      );


      break;
    }


    case MENU_EDIT_MINUTE:
    {
      formatTime12(
        tempHour,
        tempMinute,
        timeText,
        sizeof(timeText)
      );


      Serial.printf(
        "Editing Slot %d | Minute | %s\n",
        selectedSlot + 1,
        timeText
      );


      break;
    }


    case MENU_EDIT_ENABLED:
    {
      Serial.printf(
        "Enabled: [%s]\n",
        tempEnabled
          ?
          "YES"
          :
          "NO"
      );


      break;
    }


    case MENU_CONFIRM:
    {
      formatTime12(
        tempHour,
        tempMinute,
        timeText,
        sizeof(timeText)
      );


      Serial.println(
        "SAVE THIS CONFIGURATION?"
      );


      Serial.printf(
        "%s | %s | C%d | %s\n",
        getUserName(
          slot.userId
        ),
        timeText,
        slot.compartment,
        tempEnabled
          ?
          "ENABLED"
          :
          "DISABLED"
      );


      Serial.println(
        "SELECT = SAVE | BACK = EDIT"
      );


      break;
    }
  }


  Serial.println(
    "==================================="
  );
}


// =====================================================
// GAP VALIDATION
// =====================================================

bool scheduleGapValid(
  int editedIndex,
  uint8_t hour,
  uint8_t minute,
  bool enabled
)
{
  if (!enabled)
  {
    return true;
  }


  int candidate =
    hour
    *
    60
    +
    minute;


  for (
    int i = 0;
    i < NUM_SLOTS;
    i++
  )
  {
    if (
      i ==
      editedIndex
    )
    {
      continue;
    }


    if (
      !slots[i].enabled
    )
    {
      continue;
    }


    int other =
      slots[i].hour
      *
      60
      +
      slots[i].minute;


    int difference =
      abs(
        candidate
        -
        other
      );


    if (
      difference >
      720
    )
    {
      difference =
        1440
        -
        difference;
    }


    if (
      difference <
      MIN_SCHEDULE_GAP_MIN
    )
    {
      Serial.printf(
        "[VALIDATION] Too close to Slot %d\n",
        i + 1
      );


      return false;
    }
  }


  return true;
}


// =====================================================
// SAVE EDIT
// =====================================================

void saveEditedSlot()
{
  if (
    !scheduleGapValid(
      selectedSlot,
      tempHour,
      tempMinute,
      tempEnabled
    )
  )
  {
    // Say WHICH slot it clashes with, otherwise "rejected" looks
    // like the device simply refusing to accept AM times.
    Serial.println();
    Serial.println(
      "SAVE REJECTED - doses must be at least 15 minutes apart."
    );

    {
      int cand = tempHour * 60 + tempMinute;

      for (int i = 0; i < NUM_SLOTS; i++)
      {
        if (i == selectedSlot) continue;
        if (!slots[i].enabled) continue;

        int other = slots[i].hour * 60 + slots[i].minute;
        int diff = abs(cand - other);
        if (diff > 720) diff = 1440 - diff;

        if (diff < MIN_SCHEDULE_GAP_MIN)
        {
          char clash[20];
          formatTime12(
            slots[i].hour,
            slots[i].minute,
            clash,
            sizeof(clash)
          );
          Serial.printf(
            "  clashes with Slot %d (%s) - only %d min apart\n",
            i + 1,
            clash,
            diff
          );
        }
      }

      Serial.println(
        "  Pick another time, or disable the other slot first."
      );
      Serial.println();
    }


    menuState =
      MENU_EDIT_MINUTE;


    showMenu();

    return;
  }


  uint8_t oldHour =
    slots[
      selectedSlot
    ].hour;


  uint8_t oldMinute =
    slots[
      selectedSlot
    ].minute;


  bool oldEnabled =
    slots[
      selectedSlot
    ].enabled;


  slots[
    selectedSlot
  ].hour =
    tempHour;


  slots[
    selectedSlot
  ].minute =
    tempMinute;


  slots[
    selectedSlot
  ].enabled =
    tempEnabled;


  if (
    saveConfiguration()
  )
  {
    Serial.println(
      ">>> SCHEDULE SAVED <<<"
    );


    lastTriggeredDateKey[
      selectedSlot
    ] =
      0;


    menuState =
      MENU_BROWSE;


    printConfiguration();

    showMenu();

    printNextDose();
  }


  else
  {
    slots[
      selectedSlot
    ].hour =
      oldHour;


    slots[
      selectedSlot
    ].minute =
      oldMinute;


    slots[
      selectedSlot
    ].enabled =
      oldEnabled;


    Serial.println(
      "SAVE FAILED - OLD CONFIG RESTORED"
    );


    menuState =
      MENU_BROWSE;


    showMenu();
  }

  // The schedule just changed, so the hardware wake alarm has to
  // follow it - otherwise the device would sleep through the new time.
  armNextDoseAlarm();
}


// =====================================================
// USER NAME
// =====================================================

const char* getUserName(
  uint8_t userId
)
{
  // Not in users[] - see the note on CARETAKER_USER.
  if (userId == CARETAKER_USER) return BN_CARETAKER;

  for (
    int i = 0;
    i < NUM_USERS;
    i++
  )
  {
    if (
      users[i].userId
      ==
      userId
    )
    {
      return
        users[i].name;
    }
  }


  return "UNKNOWN";
}


// =====================================================
// PRINT CONFIG
// =====================================================

void printConfiguration()
{
  Serial.println();

  Serial.println(
    "========== USERS =========="
  );


  for (
    int i = 0;
    i < NUM_USERS;
    i++
  )
  {
    Serial.printf(
      "User %d | ID=%d | Name=%s | Voice=%d\n",
      i,
      users[i].userId,
      users[i].name,
      users[i].reminderTrack
    );
  }


  Serial.println();

  Serial.println(
    "========== DOSE SLOTS =========="
  );


  for (
    int i = 0;
    i < NUM_SLOTS;
    i++
  )
  {
    char timeText[16];


    formatTime12(
      slots[i].hour,
      slots[i].minute,
      timeText,
      sizeof(timeText)
    );


    Serial.printf(
      "Slot %d | %s | %s | C%d | %s | Enabled=%s\n",
      i + 1,
      getUserName(
        slots[i].userId
      ),
      timeText,
      slots[i].compartment,
      slots[i].medName,
      slots[i].enabled
        ?
        "YES"
        :
        "NO"
    );
  }


  Serial.println(
    "================================"
  );
}


// =====================================================
// DEBUG COMMANDS
// =====================================================

void checkDebugSerial()
{
  if (
    !Serial.available()
  )
  {
    return;
  }


  char c =
    Serial.read();


  if (
    c == '\r'
    ||
    c == '\n'
  )
  {
    return;
  }


  if (
    c == 'P'
    ||
    c == 'p'
  )
  {
    printConfiguration();

    return;
  }

  // Audio test. Type 1-9 to play that track, 0 for track 10.
  // Quickest way to tell whether the voice files are actually
  // where the firmware thinks they are on the SD card.
  if (
    c >= '0'
    &&
    c <= '9'
  )
  {
    uint16_t t =
      (c == '0') ? 10 : (uint16_t)(c - '0');

    Serial.print(
      "[AUDIO] test: playing track "
    );
    Serial.print(t);
    Serial.print(
      " from "
    );
    Serial.println(
      mp3UseFolder ? "/MP3/ folder" : "card root"
    );

    safePlayTrack(t);
    return;
  }

  // Force the e-paper to redraw the idle screen, bypassing the
  // "already showing this" guard.
  if (
    c == 'E'
    ||
    c == 'e'
  )
  {
    epdLastSignature = "";
    // Scrub the panel, then redraw. Repainting alone cannot shift
    // ghosting - the pixels have to be driven hard both ways.
    epdDeepClean();

    epdLastClockText = "";
    epdLastVolText   = "";

    epdIdle();

    Serial.println(
      "[EPD] panel cleaned and screen redrawn"
    );
    return;
  }

  // Toggle where the firmware looks for the voice files, and
  // remember the choice. Lets you find the right one without
  // re-uploading: press F, then press 1 to test.
  if (
    c == 'F'
    ||
    c == 'f'
  )
  {
    mp3UseFolder = !mp3UseFolder;

    preferences.begin("smrmp3", false);
    preferences.putBool("useFolder", mp3UseFolder);
    preferences.end();

    Serial.print(
      "[AUDIO] now reading tracks from: "
    );
    Serial.println(
      mp3UseFolder ? "/MP3/ folder" : "card root"
    );
    Serial.println(
      "[AUDIO] press 1 to test track 0001"
    );
    return;
  }

  // Reformat the event-log filesystem. Needed when FFat mounts but
  // reports impossible geometry, which means the FAT structures are
  // corrupt even though the mount "succeeded".
  // Destroys the event history, so it asks first.
  if (
    c == 'W'
    ||
    c == 'w'
  )
  {
    Serial.println();
    Serial.println(
      "[LOG] This ERASES the whole event history."
    );
    Serial.println(
      "[LOG] Press Y within 10 seconds to confirm."
    );

    unsigned long askedAt = millis();
    bool confirmed = false;

    while (millis() - askedAt < 10000)
    {
      esp_task_wdt_reset();

      if (Serial.available())
      {
        char k = Serial.read();
        if (k == 'Y')
        {
          confirmed = true;
          break;
        }
        if (k != '\r' && k != '\n')
        {
          break;
        }
      }
      delay(50);
    }

    if (!confirmed)
    {
      Serial.println(
        "[LOG] Cancelled - nothing was erased."
      );
      return;
    }

    Serial.print(
      "[LOG] Formatting FFat... "
    );

    FFat.end();

    if (FFat.format())
    {
      Serial.println("OK");
    }
    else
    {
      Serial.println("FAILED");
    }

    if (FFat.begin(true))
    {
      Serial.print(
        "[LOG] Remounted. Total: "
      );
      Serial.print((uint32_t)FFat.totalBytes());
      Serial.print(
        " bytes   Used: "
      );
      Serial.println((uint32_t)FFat.usedBytes());

      logStorageOK = true;
      setupEventLog();
    }
    else
    {
      Serial.println(
        "[LOG] Remount FAILED"
      );
      logStorageOK = false;
    }

    return;
  }

  // Turn battery power saving on or off, and remember it.
  //
  // With it ON the device light-sleeps between doses, waking on the
  // RTC alarm, the BACK button, or a 15 s timer. That also drops the
  // USB serial connection, which is why it ships OFF - turn it on
  // once the device is running on battery rather than on the bench.
  if (
    c == 'S'
    ||
    c == 's'
  )
  {
    powerSaveEnabled = !powerSaveEnabled;

    preferences.begin("smrpwr", false);
    preferences.putBool("save", powerSaveEnabled);
    preferences.end();

    Serial.println();
    Serial.print(
      "[PWR] power saving is now "
    );
    Serial.println(
      powerSaveEnabled ? "ON" : "OFF"
    );

    if (powerSaveEnabled)
    {
      Serial.println(
        "[PWR] the device will sleep between doses."
      );
      Serial.println(
        "[PWR] serial may go quiet - that is normal."
      );
      Serial.println(
        "[PWR] hold BACK 3s on the device to return to full mode."
      );

      // Switch the screen to the clock face immediately so the
      // change is visible rather than theoretical.
      epdLastSignature = "";
      lastClockDraw = 0;
      epdClockOnly(true);
    }
    else
    {
      Serial.println(
        "[PWR] the device will stay fully awake."
      );
    }

    noteInputActivity();
    return;
  }


  // Layered audio diagnosis.
  //
  // Worth knowing: setupMP3() calls begin(stream, isACK=false), which
  // does NOT wait for a reply from the module. So "[MP3] OK" at boot
  // succeeds even with the module unplugged - it proves nothing.
  //
  // These queries DO require a reply, so they tell us which layer is
  // actually broken: the serial link, or the SD card behind it.
  if (
    c == 'C'
    ||
    c == 'c'
  )
  {
    Serial.println();
    Serial.println(
      "---------- AUDIO DIAGNOSIS ----------"
    );

    // --- Layer 1: does the module answer at all? ---
    Serial.print("1. Module replies?   ");

    int vol = -1;
    for (int i = 0; i < 3 && vol < 0; i++)
    {
      vol = player.readVolume();
      if (vol < 0) delay(400);
    }

    bool moduleTalks = (vol >= 0 && vol <= 30);

    if (moduleTalks)
    {
      Serial.print("YES  (volume reads back as ");
      Serial.print(vol);
      Serial.println(")");
    }
    else
    {
      Serial.println("NO REPLY");
    }

    // --- Layer 2: can it see the card? ---
    Serial.print("2. Card readable?    ");

    int files = -1;
    for (int i = 0; i < 3 && files < 0; i++)
    {
      files = player.readFileCounts();
      if (files < 0) delay(400);
    }

    if (files > 0)
    {
      Serial.print("YES  (");
      Serial.print(files);
      Serial.println(" files)");
    }
    else
    {
      Serial.println("NO");
    }

    // --- Layer 3: folders ---
    Serial.print("3. Folders on card:  ");
    int folders = player.readFolderCounts();
    if (folders >= 0) Serial.println(folders);
    else Serial.println("no reply");

    Serial.println(
      "-------------------------------------"
    );

    // --- Verdict ---
    Serial.println();

    if (!moduleTalks)
    {
      Serial.println(
        "VERDICT: the ESP32 cannot talk to the DFPlayer at all."
      );
      Serial.println(
        "This is WIRING or POWER, not the SD card."
      );
      Serial.println();
      Serial.println(
        "  a) DFPlayer VCC must come from the BUCK 5V output."
      );
      Serial.println(
        "     The ESP32 board's 5V pin cannot supply it."
      );
      Serial.println(
        "  b) GPIO17 -> module RX  (through the 1k resistor)"
      );
      Serial.println(
        "     GPIO18 <- module TX  (direct)"
      );
      Serial.println(
        "     If unsure, swap these two - it is the usual mistake."
      );
      Serial.println(
        "  c) Module GND must join the same ground as the ESP32."
      );
      Serial.println(
        "  d) Measure VCC on the module: it must be 4.5-5.2 V."
      );
    }
    else if (files <= 0)
    {
      Serial.println(
        "VERDICT: the module is alive, but cannot read the SD card."
      );
      Serial.println(
        "Wiring and power are FINE. The problem is the card."
      );
      Serial.println();
      Serial.println(
        "  a) Push the card in until it clicks."
      );
      Serial.println(
        "  b) Format it FAT32 - NOT exFAT."
      );
      Serial.println(
        "     Cards over 32 GB default to exFAT, which it cannot read."
      );
      Serial.println(
        "  c) Use a card of 32 GB or smaller."
      );
    }
    else
    {
      Serial.println(
        "VERDICT: module and card are both fine."
      );
      Serial.println(
        "If there is still no sound, it is the SPEAKER:"
      );
      Serial.println(
        "  - speaker goes to SPK1 and SPK2"
      );
      Serial.println(
        "  - NEITHER speaker wire may touch GND (it is a bridged output)"
      );
      Serial.println(
        "  - use a 4-8 ohm speaker"
      );
      Serial.println(
        "  - press 1 to play a track and listen"
      );
    }

    Serial.println();
    return;
  }







  if (
    c == 'N'
    ||
    c == 'n'
  )
  {
    printNextDose();

    return;
  }


  if (
    c == 'L'
    ||
    c == 'l'
  )
  {
    printEventLog();

    return;
  }


  if (
    reminderState !=
    REMINDER_IDLE
  )
  {
    Serial.println(
      "[DEBUG] Config changes blocked during reminder."
    );

    return;
  }


  if (
    c == 'R'
    ||
    c == 'r'
  )
  {
    if (
      loadConfiguration()
    )
    {
      Serial.println(
        "[DEBUG] NVS reload OK"
      );


      resetSchedulerTriggerMemory();


      menuState =
        MENU_BROWSE;


      printConfiguration();

      showMenu();

      printNextDose();
    }


    else
    {
      Serial.println(
        "[DEBUG] NVS reload FAILED"
      );
    }


    return;
  }


  if (
    c == 'D'
    ||
    c == 'd'
  )
  {
    loadDefaultConfiguration();


    if (
      saveConfiguration()
    )
    {
      Serial.println(
        "[DEBUG] Defaults restored."
      );


      resetSchedulerTriggerMemory();


      selectedSlot =
        0;


      menuState =
        MENU_BROWSE;


      printConfiguration();

      showMenu();

      printNextDose();
    }


    return;
  }
}


// =====================================================
// RTC PERIODIC PRINT
// =====================================================

void printRTCTimePeriodically()
{
  if (!rtcOK)
  {
    return;
  }


  if (
    millis()
    -
    lastRTCPrint
    <
    RTC_PRINT_INTERVAL
  )
  {
    return;
  }


  lastRTCPrint =
    millis();


  DateTime now =
    rtc.now();


  char timeText[20];


  formatTime12WithSeconds(
    now.hour(),
    now.minute(),
    now.second(),
    timeText,
    sizeof(timeText)
  );


  Serial.printf(
    "[RTC] %04d-%02d-%02d %s\n",
    now.year(),
    now.month(),
    now.day(),
    timeText
  );
}


// =====================================================
// STATUS
// =====================================================

void printSystemStatus()
{
  Serial.println();

  Serial.println(
    "------------- STATUS -------------"
  );


  Serial.printf(
    "RTC          : %s\n",
    rtcOK
      ?
      "OK"
      :
      "FAIL"
  );


  Serial.printf(
    "MP3          : %s\n",
    mp3OK
      ?
      "OK"
      :
      "FAIL"
  );


  Serial.printf(
    "Fingerprint  : %s\n",
    fingerprintOK
      ?
      "OK"
      :
      "FAIL"
  );


  Serial.printf(
    "Config/NVS   : %s\n",
    configOK
      ?
      "OK"
      :
      "FAIL"
  );


  Serial.printf(
    "Event Log    : %s\n",
    logStorageOK
      ?
      "FFat OK"
      :
      "FAIL"
  );


  Serial.println(
    "Scheduler    : ACTIVE"
  );


  Serial.println(
    "Lock pulse   : 400 ms"
  );


  Serial.println(
    "Confirmation : 120 sec"
  );


  Serial.println(
    "Auth timeout : 10 min"
  );


  Serial.println(
    "Reed sensors : Deferred to 7B"
  );

  Serial.print(
    "Power saving : "
  );
  Serial.println(
    powerSaveEnabled ? "ON" : "OFF"
  );


  Serial.println(
    "----------------------------------"
  );
}
