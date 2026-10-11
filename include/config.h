// Centrale configuratie voor de Schrader TPMS scanner.
//
// Pinnen en radio-instellingen staan in platformio.ini als build flags, omdat
// rtl_433_ESP die via de preprocessor verwacht. Hier staat alleen wat de
// sketch zelf gebruikt.

#ifndef SCHRADERSCANNER_CONFIG_H
#define SCHRADERSCANNER_CONFIG_H

// ---------------------------------------------------------------------------
// OLED (SSD1306 0.96" op I2C)
// ---------------------------------------------------------------------------
#define OLED_SDA_PIN    21
#define OLED_SCL_PIN    22
#define OLED_WIDTH      128
#define OLED_HEIGHT     64
#define OLED_I2C_ADDR   0x3C  // sommige modules zijn 0x3D, zie TROUBLESHOOTING.md
#define OLED_RESET_PIN  -1    // geen reset-pin op deze modules

// ---------------------------------------------------------------------------
// Sensor-tracking
// ---------------------------------------------------------------------------

// Aantal sensoren dat we tegelijk bijhouden.
//
// 2 en niet 4: een motor heeft twee wielen, en op een 0.96" scherm zijn twee
// metingen het maximum dat nog leesbaar groot past. Een derde sensor zou het
// lettertype zo klein maken dat je er tijdens het rijden niets aan hebt.
#define MAX_SENSORS 2

// Na hoeveel seconden een meting als "oud" op het OLED gemarkeerd wordt.
// De Schrader RDC3 zendt rollend elke ~10s tot ~60s; stilstaand veel minder
// vaak. Zonder leeftijdsindicatie lees je straks een meting van 10 minuten
// terug alsof die actueel is.
#define READING_STALE_SECONDS 90

// Na hoeveel seconden een sensor-slot vrijgegeven wordt voor een andere ID.
#define READING_EXPIRE_SECONDS 1800

// ---------------------------------------------------------------------------
// Het model dat we eruit filteren
// ---------------------------------------------------------------------------
// Exact de string uit rtl_433 src/devices/tpms_schrader_motorcycle.c
#define SCHRADER_MODEL_NAME "Schrader-Motorcycle"

// ---------------------------------------------------------------------------
// Serieel
// ---------------------------------------------------------------------------
// Moet gelijk zijn aan monitor_speed in platformio.ini, anders zie je in de
// monitor alleen onleesbare tekens. 460800 omdat 921600 via een CP2102 achter
// USB-hubs niet betrouwbaar is; zie de toelichting in platformio.ini.
#define SERIAL_BAUD 460800

// JSON-buffer voor rtl_433_ESP. Decode-berichten zijn klein (~200 byte), maar
// de library deelt deze buffer met alle 223 decoders; sommige zijn breedsprakig.
#define JSON_MSG_BUFFER 512

// OLED verversingsinterval in ms. Het scherm moet de leeftijd-teller laten
// lopen, dus we verversen ook zonder nieuwe decode.
#define DISPLAY_REFRESH_MS 500

// ---------------------------------------------------------------------------
// Schermwisseling
// ---------------------------------------------------------------------------
// Het scherm wisselt tussen een meetweergave (druk/temperatuur) en een
// ID-weergave. De ID-weergave is bedoeld om het nummer over te nemen in
// MotoScan / GS-911 / een TPMS-tool, dus die staat langer in beeld: een ID
// foutloos overtypen kost meer tijd dan een druk aflezen.
#define SCREEN_MEASURE_MS 5000
#define SCREEN_ID_MS      8000

#endif // SCHRADERSCANNER_CONFIG_H
