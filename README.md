# ESP32 audio recorder

Rejestrator stereo do miksera: sygnał z wyjścia słuchawkowego miksera trafia do
przetwornika ADC PCM1808, a ESP32-S3 odbiera go po I2S i zapisuje jako WAV na
karcie microSD.

Stan: jest tylko dokumentacja sprzętu, firmware jeszcze nie powstał.

## Sprzęt

| Element | Model |
| --- | --- |
| Płytka | Waveshare ESP32-S3-Touch-LCD-2 (LCD 240×320, slot microSD) |
| ADC | czarny moduł „PCM1808 IIS” z gniazdem USB Type-C, jackiem 3,5 mm i generatorem kwarcowym |
| Źródło | wyjście słuchawkowe miksera, kabel jack stereo (TRS) |

Jeden moduł nagrywa oba kanały, drugi nie jest potrzebny.

## Najpierw: tryb pracy modułu

Moduł ma własny generator, więc najpewniej pracuje jako master i sam wytwarza
zegary. To wariant o najlepszej jakości, bo zegar kwarcowy ma mniejszy jitter
niż zegar z ESP32-S3. Opisu ze sklepu nie udało się odczytać, dlatego sprawdź
tryb multimetrem:

1. Zasil sam moduł przez USB Type-C, bez połączeń z ESP.
2. Zmierz napięcie stałe między GND a pinami LRCK, BCK i MCK.
3. Około 1,6 V na LRCK i BCK oznacza tryb master (wariant A).
4. Około 0 V na LRCK i BCK oznacza tryb slave (wariant B).

Przy okazji zmierz VDD33. Około 3,3 V potwierdza, że moduł ma własny
stabilizator.

## Połączenia

Wszystkie użyte piny są na złączu P1 płytki Waveshare i są wolne, dopóki nie
jest podłączona kamera.

| PCM1808 | Wariant A: moduł master | Wariant B: moduł slave |
| --- | --- | --- |
| GND | GND | GND |
| VDD33 | nie podłączać | nie podłączać |
| LRCK | GPIO 2 (wejście) | GPIO 2 (wyjście) |
| BCK | GPIO 4 (wejście) | GPIO 4 (wyjście) |
| DATA | GPIO 6 (wejście) | GPIO 6 (wejście) |
| MCK | nie podłączać | GPIO 18 (wyjście) |

Zasilanie modułu: 5 V przez jego gniazdo USB Type-C. Na złączu z sygnałami nie
ma pinu 5 V. Zasil moduł i płytkę ESP z tego samego źródła (jeden powerbank lub
ładowarka), a masy połącz przewodem GND.

Pin VDD33 podłącz do 3V3 płytki ESP tylko wtedy, gdy pomiar pokazał na nim 0 V.

Przewody I2S powinny mieć najwyżej kilka centymetrów.

## Konfiguracja I2S

| Parametr | Wariant A | Wariant B |
| --- | --- | --- |
| Rola ESP | slave, odbiór | master, odbiór |
| Format | I2S Philips, stereo | I2S Philips, stereo |
| Dane | 24 bity w slocie 32-bitowym | 24 bity w slocie 32-bitowym |
| Próbkowanie | ustala moduł, do zmierzenia na LRCK | 48 kHz |
| MCLK | nieużywany | 256 × fs = 12,288 MHz |

PCM1808 pracuje z ramką 64 BCK, dlatego slot 16-bitowy nie zadziała.

Żadnych zworek nie trzeba dodawać.

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
a moduł ma własny stabilizator i kondensatory.

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

- [ ] zmierzyć multimetrem tryb modułu (LRCK, BCK) i pin VDD33
- [ ] odczytać napis na generatorze i opisy drugiego, 4-pinowego złącza modułu
- [ ] firmware: odbiór I2S i zapis WAV na microSD
- [ ] wskaźnik poziomu i przesterowania na LCD
