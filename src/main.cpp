// Schrader 3142M TPMS scanner
// ESP32 DevKitC V1 + CC1101 433.92MHz + SSD1306 0.96" OLED
//
// Leest Schrader motor-TPMS sensoren (FCC ID MRXRDC3, PN 3142M/3141M/29073/29074)
// via de rtl_433_ESP port van rtl_433, decoder [196] "Schrader Motorcycle TPMS
// sensor".
//
// Zie README.md voor gebruik, WIRING.md voor bedrading en TROUBLESHOOTING.md
// als er niets binnenkomt.

#include <Arduino.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <ArduinoLog.h>
#include <U8g2lib.h>
#include <rtl_433_ESP.h>

#include "config.h"

// ---------------------------------------------------------------------------
// Build-time controles
// ---------------------------------------------------------------------------
// Deze checks vangen configuratiefouten bij het compileren op, in plaats van
// als stilte op een ontvanger die op de motor zit.

#if !defined(RF_CC1101)
#  error "RF_CC1101 niet gedefinieerd. Build via platformio.ini, niet los."
#endif

// De belangrijkste: rtl_433_ESP default is 40000us, maar een Schrader-frame
// is ~14000us en wordt dan weggegooid voor de decoder hem ziet.
#if !defined(MINIMUM_SIGNAL_LENGTH)
#  error "MINIMUM_SIGNAL_LENGTH niet gezet"
#endif
#if MINIMUM_SIGNAL_LENGTH > 14000
#  error "MINIMUM_SIGNAL_LENGTH te hoog: Schrader-frames (~14ms) worden gefilterd"
#endif

// De CC1101-datapin moet GDO0 zijn en mag niet op de I2C-pinnen van het OLED
// liggen. De rtl_433_ESP README gebruikt GPIO22, wat SCL is.
#if RF_MODULE_RECEIVER_GPIO == OLED_SDA_PIN || RF_MODULE_RECEIVER_GPIO == OLED_SCL_PIN
#  error "CC1101 datapin botst met de I2C-pinnen van het OLED"
#endif

// SERIAL_BAUD (config.h) en monitor_speed (platformio.ini) moeten gelijk zijn.
// Lopen ze uiteen, dan start de scanner gewoon op maar zie je in de monitor
// alleen onleesbare tekens; dat lijkt op een stukke ESP32 terwijl het alleen
// een baudrate-verschil is. MONITOR_SPEED wordt als build flag doorgegeven.
#if defined(MONITOR_SPEED) && (MONITOR_SPEED != SERIAL_BAUD)
#  error "SERIAL_BAUD in config.h wijkt af van monitor_speed in platformio.ini"
#endif

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

rtl_433_ESP rf;

// Clock- en datapin expliciet meegeven. Zonder die argumenten roept U8g2 een
// kale Wire.begin() aan en zijn we afhankelijk van de volgorde t.o.v. onze
// eigen Wire.begin(). Dat werkt alleen zolang de pinnen de ESP32-defaults zijn.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE,
                                            OLED_SCL_PIN, OLED_SDA_PIN);

// Of het scherm daadwerkelijk antwoordt. U8G2::begin() geeft op ESP32 altijd 1
// terug, dus dat is geen bruikbare test; we moeten zelf op de bus kijken.
static bool displayPresent = false;

// Buffer die rtl_433_ESP gebruikt om het JSON-bericht in te bouwen.
char messageBuffer[JSON_MSG_BUFFER];

// Eén meting per sensor-ID.
struct SensorReading {
  uint32_t id;
  float pressure_kPa;
  float temperature_C;
  int rssi;
  uint32_t lastSeenMs;
  uint32_t packetCount;
  bool valid;
};

static SensorReading sensors[MAX_SENSORS];

// De callback draait in rtl_433_DecoderTask, loop() draait op de hoofdtask.
// Zonder mutex lees je half-bijgewerkte floats.
static SemaphoreHandle_t sensorMutex = nullptr;

// Statistieken, puur voor de statusregel op het OLED.
static volatile uint32_t schraderPacketCount = 0;
static volatile uint32_t otherPacketCount = 0;

// ---------------------------------------------------------------------------
// Sensor-state
// ---------------------------------------------------------------------------

// Zoekt het slot voor dit ID, of claimt er een. Geeft -1 als alles bezet is
// door recente metingen.
static int findOrClaimSlot(uint32_t id, uint32_t nowMs) {
  // Bestaand ID?
  for (int i = 0; i < MAX_SENSORS; i++) {
    if (sensors[i].valid && sensors[i].id == id) {
      return i;
    }
  }
  // Vrij slot?
  for (int i = 0; i < MAX_SENSORS; i++) {
    if (!sensors[i].valid) {
      return i;
    }
  }
  // Alles bezet: gooi de oudste eruit als die voorbij de expiry is.
  int oldest = 0;
  for (int i = 1; i < MAX_SENSORS; i++) {
    if (sensors[i].lastSeenMs < sensors[oldest].lastSeenMs) {
      oldest = i;
    }
  }
  if ((nowMs - sensors[oldest].lastSeenMs) / 1000UL > READING_EXPIRE_SECONDS) {
    return oldest;
  }
  return -1;
}

static void recordReading(uint32_t id, float kPa, float tempC, int rssi) {
  if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return;
  }
  uint32_t nowMs = millis();
  int slot = findOrClaimSlot(id, nowMs);
  if (slot >= 0) {
    bool isNew = !sensors[slot].valid || sensors[slot].id != id;
    sensors[slot].id             = id;
    sensors[slot].pressure_kPa   = kPa;
    sensors[slot].temperature_C  = tempC;
    sensors[slot].rssi           = rssi;
    sensors[slot].lastSeenMs     = nowMs;
    sensors[slot].packetCount    = isNew ? 1 : sensors[slot].packetCount + 1;
    sensors[slot].valid          = true;
  }
  xSemaphoreGive(sensorMutex);
}

// ---------------------------------------------------------------------------
// rtl_433_ESP callback
// ---------------------------------------------------------------------------

void rtl_433_Callback(char* message) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, message);
  if (err) {
    Log.error(F("JSON parse mislukt: %s" CR), err.c_str());
    return;
  }

  const char* model = doc["model"] | "";

  if (strcmp(model, SCHRADER_MODEL_NAME) != 0) {
    // Niet onze sensor. Toch loggen: het bewijst dat de ontvangstketen leeft.
    otherPacketCount++;
    Log.notice(F("[other] %s" CR), message);
    return;
  }

  schraderPacketCount++;

  uint32_t id   = doc["id"]            | 0UL;
  float kPa     = doc["pressure_kPa"]  | NAN;
  float tempC   = doc["temperature_C"] | NAN;
  int rssi      = doc["rssi"]          | 0;

  if (isnan(kPa) || isnan(tempC)) {
    Log.error(F("Schrader-pakket zonder druk/temp: %s" CR), message);
    return;
  }

  recordReading(id, kPa, tempC, rssi);

  // De 0.5 kPa/bit schaal in de rtl_433-decoder is afgeleid uit een beperkte
  // sample-set (issue #3512), niet uit een datasheet. Log de ruwe waarde mee
  // zodat je het tegen een manometer kunt narekenen.
  int rawPressure = (int)lroundf(kPa / 0.5f);

  // Via snprintf en niet via ArduinoLog's %F: die gaat door Print::print(double)
  // en kapt op 2 decimalen af. Voor het narekenen tegen een manometer wil je
  // de waarde ongeschonden zien.
  char barStr[12], kpaStr[12], tempStr[12];
  snprintf(barStr,  sizeof(barStr),  "%.3f", kPa / 100.0f);
  snprintf(kpaStr,  sizeof(kpaStr),  "%.1f", kPa);
  snprintf(tempStr, sizeof(tempStr), "%.1f", tempC);

  Log.notice(F("[SCHRADER] id=%u  %s bar (%s kPa, raw=%d)  %s C  rssi=%d" CR),
             id, barStr, kpaStr, rawPressure, tempStr, rssi);

  // Volledige JSON er ook bij, voor loggen naar bestand tijdens een testrit.
  Log.notice(F("[json] %s" CR), message);
}

// ---------------------------------------------------------------------------
// OLED
// ---------------------------------------------------------------------------

// Probeert of er op dit adres een apparaat antwoordt.
static bool i2cDeviceAt(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// --- Layout ---------------------------------------------------------------
//
// Dit is een 0.96" scherm: ~24 mm breed. Daar passen technisch zes regels
// mini-font op, maar die zijn in de hand onleesbaar, en op een motor
// helemaal. De layout toont daarom weinig, maar groot:
//
//   1 sensor : druk 34px hoog, daaronder temperatuur en leeftijd op 10x20
//   2 sensoren: twee helften van 32px, druk 24px hoog
//
// Details als rssi, pakketteller, kPa en de ruwe waarde staan alleen nog in
// de seriele log; daar kun je ze rustig lezen.
//
// Een oude meting krijgt een inverted balk. Dat valt met een blik op, terwijl
// het woord "OUD" in klein font juist wegvalt.
//
// Alle posities zijn op de host nagemeten tegen de echte U8g2-fontmetrieken,
// inclusief de randgevallen "10.25" (breedste druk) en "-12C" (breedste temp).

// Schrijft de leeftijd compact: "12s", "90s", "14m", "3u".
// In klein formaat is "840s" niet te interpreteren, "14m" wel.
static void formatAge(uint32_t ageS, char* out, size_t n) {
  if (ageS < 100) {
    snprintf(out, n, "%lus", (unsigned long)ageS);
  } else if (ageS < 3600) {
    snprintf(out, n, "%lum", (unsigned long)(ageS / 60));
  } else {
    snprintf(out, n, "%luu", (unsigned long)(ageS / 3600));
  }
}

static void drawSplash(const char* line1, const char* line2) {
  display.clearBuffer();
  display.setFont(u8g2_font_10x20_tf);
  display.drawStr(0, 20, line1);
  if (line2) {
    display.setFont(u8g2_font_6x12_tf);
    display.drawStr(0, 38, line2);
  }
  display.sendBuffer();
}

// Scherm met geen enkele sensor gezien.
static void drawWaiting() {
  display.clearBuffer();

  display.setFont(u8g2_font_10x20_tf);
  display.drawStr(0, 17, "Wachten op");
  display.drawStr(0, 36, "sensor");

  display.setFont(u8g2_font_6x12_tf);
  char buf[32];
  snprintf(buf, sizeof(buf), "433.92MHz  rssi %d", rtl_433_ESP::currentRssi);
  display.drawStr(0, 51, buf);

  // De sensor zendt alleen bij rollen of drukdaling. Zonder deze hint ga je
  // zoeken naar een storing die er niet is.
  display.drawStr(0, 62, "rijden of lucht eruit");

  display.sendBuffer();
}

// Scherm met 1 sensor: druk zo groot als past.
static void drawSingle(const SensorReading& r, uint32_t nowMs) {
  uint32_t ageS = (nowMs - r.lastSeenMs) / 1000UL;
  bool stale = ageS > READING_STALE_SECONDS;

  char bar[12], temp[12], age[12];
  snprintf(bar, sizeof(bar), "%.2f", r.pressure_kPa / 100.0f);
  snprintf(temp, sizeof(temp), "%.0fC", r.temperature_C);
  formatAge(ageS, age, sizeof(age));

  display.clearBuffer();

  // Druk, 34px hoog.
  display.setFont(u8g2_font_logisoso34_tn);
  display.drawStr(0, 37, bar);
  int barWidth = display.getStrWidth(bar);

  // "bar" rechts van het getal. Positie meebewegen met de breedte, zodat
  // "10.25" niet onder het label schuift.
  display.setFont(u8g2_font_7x13B_tf);
  int labelX = barWidth + 4;
  int labelW = display.getStrWidth("bar");
  if (labelX > OLED_WIDTH - labelW) {
    labelX = OLED_WIDTH - labelW;
  }
  display.drawStr(labelX, 16, "bar");

  // Onderste regel. Inverted als de meting oud is.
  if (stale) {
    display.drawBox(0, 43, OLED_WIDTH, 21);
    display.setDrawColor(0);
  }
  display.setFont(u8g2_font_10x20_tf);
  int x = stale ? 2 : 0;
  display.drawStr(x, 61, temp);
  display.drawStr(x + display.getStrWidth(temp) + 20, 61, age);
  display.setDrawColor(1);

  display.sendBuffer();
}

// Scherm met 2 sensoren: twee helften van 32px.
static void drawList(uint32_t nowMs) {
  display.clearBuffer();

  int shown = 0;
  for (int i = 0; i < MAX_SENSORS && shown < 2; i++) {
    if (!sensors[i].valid) continue;

    const SensorReading& r = sensors[i];
    uint32_t ageS = (nowMs - r.lastSeenMs) / 1000UL;
    bool stale = ageS > READING_STALE_SECONDS;
    int top = shown * 32;

    char bar[12], temp[12], age[12], id[12];
    snprintf(bar, sizeof(bar), "%.2f", r.pressure_kPa / 100.0f);
    snprintf(temp, sizeof(temp), "%.0fC", r.temperature_C);
    formatAge(ageS, age, sizeof(age));
    // Laatste 4 cijfers van het ID: genoeg om twee sensoren te onderscheiden
    // en het past naast de druk. Welke sensor op welk wiel zit weten we niet,
    // dus "VOOR"/"ACHTER" zou een gok zijn die er zeker uitziet.
    snprintf(id, sizeof(id), "%lu", (unsigned long)(r.id % 10000UL));

    if (stale) {
      display.drawBox(0, top, OLED_WIDTH, 31);
      display.setDrawColor(0);
    }

    display.setFont(u8g2_font_logisoso24_tn);
    display.drawStr(0, top + 25, bar);
    int barWidth = display.getStrWidth(bar);

    display.setFont(u8g2_font_6x12_tf);
    display.drawStr(barWidth + 3, top + 12, "bar");

    display.setFont(u8g2_font_7x13B_tf);
    display.drawStr(barWidth + 3, top + 26, temp);

    // ID en leeftijd rechts uitgelijnd.
    display.setFont(u8g2_font_6x12_tf);
    display.drawStr(OLED_WIDTH - display.getStrWidth(id), top + 12, id);
    display.drawStr(OLED_WIDTH - display.getStrWidth(age), top + 26, age);

    display.setDrawColor(1);
    shown++;
  }

  // Scheidingslijn tussen de twee helften.
  if (shown > 1) {
    display.drawHLine(0, 31, OLED_WIDTH);
  }

  display.sendBuffer();
}

// Zoekt het OLED op de bus en initialiseert het.
//
// Probeert eerst het geconfigureerde adres, dan het andere gangbare SSD1306-
// adres. Antwoordt geen van beide, dan scant hij de hele bus en logt wat er
// wel hangt. Dat scheelt een losse scanner-sketch flashen.
static void setupDisplay() {
  const uint8_t candidates[] = {OLED_I2C_ADDR, 0x3C, 0x3D};
  uint8_t addr = 0;

  for (uint8_t c : candidates) {
    if (i2cDeviceAt(c)) {
      addr = c;
      break;
    }
  }

  if (addr == 0) {
    Log.error(F("Geen OLED gevonden op 0x3C of 0x3D. Bus scannen..." CR));
    int found = 0;
    for (uint8_t a = 1; a < 127; a++) {
      if (i2cDeviceAt(a)) {
        Log.error(F("  I2C-apparaat op 0x%x" CR), a);
        found++;
      }
    }
    if (found == 0) {
      Log.error(F("  Niets op de bus. Check: VCC op 3V3, GND gedeeld, "
                  "SDA=GPIO%d, SCL=GPIO%d niet verwisseld." CR),
                OLED_SDA_PIN, OLED_SCL_PIN);
    } else {
      Log.error(F("  Zet het juiste adres in include/config.h bij "
                  "OLED_I2C_ADDR." CR));
    }
    // Niet fataal: de scanner blijft via serieel bruikbaar.
    displayPresent = false;
    return;
  }

  if (addr != OLED_I2C_ADDR) {
    Log.warning(F("OLED op 0x%x in plaats van de ingestelde 0x%x. Werkt, maar "
                  "pas OLED_I2C_ADDR in config.h aan." CR),
                addr, OLED_I2C_ADDR);
  } else {
    Log.notice(F("OLED gevonden op 0x%x" CR), addr);
  }

  display.setI2CAddress(addr << 1);
  display.begin();
  display.setFontMode(1);
  displayPresent = true;
  // "Schrader TPMS" is op 10x20 130px breed en zou afgekapt worden.
  drawSplash("Schrader", "TPMS  radio starten");
}

static void updateDisplay() {
  if (!displayPresent) {
    return;
  }
  if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return;
  }

  uint32_t nowMs = millis();
  int validCount = 0;
  int firstValid = -1;
  for (int i = 0; i < MAX_SENSORS; i++) {
    if (sensors[i].valid) {
      validCount++;
      if (firstValid < 0) firstValid = i;
    }
  }

  if (validCount == 0) {
    drawWaiting();
  } else if (validCount == 1) {
    drawSingle(sensors[firstValid], nowMs);
  } else {
    drawList(nowMs);
  }

  xSemaphoreGive(sensorMutex);
}

// ---------------------------------------------------------------------------
// Optionele CC1101 sweep (env: tune)
// ---------------------------------------------------------------------------
// De tuning-logica zit in het rtl_433_ESP voorbeeld, niet in de library.
// Dit is een compacte variant: loop door bandbreedte- en AGC-instellingen en
// rapporteer per venster hoeveel signalen en decodes er waren.
#ifdef CC1101_OOK_TUNING

#ifndef CC1101_TUNING_WINDOW_SECONDS
#  define CC1101_TUNING_WINDOW_SECONDS 120UL
#endif

struct TuneSetting {
  float bandwidthKHz;
  uint8_t agcctrl2;
  uint8_t agcctrl0;
};

// Breedte eerst grof, daarna de twee AGC-registers die in de rtl_433_ESP
// documentatie als meest bepalend genoemd worden.
static const TuneSetting tuneSettings[] = {
  {812.0f, 0xC7, 0xB2},  // rtl_433_ESP default
  {406.0f, 0x03, 0x91},
  {325.0f, 0x03, 0x91},
  {270.833f, 0x03, 0x91},
  {203.125f, 0x03, 0x91},
  {203.125f, 0xC7, 0x93},
  {162.5f, 0x84, 0xA0},
  {162.5f, 0x03, 0x91},
  {116.071f, 0x84, 0xA0},
};
static const size_t tuneCount = sizeof(tuneSettings) / sizeof(tuneSettings[0]);

static size_t tuneIndex = 0;
static uint32_t tuneWindowStart = 0;

static void applyTuneSetting(size_t i) {
  const TuneSetting& s = tuneSettings[i];

  rf.disableReceiver();
  int16_t bw   = rf.setRxBandwidth(s.bandwidthKHz);
  int16_t a2   = rf.setCC1101Register(RADIOLIB_CC1101_REG_AGCCTRL2, s.agcctrl2);
  int16_t a0   = rf.setCC1101Register(RADIOLIB_CC1101_REG_AGCCTRL0, s.agcctrl0);
  int16_t recv = rf.receiveDirect();
  rf.enableReceiver();

  rtl_433_ESP::decoderSignals = 0;
  rtl_433_ESP::decodedMessages = 0;
  rtl_433_ESP::zeroDecodedSignals = 0;
  schraderPacketCount = 0;
  otherPacketCount = 0;
  tuneWindowStart = millis();

  // Bandbreedte als string: ArduinoLog's %F gaat via Print::print(double) en
  // geeft maar 2 decimalen, waardoor 270.833 als 270.83 in de log komt. Juist
  // deze waarde wil je straks exact overnemen in platformio.ini.
  char bwStr[12];
  snprintf(bwStr, sizeof(bwStr), "%.3f", s.bandwidthKHz);

  Log.notice(F("TUNE_START %u/%u bw=%s agcctrl2=0x%x agcctrl0=0x%x "
               "window=%us states=%d,%d,%d,%d" CR),
             (unsigned)(i + 1), (unsigned)tuneCount,
             bwStr, s.agcctrl2, s.agcctrl0,
             (unsigned)CC1101_TUNING_WINDOW_SECONDS, bw, a2, a0, recv);
}

static void tuneLoop() {
  if (millis() - tuneWindowStart < CC1101_TUNING_WINDOW_SECONDS * 1000UL) {
    return;
  }
  const TuneSetting& s = tuneSettings[tuneIndex];
  char bwStr[12];
  snprintf(bwStr, sizeof(bwStr), "%.3f", s.bandwidthKHz);

  Log.notice(F("TUNE_RESULT %u/%u bw=%s agcctrl2=0x%x agcctrl0=0x%x "
               "signals=%u decoded=%u zero=%u schrader=%u other=%u rssi=%d" CR),
             (unsigned)(tuneIndex + 1), (unsigned)tuneCount,
             bwStr, s.agcctrl2, s.agcctrl0,
             (unsigned)rtl_433_ESP::decoderSignals,
             (unsigned)rtl_433_ESP::decodedMessages,
             (unsigned)rtl_433_ESP::zeroDecodedSignals,
             (unsigned)schraderPacketCount,
             (unsigned)otherPacketCount,
             rtl_433_ESP::currentRssi);

  tuneIndex = (tuneIndex + 1) % tuneCount;
  if (tuneIndex == 0) {
    Log.notice(F("TUNE_CYCLE_COMPLETE, opnieuw" CR));
  }
  applyTuneSetting(tuneIndex);
}
#endif // CC1101_OOK_TUNING

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(1000);

#ifndef LOG_LEVEL
#  define LOG_LEVEL LOG_LEVEL_NOTICE
#endif
  Log.begin(LOG_LEVEL, &Serial);

  Log.notice(F(CR "=== Schrader TPMS scanner ===" CR));
  Log.notice(F("Doel: Schrader motor-TPMS (PN 3142M, FCC MRXRDC3) op 433.92 MHz" CR));

  sensorMutex = xSemaphoreCreateMutex();
  if (!sensorMutex) {
    Log.fatal(F("Kon mutex niet aanmaken, halt" CR));
    while (true) delay(1000);
  }

  memset(sensors, 0, sizeof(sensors));

  // OLED. De DevKitC heeft geen externe pull-ups nodig voor deze modules.
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  setupDisplay();

  // Radio.
  rf.initReceiver(RF_MODULE_RECEIVER_GPIO, RF_MODULE_FREQUENCY);
  rf.setCallback(rtl_433_Callback, messageBuffer, JSON_MSG_BUFFER);
  rf.enableReceiver();
  rf.getModuleStatus();

  Log.notice(F("Ontvanger actief op %F MHz, datapin GPIO %d" CR),
             (double)RF_MODULE_FREQUENCY, RF_MODULE_RECEIVER_GPIO);
  Log.notice(F("MINIMUM_SIGNAL_LENGTH=%d us (frame is ~14000 us)" CR),
             MINIMUM_SIGNAL_LENGTH);

#ifdef CC1101_OOK_TUNING
  Log.notice(F("TUNING-modus: %u instellingen, %us per venster" CR),
             (unsigned)tuneCount, (unsigned)CC1101_TUNING_WINDOW_SECONDS);
  applyTuneSetting(tuneIndex);
#endif

  Log.notice(F("Let op: de sensor zendt alleen bij rollen (>~30 km/h), bij een" CR));
  Log.notice(F("drukdaling >2 psi, of na een 125 kHz LF-trigger." CR));
  Log.notice(F("=== setup klaar ===" CR));
}

// ---------------------------------------------------------------------------
// Demo-modus (env: demo)
// ---------------------------------------------------------------------------
// Voert nepmetingen in zodat je de schermlayout kunt beoordelen zonder op een
// zendende sensor te wachten. Doorloopt 1 sensor, 2 sensoren, en een oude
// meting, en gebruikt de randgevallen die het snelst layoutfouten blootleggen
// ("10.25" is de breedst mogelijke druk, "-12C" de breedste temperatuur).
#ifdef DEMO_MODE
static void demoLoop() {
  static uint32_t lastStep = 0;
  static int step = 0;
  if (millis() - lastStep < 4000) return;
  lastStep = millis();

  if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  memset(sensors, 0, sizeof(sensors));
  uint32_t now = millis();

  switch (step) {
    case 0: // 1 sensor, normaal
      sensors[0] = {5276367, 238.0f, 33.0f, -61, now, 7, true};
      break;
    case 1: // 1 sensor, breedste druk en temperatuur
      sensors[0] = {5276367, 1025.0f, -12.0f, -78, now, 3, true};
      break;
    case 2: // 1 sensor, oude meting (inverted balk)
      sensors[0] = {5276367, 238.0f, 33.0f, -61,
                    now - (READING_STALE_SECONDS + 800) * 1000UL, 7, true};
      break;
    case 3: // 2 sensoren
      sensors[0] = {5276367, 238.0f, 33.0f, -61, now, 7, true};
      sensors[1] = {5271842, 241.0f, 35.0f, -70, now, 4, true};
      break;
    case 4: // 2 sensoren, tweede oud
      sensors[0] = {5276367, 238.0f, 33.0f, -61, now, 7, true};
      sensors[1] = {5271842, 241.0f, 35.0f, -70,
                    now - (READING_STALE_SECONDS + 200) * 1000UL, 4, true};
      break;
    default: // wachtscherm
      break;
  }
  xSemaphoreGive(sensorMutex);

  Log.notice(F("[demo] stap %d" CR), step);
  step = (step + 1) % 6;
}
#endif

void loop() {
  rf.loop();

#ifdef CC1101_OOK_TUNING
  tuneLoop();
#endif
#ifdef DEMO_MODE
  demoLoop();
#endif

  static uint32_t lastDisplay = 0;
  if (millis() - lastDisplay >= DISPLAY_REFRESH_MS) {
    lastDisplay = millis();
    updateDisplay();
  }
}
