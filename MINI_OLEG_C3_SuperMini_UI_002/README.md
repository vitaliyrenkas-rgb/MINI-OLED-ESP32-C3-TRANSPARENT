# MINI OLEG — ESP32-C3 Super Mini + Transparent SSD1309

**Build:** `MINI-002`  
**Target board:** ESP32-C3 Super Mini від Tenstar Robot  
**Display:** Waveshare 1.51" Transparent OLED, SSD1309, 128×64, заводський 4-wire SPI

Це перший бойовий кандидат односторінкового Mini OLEG:

- годинник;
- дата українською;
- температура одним значенням;
- відносна вологість;
- одна погодна іконка;
- іконка фактичного стану Wi-Fi;
- жодної батареї, міста, англійського UI або перемикання сторінок.

## Підключення

| Waveshare OLED | ESP32-C3 Super Mini |
|---|---:|
| `VCC` | `3V3` |
| `GND` | `GND` |
| `CLK` | `GPIO4` |
| `DIN` | `GPIO6` |
| `CS` | `GPIO7` |
| `DC` | `GPIO3` |
| `RST` | `GPIO10` |

Дисплей лишається у заводському режимі **4-wire SPI**. Скетч використовує software SPI через U8g2 — той самий тип дисплейного шляху, який уже фізично пройшов sanity на LoLin.

## Як вводяться Wi-Fi та API key

У коді немає домашнього SSID, пароля або API-ключа. Вони вводяться через локальний Config Portal і зберігаються в NVS.

### Перший запуск

1. Залий скетч.
2. Mini OLEG автоматично створить Wi-Fi мережу:
   - SSID: `MINI-OLEG-SETUP`
   - пароль: `olegsetup`
3. Підключись до неї телефоном або ноутбуком.
4. Відкрий `http://192.168.4.1`.
5. Заповни:
   - Wi-Fi SSID;
   - пароль Wi-Fi;
   - OpenWeather API key;
   - локацію у форматі `Kyiv,UA`, `Lviv,UA` тощо.
6. Натисни **«Зберегти й перезапустити»**.

### Як відкрити портал повторно

Після появи основного UI затисни кнопку **BOOT** приблизно на 5 секунд.

**Не затискай BOOT під час подачі живлення або reset:** GPIO9 є boot-strap піном ESP32-C3, тому плата може зайти в режим прошивки замість запуску скетчу.

## Збереження даних

`Preferences` / NVS зберігає:

- SSID;
- пароль Wi-Fi;
- OpenWeather API key;
- погодну локацію;
- останню успішну температуру, вологість та погодний стан.

Тому після короткої відсутності інтернету UI може показати останню відому погоду, а Wi-Fi іконка покаже актуальний стан з’єднання.

## OpenWeather

Скетч використовує Current Weather API:

```text
https://api.openweathermap.org/data/2.5/weather
```

Параметри: `units=metric`, `lang=uk`. Погода запитується одразу після кожного успішного запуску й підключення до Wi-Fi, а далі — раз на годину. Після невдалого запиту повторна спроба виконується через 1 хвилину.

API key створюється в обліковому записі OpenWeather. Новий ключ інколи активується не миттєво; HTTP `401` у Serial за правильного ключа може означати, що він ще не активний.

## Бібліотеки

В Arduino IDE встановити:

- **U8g2** by olikraus;
- **ArduinoJson** by Benoit Blanchon.

Решта (`WiFi`, `HTTPClient`, `WebServer`, `DNSServer`, `Preferences`) входить до Arduino core for ESP32.

## Налаштування плати в Arduino IDE

Базовий варіант:

```text
Board: ESP32C3 Dev Module
USB CDC On Boot: Enabled
Flash Size: відповідно до конкретної плати
```

## Перед комітом у main

Цей build треба фізично перевірити на реальній Tenstar Robot ESP32-C3 Super Mini. Після PASS:

```bash
git add .
git commit -m "Add ESP32-C3 Super Mini transparent OLED baseline"
git push
```

Потім відвести feature-гілку для подальшого розвитку:

```bash
git switch -c feature/mini-oleg-single-screen
git push -u origin feature/mini-oleg-single-screen
```
