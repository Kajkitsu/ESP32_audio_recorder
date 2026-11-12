# ESP32 audio recorder

Rejestrator stereo do miksera: sygnał z wyjścia słuchawkowego miksera trafia do
przetwornika ADC PCM1808, a ESP32-S3 odbiera go po I2S i zapisuje jako WAV na
karcie microSD.

Stan: jest tylko dokumentacja sprzętu, firmware jeszcze nie powstał.

## Sprzęt

| Element | Model |
| --- | --- |
| Płytka | Waveshare ESP32-S3-Touch-LCD-2 (LCD 240×320, slot microSD) |
| ADC | moduł PCM1808 z gniazdem USB Type-C (5 V) i wejściem AUX 3,5 mm |
| Źródło | wyjście słuchawkowe miksera, kabel jack stereo |

## Połączenia

| PCM1808 | ESP32-S3 | Uwagi |
| --- | --- | --- |
| 5V | 5V | albo osobny kabel do gniazda Type-C modułu, wtedy łączysz tylko GND |
| GND | GND | masa musi być wspólna |
| 3.3V | nie podłączać | wyjątek w sekcji „Zasilanie” |
| SCK | GPIO 18 | zegar systemowy (MCLK), bez niego ADC milczy |
| BCK | GPIO 16 | zegar bitowy |
| LRC | GPIO 17 | zegar kanałów (WS) |
| OUT | GPIO 21 | dane, poziom 3,3 V |

Numery GPIO to propozycja, nie zostały sprawdzone z dokumentacją płytki.
ESP32-S3 pozwala wyprowadzić I2S na dowolny wolny pin, więc wybierz cztery
dostępne na złączu i wpisz je w konfiguracji. GPIO 16, 17 i 21 są współdzielone
ze złączem kamery, czyli wolne, dopóki kamera nie jest podłączona.

Piny zajęte przez płytkę:

| Funkcja | GPIO |
| --- | --- |
| LCD i microSD (wspólne SPI) | 38, 39, 40, 41, 42, 45 |
| Dotyk i IMU (I2C) | 46, 47, 48 |
| Podświetlenie, pomiar baterii | 1, 5 |
| USB, UART | 19, 20, 43, 44 |
| Piny startowe | 0, 3 |

Z tej listy potwierdzone ze schematem są 41, 45, 46, 47 i 48, reszta wymaga
sprawdzenia.

Przewody I2S powinny mieć najwyżej kilka centymetrów; SCK niesie 12 MHz.

## Zasilanie

PCM1808 potrzebuje 5 V dla części analogowej i 3,3 V dla cyfrowej. Moduł
zasilany z 5 V powinien mieć własny stabilizator 3,3 V. Sprawdź to raz:

1. Podaj na moduł samo 5 V.
2. Zmierz napięcie między pinem 3.3V a GND.
3. Około 3,3 V oznacza, że stabilizator jest na płytce i pin 3.3V zostaje
   wolny. Nie łącz go z 3V3 płytki ESP.
4. Około 0 V oznacza, że stabilizatora nie ma. Podłącz wtedy pin 3.3V do 3V3
   płytki ESP.

## Filtr LC

Nie jest wymagany. PCM1808 ma wbudowany filtr antyaliasingowy i górnoprzepustowy,
a kondensatory sprzęgające są na module.

Filtr na linii 5 V ma sens dopiero wtedy, gdy w nagraniu ciszy słychać zakłócenia
(pisk, terkot przy transmisji WiFi): koralik ferrytowy lub dławik 10–47 µH
szeregowo, a za nim kondensator 100–470 µF do masy, tuż przy module.

Brum 50 Hz to pętla masy, nie zasilanie. Pomaga separator audio
z transformatorami albo zasilanie rejestratora z baterii.

## Poziom sygnału

Wejście PCM1808 przesterowuje się przy 3 Vpp (około 1 Vrms). Wyjście słuchawkowe
miksera potrafi dać kilka razy więcej, więc:

- ustaw głośność słuchawek tak, żeby szczyty były poniżej przesterowania,
- jeśli zakres regulacji jest niewygodny, dodaj na każdy kanał dzielnik
  10 kΩ / 10 kΩ, który obniża poziom o 6 dB.

## Konfiguracja I2S

| Parametr | Wartość |
| --- | --- |
| Rola ESP | master, odbiór (RX) |
| Format | I2S Philips, stereo |
| Próbkowanie | 48 kHz |
| Dane | 24 bity w slocie 32-bitowym |
| MCLK | 256 × fs = 12,288 MHz |

PCM1808 nie przyjmuje ramek 32 BCK, dlatego slot 16-bitowy nie zadziała.

Zworki modułu: FMT = 0 (format I2S), MD0 = MD1 = 0 (tryb slave). Poziomy muszą
być ustawione przed włączeniem zasilania.

## Do zrobienia

- [ ] potwierdzić piny złącza płytki i wpisać ostateczne GPIO
- [ ] sprawdzić pin 3.3V modułu multimetrem
- [ ] firmware: odbiór I2S i zapis WAV na microSD
- [ ] wskaźnik poziomu i przesterowania na LCD
