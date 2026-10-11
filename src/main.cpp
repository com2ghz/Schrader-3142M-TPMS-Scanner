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

static void drawSplash(const char* line1, const char* line2) {
  display.clearBuffer();
  display.setFont(u8g2_font_6x12_tf);
  display.drawStr(0, 12, line1);
  if (line2) {
    display.drawStr(0, 26, line2);
  }
  display.sendBuffer();
}

// Scherm met geen enkele sensor gezien.
static void drawWaiting() {
  display.clearBuffer();

  display.setFont(u8g2_font_6x12_tf);
  display.drawStr(0, 10, "Wachten op sensor");

  display.setFont(u8g2_font_5x8_tf);
  char buf[32];
  snprintf(buf, sizeof(buf), "433.92 MHz  rssi %d", rtl_433_ESP::currentRssi);
  display.drawStr(0, 24, buf);

  snprintf(buf, sizeof(buf), "drempel %d  sig %u",
           rtl_433_ESP::rssiThreshold,
           (unsigned)rtl_433_ESP::decoderSignals);
  display.drawStr(0, 34, buf);

  snprintf(buf, sizeof(buf), "andere decodes: %u", (unsigned)otherPacketCount);
  display.drawStr(0, 44, buf);

  // y=62 en niet 63: u8g2_font_5x8_tf heeft descent -1, dus op baseline 63
  // worden de staarten van 'p' en 'g' tegen de bufferrand afgekapt.
  display.drawStr(0, 54, "sensor zendt bij rollen");
  display.drawStr(0, 62, "of drukdaling >2psi");

  display.sendBuffer();
}

// Scherm met 1 sensor: groot.
static void drawSingle(const SensorReading& r, uint32_t nowMs) {
  uint32_t ageS = (nowMs - r.lastSeenMs) / 1000UL;
  bool stale = ageS > READING_STALE_SECONDS;

  char buf[32];

  display.clearBuffer();

  // Druk, groot.
  display.setFont(u8g2_font_logisoso24_tn);
  snprintf(buf, sizeof(buf), "%.2f", r.pressure_kPa / 100.0f);
  display.drawStr(0, 30, buf);

  display.setFont(u8g2_font_6x12_tf);
  display.drawStr(84, 30, "bar");

  // Temperatuur.
  display.setFont(u8g2_font_6x12_tf);
  snprintf(buf, sizeof(buf), "%.0f C", r.temperature_C);
  display.drawStr(0, 44, buf);

  // ID.
  display.setFont(u8g2_font_5x8_tf);
  snprintf(buf, sizeof(buf), "id %lu", (unsigned long)r.id);
  display.drawStr(44, 44, buf);

  // Statusregel: leeftijd, rssi, aantal pakketten.
  if (stale) {
    snprintf(buf, sizeof(buf), "OUD %lus  rssi %d  n%lu",
             (unsigned long)ageS, r.rssi, (unsigned long)r.packetCount);
  } else {
    snprintf(buf, sizeof(buf), "%lus geleden  rssi %d  n%lu",
             (unsigned long)ageS, r.rssi, (unsigned long)r.packetCount);
  }
  display.drawStr(0, 56, buf);

  snprintf(buf, sizeof(buf), "%.0f kPa  raw %d",
           r.pressure_kPa, (int)lroundf(r.pressure_kPa / 0.5f));
  display.drawStr(0, 62, buf);

  display.sendBuffer();
}

// Scherm met meerdere sensoren: compacte lijst.
static void drawList(uint32_t nowMs) {
  display.clearBuffer();
  display.setFont(u8g2_font_5x8_tf);

  char buf[40];
  int y = 8;

  for (int i = 0; i < MAX_SENSORS; i++) {
    if (!sensors[i].valid) continue;

    uint32_t ageS = (nowMs - sensors[i].lastSeenMs) / 1000UL;
    bool stale = ageS > READING_STALE_SECONDS;

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)sensors[i].id);
    display.drawStr(0, y, buf);

    snprintf(buf, sizeof(buf), "%.2fb", sensors[i].pressure_kPa / 100.0f);
    display.drawStr(54, y, buf);

    snprintf(buf, sizeof(buf), "%.0fC", sensors[i].temperature_C);
    display.drawStr(88, y, buf);

    snprintf(buf, sizeof(buf), "%s%lus", stale ? "!" : "", (unsigned long)ageS);
    display.drawStr(108, y, buf);

    y += 10;
    if (y > 50) break;
  }

  snprintf(buf, sizeof(buf), "rssi %d  pkt %u",
           rtl_433_ESP::currentRssi, (unsigned)schraderPacketCount);
  display.drawStr(0, 62, buf);

  display.sendBuffer();
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

  // Zelf proben of er iets op het adres antwoordt. U8G2::begin() geeft op deze
  // build altijd 1 terug en controleert de I2C-ACK niet, dus zonder deze test
  // levert een verkeerd adres een zwart scherm zonder enige melding.
  Wire.beginTransmission(OLED_I2C_ADDR);
  displayPresent = (Wire.endTransmission() == 0);

  if (!displayPresent) {
    // Niet fataal: de scanner blijft via serieel bruikbaar.
    Log.error(F("OLED antwoordt niet op 0x%x. Probeer 0x3D in config.h, "
                "zie TROUBLESHOOTING.md" CR),
              OLED_I2C_ADDR);
  } else {
    display.setI2CAddress(OLED_I2C_ADDR << 1);
    display.begin();
    display.setFontMode(1);
    drawSplash("Schrader TPMS", "radio starten...");
    Log.notice(F("OLED gevonden op 0x%x" CR), OLED_I2C_ADDR);
  }

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

void loop() {
  rf.loop();

#ifdef CC1101_OOK_TUNING
  tuneLoop();
#endif

  static uint32_t lastDisplay = 0;
  if (millis() - lastDisplay >= DISPLAY_REFRESH_MS) {
    lastDisplay = millis();
    updateDisplay();
  }
}
