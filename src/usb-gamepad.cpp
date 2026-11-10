#include <Arduino.h>
#include "usb-gamepad.h"
#include "EspUsbHost.h"
#include "esp_timer.h"

static EspUsbHost usb;
volatile uint8_t gPadState = 0;
static uint8_t lastLogged = 0;

// HID usage pages / usages
constexpr uint16_t PAGE_DESKTOP = 0x01, PAGE_BUTTON = 0x09;
constexpr uint16_t U_X = 0x30, U_Y = 0x31, U_HAT = 0x39;
constexpr uint16_t U_DPAD_UP = 0x90, U_DPAD_DOWN = 0x91, U_DPAD_RIGHT = 0x92, U_DPAD_LEFT = 0x93;

// Button n (1-based) -> NES button. Typical layouts: 1/3 = bottom/left face,
// 2/4 = right/top face, 5/6 shoulders, 7/8 back/start.
static uint8_t buttonMap(uint16_t n) {
  switch (n) {
    case 1: case 3: case 5: return PAD_B;
    case 2: case 4: case 6: return PAD_A;
    case 7: case 9:         return PAD_SELECT;
    case 8: case 10:        return PAD_START;
  }
  return 0;
}

static uint8_t hatMap(int v) {   // 0 = N, clockwise to 7 = NW; else centred
  static const uint8_t m[8] = {
    PAD_UP, PAD_UP | PAD_RIGHT, PAD_RIGHT, PAD_DOWN | PAD_RIGHT,
    PAD_DOWN, PAD_DOWN | PAD_LEFT, PAD_LEFT, PAD_UP | PAD_LEFT };
  return (v >= 0 && v < 8) ? m[v] : 0;
}

// ---- HID report descriptor -> input field table --------------------------
// EspUsbHost's onGamepad only fires for reports with ID 0x03, so a pad without
// report IDs (or with other IDs) never reaches it. Parse the descriptor
// ourselves and decode the raw reports from onHIDInput instead.
struct Field {
  uint8_t iface, reportId;
  uint16_t page, usage, bitOffset;
  uint8_t bitSize;
  int32_t lmin, lmax;
};
constexpr int MAX_FIELDS = 64;
static Field fields[MAX_FIELDS];
static volatile int fieldCount = 0;
static bool ifaceHasIds[8];

constexpr uint8_t PAD_DIRS = PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT;

#ifdef PAD_FILTER_DOWN_DIAGONALS
// Some games (NES Tetris) cancel soft drop on a Down+Left/Right frame and
// ignore the Down that follows until it is pressed afresh. So a Down diagonal
// is sent as its sideways half only, and when it starts from plain Down the
// diagonal is held back for DIAG_DELAY_MS: a quick roll through it (Down ->
// Down+Right -> Down) then sends nothing extra, and Down is never interrupted.
constexpr uint32_t DIAG_DELAY_MS = 30;
static uint8_t prevDirs = 0;
static volatile bool waiting = false;
static volatile uint8_t pendingState = 0;
static esp_timer_handle_t diagTimer = nullptr;

static uint8_t applyDiagFilter(uint8_t r) {
  bool diag = (r & PAD_DOWN) && (r & (PAD_LEFT | PAD_RIGHT));
  uint8_t out = r;
  if (diag) {
    if (waiting || (prevDirs == PAD_DOWN && diagTimer)) {
      out = (r & ~PAD_DIRS) | PAD_DOWN;          // keep sending plain Down for now
      pendingState = r & ~PAD_DOWN;               // what to send if it is still held
      if (!waiting) {
        waiting = true;
        esp_timer_start_once(diagTimer, DIAG_DELAY_MS * 1000ULL);
      }
    } else {
      out = r & ~PAD_DOWN;                        // already moving sideways: sideways only
    }
  } else if (waiting) {
    if (diagTimer) esp_timer_stop(diagTimer);
    waiting = false;
  }
  prevDirs = r & PAD_DIRS;
  return out;
}
#endif

static void parseDescriptor(uint8_t iface, const uint8_t *d, size_t n) {
  // drop this interface's old fields
  int k = 0;
  for (int i = 0; i < fieldCount; i++) if (fields[i].iface != iface) fields[k++] = fields[i];
  fieldCount = k;

  uint16_t page = 0; int32_t lmin = 0, lmax = 0; uint32_t size = 0, count = 0; uint8_t rid = 0;
  uint16_t usages[32]; int nUsages = 0; uint32_t umin = 0, umax = 0; bool hasRange = false;
  uint16_t offs[256] = {};   // running bit offset per report ID
  bool ids = false;
  for (size_t i = 0; i < n;) {
    uint8_t b = d[i];
    if (b == 0xFE) { i += 3 + (i + 1 < n ? d[i + 1] : 0); continue; }  // long item
    uint8_t sz = b & 3; if (sz == 3) sz = 4;
    uint8_t type = (b >> 2) & 3, tag = b >> 4;
    if (i + 1 + sz > n) break;
    uint32_t u = 0;
    for (uint8_t j = 0; j < sz; j++) u |= (uint32_t)d[i + 1 + j] << (8 * j);
    int32_t sv = sz == 0 ? 0 : (sz == 1 ? (int8_t)u : sz == 2 ? (int16_t)u : (int32_t)u);
    i += 1 + sz;
    if (type == 1) {            // global
      switch (tag) {
        case 0: page = u; break;
        case 1: lmin = sv; break;
        case 2: lmax = sv; break;
        case 7: size = u; break;
        case 8: rid = u; ids = true; break;
        case 9: count = u; break;
      }
    } else if (type == 2) {     // local
      switch (tag) {
        case 0: if (nUsages < 32) usages[nUsages++] = u; break;
        case 1: umin = u; hasRange = true; break;
        case 2: umax = u; break;
      }
    } else if (type == 0) {     // main
      if (tag == 8) {           // Input
        bool constant = u & 1;
        for (uint32_t c = 0; c < count; c++) {
          if (!constant && fieldCount < MAX_FIELDS && size <= 32) {
            Field &f = fields[fieldCount];
            f.iface = iface; f.reportId = rid; f.page = page; f.bitOffset = offs[rid];
            f.bitSize = size; f.lmin = lmin; f.lmax = lmax;
            if (hasRange) f.usage = umin + c < umax ? umin + c : umax;
            else if (nUsages) f.usage = usages[c < (uint32_t)nUsages ? c : nUsages - 1];
            else f.usage = 0;
            fieldCount = fieldCount + 1;
          }
          offs[rid] += size;
        }
      }
      if (tag == 8 || tag == 9 || tag == 11 || tag == 10) { nUsages = 0; hasRange = false; umin = umax = 0; }
    }
  }
  if (iface < 8) ifaceHasIds[iface] = ids;
  Serial.printf("[pad] descriptor if%u: %u bytes, %d input fields%s\n", iface, (unsigned)n,
                (int)fieldCount, ids ? ", report IDs" : "");
}

static uint32_t getBits(const uint8_t *p, size_t len, uint16_t off, uint8_t size) {
  uint32_t v = 0;
  for (uint8_t i = 0; i < size; i++) {
    size_t bit = off + i;
    if ((bit >> 3) < len && (p[bit >> 3] >> (bit & 7)) & 1) v |= 1u << i;
  }
  return v;
}

static void log(uint8_t s) {
  if (s == lastLogged) return;
  lastLogged = s;
  Serial.printf("[pad] %02X:", s);
  if (!s) Serial.print(" -");
  if (s & PAD_UP)     Serial.print(" UP");
  if (s & PAD_DOWN)   Serial.print(" DOWN");
  if (s & PAD_LEFT)   Serial.print(" LEFT");
  if (s & PAD_RIGHT)  Serial.print(" RIGHT");
  if (s & PAD_B)      Serial.print(" B");
  if (s & PAD_A)      Serial.print(" A");
  if (s & PAD_SELECT) Serial.print(" SELECT");
  if (s & PAD_START)  Serial.print(" START");
  Serial.println();
}

#ifdef HID_RAW_DEBUG
static uint8_t lastRaw[32];
static size_t lastRawLen = 0;
constexpr size_t RAW_CMP = 8;
#endif

static void onInput(const EspUsbHostHIDInput &in) {
  const uint8_t *d = in.data; size_t n = in.length;
  if (!d || !n) return;
#ifdef HID_RAW_DEBUG
  // Raw debug dump when the first bytes (buttons, hat, sticks) change; later
  // bytes on some pads are counters/sensors that change on every report.
  size_t cmp = n < RAW_CMP ? n : RAW_CMP;
  if (n != lastRawLen || memcmp(d, lastRaw, cmp)) {
    lastRawLen = n; memcpy(lastRaw, d, cmp);
    Serial.printf("[hid] if%u raw:", in.interfaceNumber);
    for (size_t i = 0; i < n && i < 32; i++) Serial.printf(" %02X", d[i]);
    Serial.println();
  }
#endif
  bool ids = in.interfaceNumber < 8 && ifaceHasIds[in.interfaceNumber];
  uint8_t rid = ids ? d[0] : 0;
  const uint8_t *p = ids ? d + 1 : d;
  size_t len = ids ? n - 1 : n;
  uint8_t s = 0;
  for (int i = 0; i < fieldCount; i++) {
    const Field &f = fields[i];
    if (f.iface != in.interfaceNumber || f.reportId != rid) continue;
    uint32_t raw = getBits(p, len, f.bitOffset, f.bitSize);
    int32_t v = (int32_t)raw;
    if (f.lmin < 0 && f.bitSize < 32 && (raw >> (f.bitSize - 1)) & 1) v = (int32_t)(raw | (~0u << f.bitSize));
    int32_t span = f.lmax - f.lmin;
    if (f.page == PAGE_BUTTON) {
      if (v) s |= buttonMap(f.usage);
    } else if (f.page == PAGE_DESKTOP) {
      switch (f.usage) {
        case U_HAT:        s |= hatMap(v - f.lmin); break;
        case U_DPAD_UP:    if (v) s |= PAD_UP; break;
        case U_DPAD_DOWN:  if (v) s |= PAD_DOWN; break;
        case U_DPAD_LEFT:  if (v) s |= PAD_LEFT; break;
        case U_DPAD_RIGHT: if (v) s |= PAD_RIGHT; break;
        case U_X: case U_Y: {
          if (span < 2) break;
          int32_t pc = (int32_t)(((int64_t)(v - f.lmin) * 100) / span);
          uint8_t lo = f.usage == U_X ? PAD_LEFT : PAD_UP;
          uint8_t hi = f.usage == U_X ? PAD_RIGHT : PAD_DOWN;
          if (pc < 25) s |= lo; else if (pc > 75) s |= hi;
          break;
        }
      }
    }
  }
#ifdef PAD_FILTER_DOWN_DIAGONALS
  s = applyDiagFilter(s);
#endif
  gPadState = s;
  log(s);
}

void gamepadBegin() {
#ifdef PAD_FILTER_DOWN_DIAGONALS
  esp_timer_create_args_t ta = {};
  ta.callback = [](void *) {
    if (!waiting) return;
    waiting = false;
    gPadState = pendingState;
    Serial.printf("[pad] diagonal held %ums: %02X\n", (unsigned)DIAG_DELAY_MS, (unsigned)pendingState);
  };
  ta.name = "diag";
  esp_timer_create(&ta, &diagTimer);
#endif
  usb.onDeviceConnected([](const EspUsbHostDeviceInfo &d) {
    Serial.print("[usb] connected: ");
    espUsbHostPrint(d);
  });
  usb.onDeviceDisconnected([](const EspUsbHostDeviceInfo &d) {
    Serial.print("[usb] disconnected: ");
    espUsbHostPrint(d);
    gPadState = 0;
    fieldCount = 0;
  });
  usb.onHIDReportDescriptor([](const EspUsbHostHIDReportDescriptor &r) {
    parseDescriptor(r.interfaceNumber, r.data, r.length);
  });
  usb.onHIDInput(onInput);
  EspUsbHostConfig cfg;
  cfg.taskCore = 0;
  Serial.println(
#ifdef PAD_FILTER_DOWN_DIAGONALS
    "[pad] down-diagonal filter: ON"
#else
    "[pad] down-diagonal filter: OFF"
#endif
  );
  if (usb.begin(cfg))
    Serial.println("[usb] host started, waiting for a gamepad on the OTG port");
  else
    Serial.printf("[usb] host start failed: %s\n", usb.lastErrorName());
}

