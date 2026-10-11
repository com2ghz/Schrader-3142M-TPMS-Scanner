# Troubleshooting

Deze scanner heeft twee soorten "ik zie niets":

1. **De sensor zendt niet.** Veruit het meest waarschijnlijk. Lees eerst
   [De sensor zendt bijna nooit](#de-sensor-zendt-bijna-nooit).
2. **De ontvangstketen werkt niet.** Daarvoor is de beslisboom hieronder.

Begin altijd met de sanity check, anders weet je niet welke van de twee je aan
het debuggen bent.

---

## Upload mislukt

### "Unable to verify flash chip connection (No serial data received.)"

Meestal **te hoge upload-snelheid**, niet een defect board. Typisch beeld:

```
Changing baud rate to 921600
Changed.
A fatal error occurred: Unable to verify flash chip connection (No serial data received.).
```

Vaak staat erboven ook:

```
WARNING: Detected crystal freq 15.67MHz is quite different to normalized freq 26MHz.
```

Die kristal-waarschuwing is **geen hardwareprobleem**. Het is een symptoom van
dezelfde onbetrouwbare seriële link: esptool kan de timing niet goed meten.
Zodra de snelheid omlaag gaat, verdwijnt de melding.

Dit board heeft een CP2102 en hangt achter een keten USB-hubs. Daar is 921600
(de waarde uit de rtl_433_ESP voorbeelden) te snel. `platformio.ini` staat
daarom op **460800**, wat hier 3 van 3 uploads haalde met een schone
`Hash of data verified`.

Werkt 460800 bij jou nog niet betrouwbaar:

1. Zet in `platformio.ini` **beide** waarden op 115200:
   ```ini
   monitor_speed = 115200
   upload_speed = 115200
   ```
2. Pas `SERIAL_BAUD` in `include/config.h` naar dezelfde waarde aan.

Die twee moeten gelijk blijven. De sketch heeft een `#error` die de build laat
falen als ze uit elkaar lopen, want anders zie je in de monitor alleen
onleesbare tekens — wat op een stukke ESP32 lijkt terwijl het alleen een
baudrate-verschil is.

Verder nog: sluit de ESP32 direct op de PC aan in plaats van via een hub, en
gebruik een korte, goede USB-kabel.

### Onleesbare tekens in de monitor

Een paar regels rommel **direct na een reset** is normaal: dat is de ESP32
bootloader, die op een andere vaste snelheid praat dan onze firmware. Zodra
`=== Schrader TPMS scanner ===` verschijnt, is de link goed.

Blijft *alles* onleesbaar, dan wijkt `monitor_speed` af van `SERIAL_BAUD`.

---

## De sensor zendt bijna nooit

Uit de FCC-documentatie van de Schrader RDC3 (FCC ID MRXRDC3):

- **Stationary Mode**: stilstaand zendt de sensor zeer weinig tot niets. Dit is
  met opzet, om de batterij 5–10 jaar mee te laten gaan.
- **Rolschakelaar**: de sensor wordt gewekt door centrifugaalkracht van 2–17 G.
  Dat is afhankelijk van de wieldiameter typisch **11–32 km/h**.
- **Drukverandling**: een **daling** van meer dan 2 psi (~0.14 bar) geeft een
  directe transmissie. Een *stijging* doet niets (FCC Part 15.231-eis).
- **LF-trigger**: een 125 kHz magnetisch signaal forceert een transmissie
  binnen 150 ms.

**Je sensor op de werkbank naast de ESP32 leggen geeft dus niets.** Dat is geen
bug in de firmware.

### Triggers, van makkelijk naar lastig

| Methode | Werkt | Opmerking |
|---|---|---|
| Lucht laten ontsnappen | Ja, direct | >2 psi **daling**. Snelste bench-test. |
| Rijden boven ~30 km/h | Ja | Gebruik een USB-powerbank en lees de log terug. |
| Sensor ronddraaien met de hand | Soms | Moet 2+ G halen, dat is flink zwaaien. |
| 125 kHz wektool | Ja, betrouwbaar | Extra hardware. Op BMW/KTM-fora werkt de "GM"-stand op deze sensoren. |
| Stilstaand wachten | Nauwelijks | Kan tientallen minuten tot niets zijn. |

---

## Stap 0: sanity check van de ontvangstketen

**Doe dit vóór al het andere.** Je wilt weten of de radio überhaupt werkt,
zonder van de TPMS-sensor af te hangen.

```bash
.venv/bin/pio run -e diag_raw -t upload
.venv/bin/pio device monitor
```

Je woonomgeving zit vol 433 MHz: weerstations van buren, autosleutels,
garagedeuren, deurbellen, thermometers. Binnen een paar minuten hoort er iets
langs te komen.

**Wat je wilt zien:**

```
rtl_433_ESP(6): CC1101 receive frequency: 433.920013
CC1101_VERSION: 0x14
...
rtl_433_ESP(6): Signal length: 215000, Gap length: ..., Signal RSSI: -55, pulses: 162
N: [other] {"model":"Nexus-TH","id":23,"temperature_C":18.4,...}
```

- **`CC1101_VERSION: 0x14`** → de SPI-bus werkt. Is het `0x00` of `0xff`, dan is
  de bedrading van SCK/MOSI/MISO/CSN mis. Ga naar [SPI werkt niet](#spi-werkt-niet).
- **Een willekeurige `[other]` decode** → de hele keten werkt. Als je nu geen
  Schrader ziet, is dat een sensor-trigger-probleem, niet een firmware-probleem.
  Ga terug naar [De sensor zendt bijna nooit](#de-sensor-zendt-bijna-nooit).
- **`Signal length:` regels maar nooit een decode** → de radio hoort iets maar
  het slicen lukt niet. Ga naar [Pulsen maar geen decodes](#pulsen-maar-geen-decodes).
- **Helemaal niets, ook geen RSSI-beweging** → ga naar [Radio lijkt dood](#radio-lijkt-dood).

---

## Beslisboom

### SPI werkt niet

Symptoom: `CC1101_VERSION: 0x00` of `0xff`, of `radio.begin() failed`.

1. VCC op **3V3**, niet 5V en niet VIN.
2. GND gedeeld tussen CC1101 en ESP32.
3. SCK=18, MOSI=23, MISO=19, CSN=5. MOSI en MISO worden vaak verwisseld; op
   sommige modules heten ze `SI`/`SO`.
4. Korte draden. Dupont-draadjes van 20cm+ op een SPI-bus van enkele MHz
   geven problemen.

### Radio lijkt dood

Symptoom: SPI werkt (`VERSION: 0x14`), maar geen enkele `Signal length:` regel
en de RSSI op het OLED beweegt niet.

1. **Antenne vastgeschroefd?** Zonder antenne hoor je vrijwel niets.
2. **GDO0 op GPIO 13?** Dit is de datapin. Zit die los, dan komt er nooit een
   interrupt en dus nooit een signaal — terwijl SPI gewoon blijft werken. Dit is
   de meest voorkomende oorzaak van "SPI ok, maar niets ontvangen".
3. Kijk naar de RSSI-waarde op het OLED. Een normale waarde is tussen −90 en
   −100 dBm als er niets zendt. Blijft hij op precies dezelfde waarde hangen,
   dan leest de library de radio niet goed uit.

### Pulsen maar geen decodes

Symptoom: `Signal length: ...` regels, maar nooit een decode. Dit is het punt
waar het interessant wordt.

Zet de Schrader-decoder zelf aan het woord:

```bash
.venv/bin/pio run -e diag_schrader -t upload
.venv/bin/pio device monitor
```

Trigger nu de sensor (lucht laten ontsnappen). De decoder logt dan precies
waar hij afhaakt:

| Log | Betekenis | Actie |
|---|---|---|
| `Row error` | bitbuffer heeft meerdere rows | Ruis splitst het frame. Zie [Ruis](#te-veel-ruis). |
| `Preamble not found` | `0x7ff8` niet gevonden | Timing zit naast. Zie [Timing](#timing-zit-naast). |
| `Too short` | preamble gevonden, <56 bits erna | Signaal wordt afgekapt. Verhoog `MINIMUM_SIGNAL_LENGTH` **niet**; kijk naar de RSSI-drempel. |
| `CRC error` | bits komen door, maar verkeerd | Bijna goed. Zie [Timing](#timing-zit-naast). |
| `MSG <hex>` | de ruwe 56 bits | Goud. Hiermee kun je handmatig narekenen. |

Zie je `MSG` met plausibele hex maar `CRC error`, dan ben je er bijna: de
bitstream komt door, enkele bits kantelen. Dat is een bandbreedte/AGC-kwestie.

### Timing zit naast

De decoder verwacht 122 µs symbolen met `reset_limit` 375 µs. De CC1101 heeft
een vaste bandbreedte die de pulsvormen vervormt.

Probeer een smallere ontvangstbandbreedte. De default in rtl_433_ESP is
812 kHz, wat erg breed is voor dit signaal:

```ini
; in platformio.ini, bij env:schraderscanner
'-DCC1101_RX_BANDWIDTH=203.125'
'-DCC1101_AGCCTRL2=0xC7'
'-DCC1101_AGCCTRL0=0x93'
```

Werkt dat niet, laat dan de sweep lopen (zie [tune](#tune-laatste-redmiddel)).

### Te veel ruis

Symptoom: veel `Signal length:` regels met weinig pulsen, of `Row error`.

De RSSI-drempel wordt automatisch gezet op de gemiddelde ruisvloer + 9 dB.
In een luidruchtige omgeving kun je die verhogen:

```ini
'-DRSSI_THRESHOLD=14'
```

Nadeel: een hogere drempel betekent dat zwakke signalen (sensor ver weg)
gemist worden. Dit is een afweging, geen oplossing.

### Is het eigenlijk FSK?

Dit is het grootste openstaande risico in dit project, en het is niet met
zekerheid op te lossen zonder meten.

De FCC-documentatie van de RDC3 stelt letterlijk: *"During the Rotating Mode,
the modulation used for the sensor is FSK (Frequency-Shift Keying) with 50%
Manchester bi-phase encoding."*

Maar de rtl_433-decoder staat geregistreerd als `OOK_PULSE_MANCHESTER_ZEROBIT`,
dus in het OOK-pad. Op een RTL-SDR maakt dat niet uit — die demoduleert de
envelope en een FSK-signaal met voldoende deviatie ziet er in de envelope uit
als OOK. Een CC1101 in ASK/OOK-mode is daar gevoeliger voor.

Test:

```bash
.venv/bin/pio run -e diag_fsk -t upload
.venv/bin/pio device monitor
```

**Let op: deze env decodeert niets.** Decoder 196 staat niet in het FSK-pad
geregistreerd. De env dient om te zien of er in FSK-mode pulsen binnenkomen
waar het OOK-pad stil bleef.

| Uitkomst | Conclusie |
|---|---|
| Geen pulsen in OOK, wél in FSK | Dit is het probleem. De decoder moet naar het FSK-pad. |
| Pulsen in beide | Modulatie is niet het probleem; kijk naar timing/bandbreedte. |
| Geen pulsen in beide | De sensor zendt niet, of de radio is dood. |

Blijkt het FSK te zijn, dan is de oplossing de library vendoren en
`tpms_schrader_motorcycle` in de FSK-registratielijst zetten. Dat is een
ingreep in `signalDecoder.cpp` en `rtl_433_devices.h`; meld het dan ook
upstream bij rtl_433 issue #3512.

### tune: laatste redmiddel

```bash
.venv/bin/pio run -e tune -t upload
.venv/bin/pio device monitor | tee logs/tune.log
```

Loopt automatisch door 9 combinaties van bandbreedte en AGC-registers, 120
seconden per combinatie (~18 minuten per cyclus) en rapporteert per instelling:

```
TUNE_RESULT 3/9 bw=325.00 agcctrl2=0x3 agcctrl0=0x91 signals=12 decoded=0 zero=12 schrader=0 other=2 rssi=-88
```

Let op `schrader=` en `decoded=`. De instelling met de hoogste waarden neem je
over als `CC1101_RX_BANDWIDTH` / `CC1101_AGCCTRL2` / `CC1101_AGCCTRL0` in de
hoofd-env.

Dit is alleen zinvol als de sensor ondertussen blíjft zenden — dus met een
125 kHz wektool, of tijdens een lange rit. Stilstaand levert het niets op.

---

## OLED blijft leeg

De firmware zoekt het scherm zelf op: eerst het adres uit `config.h`, dan
0x3C en 0x3D. Lukt dat niet, dan scant hij de hele I²C-bus en logt wat er wel
hangt. Je hoeft dus geen losse scanner-sketch te flashen; lees gewoon de
seriële output bij het opstarten.

**Geval 1 — scherm gevonden op een ander adres:**

```
W: OLED op 0x3d in plaats van de ingestelde 0x3c. Werkt, maar pas OLED_I2C_ADDR in config.h aan.
```

Werkt meteen. Zet het adres alsnog in `include/config.h` zodat de melding weg
is.

**Geval 2 — wel apparaten, niet het OLED:**

```
E: Geen OLED gevonden op 0x3C of 0x3D. Bus scannen...
E:   I2C-apparaat op 0x68
E:   Zet het juiste adres in include/config.h bij OLED_I2C_ADDR.
```

De bus werkt. Is 0x68 je enige apparaat, dan is dat geen SSD1306 (dat is een
typisch adres voor een gyro/RTC) — check of je het juiste moduletype hebt.

**Geval 3 — niets op de bus:**

```
E: Geen OLED gevonden op 0x3C of 0x3D. Bus scannen...
E:   Niets op de bus. Check: VCC op 3V3, GND gedeeld, SDA=GPIO21, SCL=GPIO22 niet verwisseld.
```

Geen enkel apparaat op 127 adressen. Dat is bedrading, geen adresprobleem:

1. **VCC op 3V3** en **GND gedeeld** met de ESP32. Zonder gemeenschappelijke
   massa werkt I²C niet, ook al zit VCC goed.
2. **SDA op GPIO 21, SCL op GPIO 22.** Verwisseld is de meest voorkomende fout.
3. Draadjes echt doorverbonden — bij breakout boards en breadboards is een
   slecht contact snel gemaakt. Meet desnoods door.
4. Sommige goedkope modules hebben geen pull-up-weerstanden aan boord. In dat
   geval 4.7k–10k van SDA en SCL naar 3V3.

De scanner werkt zonder OLED gewoon door via serieel. Dit is dus niet fataal —
alleen je scherm blijft zwart.

---

## De drukwaarde lijkt verkeerd

De decoder rekent `pressure_kPa = raw * 0.5`. Die schaal is in rtl_433
afgeleid uit een beperkte sample-set van één gebruiker (issue #3512), niet uit
een Schrader-datasheet.

De firmware logt daarom de ruwe waarde mee:

```
[SCHRADER] id=5276367  2.38 bar (238.0 kPa, raw=476)  33.0 C  rssi=-61
```

Vergelijk `bar` met een manometer. Zit er een consistente factor of offset
tussen, dan is de schaal in de decoder voor jouw sensorvariant niet precies
goed. Dat is een bevinding die upstream thuishoort, niet een fout in deze
firmware. Noteer meerdere (raw, werkelijke druk)-paren voordat je conclusies
trekt.

Temperatuur is `raw - 50` in °C. Die is op dezelfde manier afgeleid.

---

## Logs bewaren voor een testrit

```bash
mkdir -p logs
.venv/bin/pio device monitor | tee logs/rit-$(date +%Y%m%d-%H%M).log
```

De firmware logt elke Schrader-decode twee keer: één leesbare regel en één
`[json]`-regel. Die laatste is makkelijk te filteren:

```bash
grep '^\[json\]' logs/rit-*.log | sed 's/^\[json\] //' > readings.jsonl
```
