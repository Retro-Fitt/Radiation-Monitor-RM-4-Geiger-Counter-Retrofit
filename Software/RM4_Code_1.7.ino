#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <LittleFS.h>

// ─── Hardware ─────────────────────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  32
#define OLED_RESET     -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
#define GEIGER_PIN  2

// ─── Battery ADC ──────────────────────────────────────────────────────────────
#define BAT_PIN          26
#define BAT_R1       100000.0f
#define BAT_R2       100000.0f
#define BAT_VMIN          3.7f
#define BAT_VMAX          4.2f
#define USB_THRESHOLD     4.35f
#define BAT_CAL_FACTOR    1.052f   // Corrects the measured deviation — adjust if necessary
#define BAT_LOW_PCT       15       // Shut down below this percentage
#define BAT_CHECK_MS  1800000UL   // Periodic check every 30 minutes

// ─── Parameters ──────────────────────────────────────────────────────────────
#define WINDOW_MS       60000UL
#define FAST_WINDOW_MS  15000UL
#define UPDATE_MS         500UL
#define BUFFER_SIZE        512
#define MIN_GAP_US         500UL
#define SYMBOL_MS          500UL
#define LOG_INTERVAL_MS  60000UL
#define LOG_MAX_BYTES   500000UL

const float CPM_TO_MRHR = 1.0f / 330.0f;  // PLACEHOLDER — must be calibrated

// ─── 16×16 Radiation symbol ───────────────────────────────────────────────────
static const uint8_t PROGMEM radBmp[] = {
  0x00, 0x00,
  0x0F, 0xF0,
  0x07, 0xE0,
  0x07, 0xE0,
  0x03, 0xC0,
  0x01, 0x80,
  0x00, 0x00,
  0x01, 0x80,
  0x7D, 0xBE,
  0x7C, 0x3E,
  0x7E, 0x7E,
  0x7C, 0x3E,
  0x38, 0x1C,
  0x18, 0x18,
  0x00, 0x00,
  0x00, 0x00,
};

// ─── Ring buffer ──────────────────────────────────────────────────────────────
volatile unsigned long timestamps[BUFFER_SIZE];
volatile uint16_t      bufHead     = 0;
volatile uint32_t      totalPulses = 0;
volatile unsigned long lastPulseUs = 0;
volatile unsigned long lastPulseMs = 0;

unsigned long bootMs        = 0;
unsigned long lastUpdateMs  = 0;
unsigned long lastLogMs     = 0;
unsigned long lastBatCheckMs = 0;

// ─── ISR ──────────────────────────────────────────────────────────────────────
void geigerISR() {
  unsigned long nowUs = micros();
  if (nowUs - lastPulseUs < MIN_GAP_US) return;
  lastPulseUs = nowUs;
  unsigned long nowMs = millis();
  lastPulseMs = nowMs;
  timestamps[bufHead] = nowMs;
  bufHead = (bufHead + 1) % BUFFER_SIZE;
  totalPulses++;
}

// ─── CPM ──────────────────────────────────────────────────────────────────────
void calcCPMs(unsigned long nowMs, float &cpmAvg, float &cpmFast) {
  uint32_t cntAvg = 0, cntFast = 0;
  noInterrupts();
  for (uint16_t i = 0; i < BUFFER_SIZE; i++) {
    unsigned long t = timestamps[i];
    if (t == 0) continue;
    unsigned long age = nowMs - t;
    if (age <= WINDOW_MS) {
      cntAvg++;
      if (age <= FAST_WINDOW_MS) cntFast++;
    }
  }
  interrupts();
  unsigned long effAvg  = min(WINDOW_MS,      nowMs - bootMs);
  unsigned long effFast = min(FAST_WINDOW_MS, nowMs - bootMs);
  if (effAvg  < 1000) effAvg  = 1000;
  if (effFast < 1000) effFast = 1000;
  cpmAvg  = (float)cntAvg  * (60000.0f / effAvg);
  cpmFast = (float)cntFast * (60000.0f / effFast);
}

// ─── Battery reading ──────────────────────────────────────────────────────────
float readBatVoltage() {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(BAT_PIN);
  float vout = (sum / 8.0f) * (3.3f / 4095.0f);
  return vout * (BAT_R1 + BAT_R2) / BAT_R2 * BAT_CAL_FACTOR;
}

int batPercent(float vbat) {
  return (int)constrain(
    (vbat - BAT_VMIN) / (BAT_VMAX - BAT_VMIN) * 100.0f,
    0.0f, 100.0f
  );
}

void drawBattery(int x, int y, int pct) {
  display.drawRect(x, y, 13, 6, SSD1306_WHITE);
  display.fillRect(x + 13, y + 2, 2, 2, SSD1306_WHITE);
  int fill = (pct * 11) / 100;
  if (fill > 0)
    display.fillRect(x + 1, y + 1, fill, 4, SSD1306_WHITE);
}

// ─── Low battery: warn and shut down ──────────────────────────────────────────
//
//  1. Display LOW BATTERY warning (3 seconds)
//  2. Turn off the display (power saving)
//  3. Keep the CPU in a __wfi() loop → it will not wake until the switch
//     is turned off and on
//     (RP2040 WFI: ~1–2mA, the most practical method within the Arduino layer
//     instead of full dormant mode)
//
void lowBatteryShutdown() {
  detachInterrupt(digitalPinToInterrupt(GEIGER_PIN));  // Stop the ISR

  // Warning screen
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(5, 2);
  display.print("LOW BAT!");
  display.setTextSize(1);
  display.setCursor(10, 22);
  display.print("Sarj edin / Charge");
  display.display();

  Serial.println("[SHUTDOWN] Pil dusuk! Kapaniyor.");

  delay(3000);

  // Turn off the display
  display.ssd1306_command(SSD1306_DISPLAYOFF);

  // Keep the CPU waiting — execution will not leave this loop until
  // the switch is turned off and on
  while (true) {
    __wfi();  // ARM Wait-For-Interrupt: minimum power consumption
  }
}

// ─── Battery status check ─────────────────────────────────────────────────────
// Do not check while on USB (it may already be charging)
void checkBattery() {
  float vbat = readBatVoltage();
  if (vbat > USB_THRESHOLD) return;   // USB power supply → skip
  int pct = batPercent(vbat);
  Serial.print("[BAT CHECK] ");
  Serial.print(vbat, 2); Serial.print("V ");
  Serial.print(pct); Serial.println("%");
  if (pct < BAT_LOW_PCT) lowBatteryShutdown();
}

// ─── LittleFS log ─────────────────────────────────────────────────────────────
void initLog() {
  if (!LittleFS.begin()) { LittleFS.format(); LittleFS.begin(); }
  if (!LittleFS.exists("/rm4log.csv")) {
    File f = LittleFS.open("/rm4log.csv", "w");
    if (f) { f.println("uptime_s,cpm_avg,cpm_fast,mrhr,total,vbat"); f.close(); }
  }
}

void logData(unsigned long uptimeS, float cpmAvg, float cpmFast, float mrhr, float vbat) {
  File chk = LittleFS.open("/rm4log.csv", "r");
  bool full = chk && (chk.size() >= LOG_MAX_BYTES);
  if (chk) chk.close();
  if (full) { Serial.println("[LOG] 500KB doldu. 'd' ile sil."); return; }
  File f = LittleFS.open("/rm4log.csv", "a");
  if (!f) { Serial.println("[LOG] Dosya acilamadi!"); return; }
  f.print(uptimeS);     f.print(",");
  f.print(cpmAvg, 1);   f.print(",");
  f.print(cpmFast, 1);  f.print(",");
  f.print(mrhr, 4);     f.print(",");
  f.print(totalPulses); f.print(",");
  f.println(vbat, 2);
  f.close();
}

void dumpLog() {
  File f = LittleFS.open("/rm4log.csv", "r");
  if (!f) { Serial.println("[LOG] Dosya yok."); return; }
  Serial.println("=== RM-4 LOG BASLANGIC ===");
  uint8_t buf[64];
  while (int n = f.read(buf, sizeof(buf))) Serial.write(buf, n);
  f.close();
  Serial.println("\n=== RM-4 LOG BITIS ===");
}

// ─── Display ──────────────────────────────────────────────────────────────────
void updateDisplay(float cpmAvg, float cpmFast, float mrhr, bool showSym,
                   float vbat, int batPct, bool onUSB) {
  display.clearDisplay();

  // Line 1: counter + voltage + battery/USB
  display.setTextSize(1);
  display.setCursor(2, 1);
  display.print("T:");
  display.print(totalPulses);

  display.setCursor(46, 1);
  display.print(vbat, 2);
  display.print("V");

  if (onUSB) {
    display.setCursor(82, 1);
    display.print("USB");
  } else {
    drawBattery(78, 1, batPct);
    display.setCursor(96, 1);
    display.print(batPct);
    display.print("%");
  }

  // Line 2: main CPM + trefoil (in the empty space on the right)
  display.setTextSize(2);
  display.setCursor(2, 9);
  display.print("CPM:");
  display.print((uint32_t)cpmAvg);

  if (showSym)
    display.drawBitmap(110, 8, radBmp, 16, 16, SSD1306_WHITE);

  // Line 3: instantaneous + dose rate
  display.setTextSize(1);
  display.setCursor(2, 25);
  display.print("15s:");
  display.print((uint32_t)cpmFast);
  display.print("  ");
  display.print(mrhr, 3);
  display.print("mR/h");

  display.display();
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Wire.setSDA(0); Wire.setSCL(1);
  Wire.begin(); Wire.setClock(400000);

  analogReadResolution(12);
  pinMode(BAT_PIN, INPUT);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 bulunamadi!"); for (;;);
  }

  // ── Startup screen (bitmap on the left, text on the right — no overlap) ────
  //
  //  [☢]  HEATHKIT RM-4      bitmap: x=2, y=8 (16x16 → y=8..23)
  //       Digital v1.7       text1:   x=20, y=8
  //                          text2:   x=20, y=18
  //
  display.clearDisplay();
  display.drawBitmap(2, 8, radBmp, 16, 16, SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(20, 8);  display.print("HEATHKIT RM-4");
  display.setCursor(20, 18); display.print("Digital v1.7");
  display.display();
  delay(2000);

  // ── Startup battery check (2 seconds after the splash screen finishes) ─────
  checkBattery();
  lastBatCheckMs = millis();

  initLog();
  Serial.println("Komutlar: 'r'=log oku | 'd'=log sil");

  memset((void*)timestamps, 0, sizeof(timestamps));
  pinMode(GEIGER_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(GEIGER_PIN), geigerISR, FALLING);
  bootMs = lastUpdateMs = lastLogMs = millis();
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

if (Serial.available()) {
  char cmd = Serial.read();
  if      (cmd == 'r') dumpLog();
  else if (cmd == 'd') {
    LittleFS.remove("/rm4log.csv"); initLog();
    Serial.println("[LOG] Silindi.");
  }
  else if (cmd == 'b') {           // ← ADD THIS LINE
    Serial.println("Bootloader moduna giriliyor...");
    delay(100);                    // Short delay to allow the Serial message to be sent
    rp2040.rebootToBootloader();   // Appears as an RPI-RP2 drive
  }
}

  // Periodic battery check every 30 minutes
  if (now - lastBatCheckMs >= BAT_CHECK_MS) {
    lastBatCheckMs = now;
    checkBattery();
  }

  if (now - lastUpdateMs >= UPDATE_MS) {
    lastUpdateMs = now;

    float cpmAvg, cpmFast;
    calcCPMs(now, cpmAvg, cpmFast);
    float mrhr    = cpmAvg * CPM_TO_MRHR;
    bool  showSym = (now - lastPulseMs) < SYMBOL_MS;

    float vbat   = readBatVoltage();
    bool  onUSB  = (vbat > USB_THRESHOLD);
    int   batPct = onUSB ? -1 : batPercent(vbat);

    updateDisplay(cpmAvg, cpmFast, mrhr, showSym, vbat, batPct, onUSB);

    Serial.print("AVG:"); Serial.print(cpmAvg, 1);
    Serial.print(" 15s:"); Serial.print(cpmFast, 1);
    Serial.print(" T:"); Serial.print(totalPulses);
    Serial.print(" mR/hr:"); Serial.print(mrhr, 4);
    Serial.print(" V:"); Serial.print(vbat, 2);
    if (onUSB) Serial.println("V (USB)");
    else { Serial.print("V ("); Serial.print(batPct); Serial.println("%)"); }
  }

  if (now - lastLogMs >= LOG_INTERVAL_MS) {
    lastLogMs = now;
    float cpmAvg, cpmFast;
    calcCPMs(now, cpmAvg, cpmFast);
    float vbat = readBatVoltage();
    logData(now / 1000, cpmAvg, cpmFast, cpmAvg * CPM_TO_MRHR, vbat);
  }
}