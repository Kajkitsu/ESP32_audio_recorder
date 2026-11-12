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
| „+” (spód modułu) | 5V | zasilanie modułu z gniazda USB płytki ESP |
| „−” (spód modułu) | GND | masa zasilania |
| GND | GND | masa sygnałowa, przy przewodach I2S |
| LRCK | GPIO 2 | wyjście modułu, 48 kHz |
| BCK | GPIO 4 | wyjście modułu, 3,072 MHz |
| DATA | GPIO 6 | wyjście modułu |
| MCK | nie podłączać | wyjście 12,288 MHz, ESP go nie potrzebuje |
| VDD33 | nie podłączać | wyjście stabilizatora modułu |
| FMT, MD1, MD0 | nie podłączać | bez zworek |

GPIO 2, 4 i 6 są wolne, dopóki nie jest podłączona kamera. Przewody I2S powinny
mieć najwyżej kilka centymetrów.

## Zasilanie

Całość zasila jedno gniazdo USB Type-C na płytce ESP. Pin 5V złącza P1 to
bezpośrednio napięcie z USB i z niego moduł dostaje 5 V przez pola „+” i „−”.
Napięcie 3,3 V moduł wytwarza własnym stabilizatorem.

Opis sprzedawcy nie wyjaśnia pól „+” i „−”, więc przed lutowaniem sprawdź je raz:

1. Zasil sam moduł przez jego gniazdo USB Type-C.
2. Zmierz napięcie między „+” a „−”. Powinno być 4,5–5 V, plus na „+”.
3. Odłącz kabel od modułu i dopiero wtedy połącz pola z pinami 5V i GND.

Zasady:

- Do gniazda USB modułu nic już nie podłączaj. Dwa źródła 5 V byłyby zwarte
  ze sobą.
- Rejestrator wymaga zasilania z USB (ładowarka lub powerbank). Z samego
  akumulatora płytki ESP pin 5V ma 0 V i moduł nie działa.
- Jeśli pomiar nie pokaże 5 V, zostaje osobny kabel USB do modułu z tego samego
  źródła co płytka ESP.

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

## Filtr LC

Nie jest wymagany. PCM1808 ma wbudowany filtr antyaliasingowy i górnoprzepustowy,
a moduł ma własny stabilizator i kondensatory sprzęgające na wejściu.

Moduł dzieli linię 5 V z płytką ESP, więc zakłócenia od WiFi i przetwornicy
mogą przechodzić do części analogowej. Jeśli w nagraniu ciszy słychać pisk lub
terkot, wstaw filtr w przewód 5 V do modułu: koralik ferrytowy albo dławik
10–47 µH szeregowo, a za nim kondensator 100–470 µF między „+” a „−”.

Brum 50 Hz to pętla masy, nie zasilanie. Pomaga separator audio
z transformatorami albo zasilanie rejestratora z powerbanku.

## Poziom sygnału

Wejście PCM1808 przesterowuje się przy 3 Vpp (około 1 Vrms). Wyjście słuchawkowe
miksera potrafi dać kilka razy więcej, więc:

- ustaw głośność słuchawek tak, żeby szczyty były tuż poniżej przesterowania,
- jeśli zakres regulacji jest niewygodny, dodaj na każdy kanał dzielnik
  10 kΩ / 10 kΩ, który obniża poziom o 6 dB.

## Do zrobienia

- [ ] sprawdzić multimetrem pola „+” i „−” modułu, potem połączyć z 5V i GND
- [ ] nagrać ciszę i ocenić, czy potrzebny jest filtr na linii 5 V
- [ ] firmware: odbiór I2S i zapis WAV na microSD
- [ ] wskaźnik poziomu i przesterowania na LCD
