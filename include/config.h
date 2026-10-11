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

// Aantal sensoren dat we tegelijk bijhouden. Een motor heeft 2 wielen; 4 geeft
// ruimte om een losse sensor op de werkbank mee te nemen zonder dat een
// bestaande meting verdrongen wordt.
#define MAX_SENSORS 4

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
#define SERIAL_BAUD 921600

// JSON-buffer voor rtl_433_ESP. Decode-berichten zijn klein (~200 byte), maar
// de library deelt deze buffer met alle 223 decoders; sommige zijn breedsprakig.
#define JSON_MSG_BUFFER 512

// OLED verversingsinterval in ms. Het scherm moet de leeftijd-teller laten
// lopen, dus we verversen ook zonder nieuwe decode.
#define DISPLAY_REFRESH_MS 500

#endif // SCHRADERSCANNER_CONFIG_H
