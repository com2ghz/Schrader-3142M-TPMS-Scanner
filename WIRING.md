# Bedrading

ESP32 DevKitC V1 (30-pin) + breakout board, CC1101 433MHz module, SSD1306 0.96" OLED.

## Waarschuwing vooraf

**De CC1101 is een 3.3V module (1.9–3.6V).** Sluit VCC aan op de **3V3**-pin van
de ESP32, niet op 5V/VIN. 5V beschadigt de module.

## CC1101 → ESP32

De CC1101 heeft 8 pinnen. Op de meeste modules staan ze in twee rijen van 4
met de labels op de print.

| CC1101 | ESP32 GPIO | Functie | Build flag |
|--------|-----------|---------|------------|
| VCC    | **3V3**   | voeding | — |
| GND    | GND       | massa   | — |
| SCK    | 18        | SPI clock | VSPI default |
| MOSI   | 23        | SPI data naar module | VSPI default |
| MISO   | 19        | SPI data van module | VSPI default |
| CSN    | 5         | chip select | `RF_MODULE_CS=5` |
| GDO0   | **13**    | **datapin** (gedemoduleerde bitstream) | `RF_MODULE_GDO0=13` |
| GDO2   | 4         | status | `RF_MODULE_GDO2=4` |

Sommige modules labelen CSN als `CSN`, `CS` of `SS`. MOSI/MISO heten soms
`SI`/`SO`. GDO0/GDO2 zijn altijd zo gelabeld.

### Waarom GPIO 13 en niet 22

De rtl_433_ESP README gebruikt GPIO 22 voor GDO0. Dat kan hier niet: GPIO 22
is I²C SCL voor het OLED. GDO0 is verlegd naar 13. De sketch heeft een
compile-time check die faalt als de datapin alsnog op een I²C-pin terechtkomt.

## OLED (SSD1306) → ESP32

| OLED | ESP32 GPIO |
|------|-----------|
| VCC  | 3V3 |
| GND  | GND |
| SDA  | 21 |
| SCL  | 22 |

I²C-adres is standaard `0x3C`. Sommige modules zijn `0x3D` — zie
TROUBLESHOOTING.md als het scherm leeg blijft.

## Antenne

Schroef de meegeleverde SMA-staafantenne op de module.

**Oriënteer de antenne verticaal.** De rtl_433_ESP documentatie noemt dit
expliciet: verticaal geeft merkbaar betere ontvangst. Reken er daarnaast op dat
een CC1101 ongeveer de helft van het bereik van een RTL-SDR haalt. Voor een
sensor in je wielkast naar een ontvanger op het stuur zou dat moeten volstaan,
maar het is geen gegeven.

Houd de antenne uit de buurt van de ESP32-print zelf en van USB-kabels; die
koppelen ruis in op 433 MHz.

## Pinnen die je vrij moet laten

- **GPIO 6–11**: intern verbonden met de SPI-flash. Niet gebruiken.
- **GPIO 0, 2, 12, 15**: strapping-pinnen die het boot-gedrag bepalen. GPIO 2
  wordt hier gebruikt voor de onboard LED (die is daar al op de DevKitC op
  aangesloten), dat is veilig. Hang niets anders aan 0/12/15.
- **GPIO 34–39**: input-only, geen pull-ups. Niet geschikt voor CS/GDO.

## Controle vóór inschakelen

1. VCC van de CC1101 op 3V3, niet 5V.
2. Geen draad tussen GDO0 (13) en de I²C-pinnen (21/22).
3. GND van beide modules aan de ESP32-GND.
4. Antenne vastgeschroefd — zenden zonder antenne is slecht voor de module,
   en wij ontvangen alleen, maar een losse antenne verklaart ook "ik zie niets".

Na het aanzetten moet de seriële monitor een CC1101-registerdump laten zien
(`RF_MODULE_INIT_STATUS` staat aan). Zie je `CC1101_VERSION: 0x14`, dan
praat de SPI-bus. Zie je `0x00` of `0xff`, dan is de bedrading van
SCK/MOSI/MISO/CSN mis.
