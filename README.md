# Schrader TPMS scanner

Leest een **Schrader 3142M** motor-TPMS sensor uit met een ESP32 en een
CC1101 433 MHz module, en zet druk en temperatuur op een 0.96" OLED.

Gebruikt [rtl_433_ESP](https://github.com/NorthernMan54/rtl_433_ESP), de
ESP32-port van [rtl_433](https://github.com/merbanan/rtl_433), met decoder
**[196] "Schrader Motorcycle TPMS sensor"**.

## Werkt dit met mijn sensor?

Deze decoder is gebouwd voor exact deze sensor. De bron-header in rtl_433
noemt:

```
PN:3142M / 29074
FCC ID MRXRDC3
M:RDC3
```

Dezelfde sensor wordt verkocht als 3141M (BMW), 3142M (KTM), 29073, 29074,
29177M (Kawasaki) en als onverprogrammeerde 33900. Allemaal RDC3, 433.92 MHz.

## Hardware

- ESP32 DevKitC V1 (30-pin) + breakout board
- CC1101 433 MHz module met SMA-staafantenne
- SSD1306 0.96" OLED (I²C)

Bedrading staat in **[WIRING.md](WIRING.md)**. Lees die eerst — de CC1101 is
een **3.3V** module en de datapin is verlegd naar GPIO 13 omdat GPIO 22 door
het OLED gebruikt wordt.

## Installeren

PlatformIO staat in een lokale venv, dus er is niets globaal nodig.

```bash
# eenmalig (al gedaan als je dit project net hebt gekregen)
python3 -m venv .venv
.venv/bin/pip install platformio
```

## Bouwen en flashen

```bash
# bouwen
.venv/bin/pio run

# flashen
.venv/bin/pio run -t upload

# meekijken
.venv/bin/pio device monitor
```

De monitor draait op **460800** baud. Dat staat al in `platformio.ini`, dus
`pio device monitor` zonder argumenten is goed.

Niet de 921600 uit de rtl_433_ESP voorbeelden: dit board heeft een CP2102
achter USB-hubs en faalde daar wisselend bij het uploaden
("Unable to verify flash chip connection"). Op 460800 is dat opgelost. Zie
TROUBLESHOOTING.md als je het alsnog tegenkomt.

Een paar regels rommel direct na een reset is normaal — dat is de ESP32
bootloader op een andere vaste snelheid. Zodra
`=== Schrader TPMS scanner ===` verschijnt, is de link goed.

Verschijnt de ESP32 niet, check dan of `/dev/ttyUSB0` bestaat na het
aansluiten. Je zit al in de `dialout`-groep, dus rechten horen te kloppen.

### Controleren of de radio werkt

Bij het opstarten dumpt de firmware de CC1101-registers. De regel die telt:

```
CC1101_VERSION: 0x14
```

`0x14` betekent dat de SPI-bus werkt en de module antwoordt. `0x00` of `0xff`
betekent dat de bedrading van SCK/MOSI/MISO/CSN niet klopt.

Daarna hoort er een RSSI-kalibratie te komen met een **variërende** waarde:

```
Initial RSSI calibration complete: average -64 dbm, threshold -55 dbm, peak -58 dbm
```

## Wat je ziet

Serieel, per decode:

```
[SCHRADER] id=5276367  2.38 bar (238.0 kPa, raw=476)  33.0 C  rssi=-61
[json] {"model":"Schrader-Motorcycle","id":5276367,"pressure_kPa":238.0,...}
```

De `raw=` waarde staat er met opzet bij: de omrekening naar kPa is in rtl_433
afgeleid uit een beperkte sample-set, niet uit een datasheet. Zo kun je het
tegen een manometer narekenen.

Decodes van andere 433 MHz-apparaten in de buurt komen langs als `[other]`.
Die zijn nuttig: ze bewijzen dat de ontvangstketen werkt.

### Op het OLED

Dit is een scherm van 2,4 cm. Daar passen technisch zes regels mini-font op,
maar die zijn in de hand onleesbaar en op een rijdende motor helemaal. De
layout toont daarom **weinig, maar groot**:

**Eén sensor:**

```
 ____________________
| 2.38          bar  |   <- druk, 34px hoog
|                    |
| 33C      12s       |   <- temperatuur en leeftijd
 --------------------
```

**Twee sensoren** (voor- en achterwiel), twee helften met de laatste 4 cijfers
van het sensor-ID:

```
 ____________________
| 2.38 bar     6367  |
|      33C      12s  |
|--------------------|
| 2.41 bar     1842  |
|      35C       4s  |
 --------------------
```

De **leeftijd** is belangrijk: zonder die aanduiding lees je een meting van
tien minuten oud alsof die actueel is. Boven 90 seconden wordt de regel
**inverted** (witte balk) — dat valt met één blik op, terwijl het woord "OUD"
in klein font juist wegvalt. Leeftijden worden compact geschreven: `12s`,
`90s`, `14m`, `3u`.

Details als RSSI, pakketteller, kPa en de ruwe waarde staan alleen in de
seriële log. Daar kun je ze rustig lezen; op het scherm zouden ze alleen de
cijfers kleiner maken.

Alle posities zijn tegen de echte U8g2-fontmetrieken nagemeten, inclusief de
randgevallen `10.25` (breedste druk) en `-12C` (breedste temperatuur), zodat
er niets afgekapt wordt.

### Layout bekijken zonder sensor

```bash
.venv/bin/pio run -e demo -t upload
```

Doorloopt elke 4 seconden alle schermen met nepmetingen: één sensor, breedste
waarden, oude meting, twee sensoren, wachtscherm. Daarna de normale build
terugflashen met `.venv/bin/pio run -t upload`.

## Belangrijk: de sensor zendt bijna nooit

Dit is het eerste waar iedereen tegenaan loopt.

Uit de FCC-documentatie van de RDC3: de sensor zendt bij **rollen** (de
rolschakelaar sluit pas rond 11–32 km/h), bij een **drukdaling van meer dan
2 psi**, of na een **125 kHz LF-trigger**. Stilstaand op de werkbank zendt hij
vrijwel niets — dat is met opzet, voor de batterijduur.

**De sensor naast de ESP32 op tafel leggen levert dus niets op.** Dat is geen
bug.

### Testprotocol

In deze volgorde, anders weet je niet wat je aan het debuggen bent:

1. **Sanity check.** Flash `diag_raw` en kijk of je *welke* 433 MHz-decode dan
   ook krijgt — een weerstation van de buren, een autosleutel, een
   garagedeur. Werkt dat, dan weet je dat stilte later "de sensor zendt niet"
   betekent en niet "de firmware is stuk".

   ```bash
   .venv/bin/pio run -e diag_raw -t upload && .venv/bin/pio device monitor
   ```

2. **Drukdaling.** Laat met de motor stil kort lucht uit de band ontsnappen.
   Een daling van >2 psi forceert een directe transmissie. Dit is de snelste
   reproduceerbare trigger zonder extra hardware.

3. **Rollend.** ESP32 op een USB-powerbank, boven ~30 km/h een stuk rijden,
   daarna de log teruglezen:

   ```bash
   mkdir -p logs
   .venv/bin/pio device monitor | tee logs/rit-$(date +%Y%m%d-%H%M).log
   ```

4. **Optioneel later:** een 125 kHz wektool. Maakt bench-testen pas echt
   praktisch. Op de BMW/KTM-fora werkt de "GM"-stand op deze sensoren.

## Build-omgevingen

| Env | Waarvoor |
|---|---|
| `schraderscanner` | normale build, dit is wat je wilt flashen |
| `demo` | nepmetingen, om de schermlayout te bekijken |
| `diag_raw` | dumpt pulse/gap-timings van elk signaal |
| `diag_schrader` | laat decoder 196 zelf loggen waar hij faalt |
| `diag_fsk` | FSK-pad, om te zien of er signaal is (decodeert **niet**) |
| `tune` | CC1101 bandbreedte/AGC sweep, duurt uren |

```bash
.venv/bin/pio run -e diag_schrader -t upload
```

Wanneer je welke gebruikt staat in **[TROUBLESHOOTING.md](TROUBLESHOOTING.md)**,
met een beslisboom.

## De kritieke instelling

`MINIMUM_SIGNAL_LENGTH` staat op **8000 µs** in plaats van de rtl_433_ESP
default van 40000 µs.

Dit is geen smaakkwestie. `isCompleteSignal()` in de library eist
`duration > MINIMUM_SIGNAL_LENGTH`. Een Schrader-frame is 56 bits Manchester
bij ~122 µs per symbool, dus ongeveer **14 ms**. Met de default van 40 ms
wordt elk frame weggegooid *voordat een decoder het ziet* — je krijgt dan
nooit iets, zonder enige foutmelding.

De sketch heeft een `#error` die de build laat falen als deze waarde boven
14000 komt, zodat dit niet stil terug kan sluipen.

## Bekende risico's

**OOK versus FSK.** De FCC-documentatie van de RDC3 stelt dat de sensor in
Rotating Mode *FSK* met 50% Manchester gebruikt, terwijl de rtl_433-decoder in
het OOK-pad geregistreerd staat. Op een RTL-SDR maakt dat niet uit (die
demoduleert de envelope); op een CC1101 in ASK/OOK-mode mogelijk wel. Dit is
het grootste openstaande risico en de reden dat `diag_fsk` bestaat. Zie
TROUBLESHOOTING.md.

**Bereik.** Een CC1101 haalt ongeveer de helft van het bereik van een
RTL-SDR. Wielkast naar stuur zou moeten lukken, maar is niet gegarandeerd.
Antenne verticaal houden helpt merkbaar.

**Drukschaal.** Zie hierboven bij `raw=`. De 0.5 kPa/bit komt uit een
beperkte sample-set van één gebruiker in rtl_433 issue #3512.

## Projectstructuur

```
platformio.ini        5 build-omgevingen
include/config.h      pinnen, drempels, tracking-parameters
src/main.cpp          sketch
WIRING.md             bedrading
TROUBLESHOOTING.md    beslisboom als er niets binnenkomt
```

## Licentie

rtl_433 en rtl_433_ESP zijn GPL-3.0. Deze sketch linkt daartegen en valt
daarmee onder dezelfde voorwaarden.
