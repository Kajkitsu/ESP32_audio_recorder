# ESP32 audio recorder

Rejestrator stereo do miksera: sygnał z wyjścia słuchawkowego miksera trafia do
przetwornika ADC PCM1808, a ESP32-S3 odbiera go po I2S i zapisuje jako WAV na
karcie microSD.

Stan: jest tylko dokumentacja sprzętu, firmware jeszcze nie powstał.

## Sprzęt

| Element | Model |
| --- | --- |
| Płytka | Waveshare ESP32-S3-Touch-LCD-2 (LCD 240×320, slot microSD) |
| ADC | DIY MORE DM-AMAD01 „PCM1808_IIS”: USB Type-C, jack 3,5 mm, generator 12,288 MHz |
| Źródło | wyjście słuchawkowe miksera, kabel jack stereo (TRS) |

Jeden moduł nagrywa oba kanały, drugi nie jest potrzebny.

## Połączenia

Moduł zostaje w ustawieniu fabrycznym: jest masterem I2S i sam wytwarza zegary
z generatora kwarcowego. ESP32-S3 pracuje jako slave i tylko odbiera dane.

| PCM1808 | ESP32-S3 (złącze P1) | Uwagi |
| --- | --- | --- |
| GND | GND | masa musi być wspólna |
| LRCK | GPIO 2 | wyjście modułu, 48 kHz |
| BCK | GPIO 4 | wyjście modułu, 3,072 MHz |
| DATA | GPIO 6 | wyjście modułu |
| MCK | nie podłączać | wyjście 12,288 MHz, ESP go nie potrzebuje |
| VDD33 | nie podłączać | wyjście stabilizatora modułu |
| FMT, MD1, MD0 | nie podłączać | bez zworek |

GPIO 2, 4 i 6 są wolne, dopóki nie jest podłączona kamera. Przewody I2S powinny
mieć najwyżej kilka centymetrów.

## Zasilanie

Moduł potrzebuje tylko 5 V; napięcie 3,3 V wytwarza własnym stabilizatorem.

- Najprościej: osobny kabel do gniazda USB Type-C modułu, z tego samego
  powerbanku lub ładowarki co płytka ESP.
- Jeden kabel: pola „+” i „−” na spodzie modułu połączone z pinami 5V i GND
  złącza P1. Opis sprzedawcy nie wyjaśnia tych pól, więc najpierw zasil moduł
  przez USB i sprawdź multimetrem, czy jest na nich 5 V.

## Tryb pracy i zworki

| Pole | Stan fabryczny | Znaczenie |
| --- | --- | --- |
| FMT | niski | format I2S |
| MD1, MD0 | wysoki (rezystory do 3,3 V na płytce) | master, 256 fs, 48 kHz |

Sprzedawca zaleca dla ESP32 tryb slave (MD1 i MD0 do GND). Nie rób tego:
przetwornik nadal pracowałby na własnym generatorze, a BCK i LRCK pochodziłyby
z niezsynchronizowanego zegara ESP. Według noty PCM1808 kończy się to
okresowymi przerwami w dźwięku.

Z generatorem 12,288 MHz moduł nagrywa w 48 kHz. Podane w ofercie 96 kHz
wymagałoby innego zegara.

## Konfiguracja I2S

| Parametr | Wartość |
| --- | --- |
| Rola ESP | slave, odbiór (RX) |
| Format | I2S Philips, stereo |
| Próbkowanie | 48 kHz |
| Dane | 24 bity w slocie 32-bitowym (ramka 64 BCK) |
| MCLK | nieużywany |

## Piny płytki Waveshare

Ze schematu producenta. Złącza P1 i P2 wyprowadzają 22 GPIO.

| Funkcja | GPIO |
| --- | --- |
| LCD i microSD (wspólne SPI) | 38, 39, 40, 41, 42, 45 |
| Reset LCD i przycisk BOOT | 0 |
| Podświetlenie LCD | 1 |
| Dotyk i IMU (I2C) | 47, 48, przerwania 46 i 3 |
| Pomiar baterii | 5 |
| USB | 19, 20 |
| UART | 43, 44 |
| Złącze kamery | 2, 4, 6–17, 21 |
| Wolny | 18 |

Pin 5V złącza to bezpośrednio napięcie z USB. Przy zasilaniu z akumulatora
jest na nim 0 V.

## Filtr LC

Nie jest wymagany. PCM1808 ma wbudowany filtr antyaliasingowy i górnoprzepustowy,
a moduł ma własny stabilizator i kondensatory sprzęgające na wejściu.

Filtr na linii 5 V ma sens dopiero wtedy, gdy w nagraniu ciszy słychać pisk lub
terkot: koralik ferrytowy albo dławik 10–47 µH szeregowo, a za nim kondensator
100–470 µF do masy.

Brum 50 Hz to pętla masy, nie zasilanie. Pomaga separator audio
z transformatorami albo zasilanie rejestratora z powerbanku.

## Poziom sygnału

Wejście PCM1808 przesterowuje się przy 3 Vpp (około 1 Vrms). Wyjście słuchawkowe
miksera potrafi dać kilka razy więcej, więc:

- ustaw głośność słuchawek tak, żeby szczyty były tuż poniżej przesterowania,
- jeśli zakres regulacji jest niewygodny, dodaj na każdy kanał dzielnik
  10 kΩ / 10 kΩ, który obniża poziom o 6 dB.

## Do zrobienia

- [ ] sprawdzić multimetrem pola „+” i „−” modułu
- [ ] firmware: odbiór I2S i zapis WAV na microSD
- [ ] wskaźnik poziomu i przesterowania na LCD
