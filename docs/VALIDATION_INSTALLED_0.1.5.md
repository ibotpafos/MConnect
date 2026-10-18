# Установленная MConnect 0.1.5: звук и восстановление — 2026-10-19

Проверены AU и VST3, установленные через GD Connect в пользовательские
`~/Library/Audio/Plug-Ins/Components` и `~/Library/Audio/Plug-Ins/VST3`.
DAW в этом прогоне не запускалась и не управлялась. Источник сигнала —
отдельный программный хост, не открывающий аудиоустройство.

## Подтверждение установленной копии

Новый `MConnectInstalledHarness` загружает указанный bundle через штатный
JUCE AudioPluginFormatManager. Он не линкует MConnect SharedCode и проверяет
путь реально загруженного Mach-O через dyld. URL подключения получен
распознаванием настоящего QR в нативном окне установленного плагина.
Оба формата сообщили версию 0.1.5.

| Формат | SHA-256 установленного Mach-O до и после проверки |
| --- | --- |
| AU | `094e45e2798da61a9cf6b762664016fb47ca32f7c210ea07a5cb10cb725fa439` |
| VST3 | `58083c19e16bb6a228b5ab8644e390e49a094a23e759629f7c7eed35e8b1d006` |

Хеши совпали до/после; строгая проверка обеих ad-hoc подписей прошла.
Производственные бинарники не пересобирались и не заменялись.

## Автоматические проверки

Источник: 48 kHz, блок 256, левый тон 997 Hz, правый 1499 Hz,
амплитуда 0.25. Приём — настоящий WebRTC/Opus и измерение декодированных
каналов через AudioContext, без подмены аудиоданных.

| Установленный формат | Chromium | WebKit |
| --- | --- | --- |
| VST3 | PASS | PASS |
| AU | PASS | PASS |

Во всех четырёх случаях проверены:

- Разные тоны L/R до обрыва, после разрыва WebSocket и после полной остановки
  сервера на восемь секунд. Повторное нажатие «Слушать» не требуется.
- Основной тон после длинного обрыва: примерно 0.250–0.251 по амплитуде;
  примесь тона другого канала меньше 0.005, при пороге проверки 0.02.
- Три ручных цикла остановки/запуска, autoplay fallback, отказ неверному
  pairing key и отсутствие ошибок JavaScript.
- Локальный UDP media transport. В Chromium адрес выбранного ICE peer
  совпал с LAN-адресом в QR; WebKit в этой среде не вернул адрес в статистике,
  поэтому для него отдельно подтверждены тип host и протокол UDP.

Установленный VST3 дополнительно прошёл `tests/robustness.cjs`:
четыре слушателя; подключение пятого после освобождения слота; адаптация
буфера к потерям и concealment; снижение буфера после восстановления;
page-cache restore; устаревшее pause-событие; длинный обрыв; смена частоты
192/44.1/48 kHz с блоком 2048 и повторным измерением стерео; тишина и
media keepalive при остановке источника; корректное завершение с активным
слушателем. Ошибок кодирования и ресемплинга нет.

Установленный VST3 также прошёл pluginval 1.0.4, strictness 5,
48 kHz / 256. Это дополнительная проверка установленной копии;
предыдущий strictness 10 описан в `VALIDATION.md` и не выдаётся за новый прогон.

## Настоящий iPhone

После автоматических проверок тестовый сигнал уменьшен до пика 0.04
(примерно −28 dBFS). Пользователь открыл QR в Safari на iPhone и подтвердил:
«Сигнал есть, после обрыва вернулся сам». Условие проверки: выключить Wi-Fi
на 8–10 секунд, включить обратно, оставить ту же страницу без перезагрузки
и повторного нажатия «Слушать».

Физическая проверка выполнена с установленным VST3. AU проверен
автоматически двумя браузерными движками. Модель iPhone и версия iOS
не фиксировались; физический Android в этом прогоне не проверялся.

## Повторение

Тестовая цель исключена из обычной сборки; она не попадает в установщик.
Нужны macOS с доступным графическим сеансом, Command Line Tools, CMake,
Python 3, Node.js и Playwright с Chromium/WebKit (в этом прогоне 1.62.1).
Запускайте команды из
корня репозитория на `/Volumes/D`. Зависимости C++ закреплены в
`dependencies.lock.json` и загружаются скриптом в следующем блоке.

В каждом терминале сначала настройте пути. `MCONNECT_PLAYWRIGHT` должен
указывать на установленный модуль Playwright; путь ниже соответствует
среде этого прогона. Для другой установки измените его и
`PLAYWRIGHT_BROWSERS_PATH` на соответствующие каталоги на `/Volumes/D`.

```sh
mkdir -p output/installed-check/tmp output/installed-check/cache test-results
export TMPDIR="$PWD/output/installed-check/tmp"
export XDG_CACHE_HOME="$PWD/output/installed-check/cache"
export CLANG_MODULE_CACHE_PATH="$XDG_CACHE_HOME/clang"
export PYTHONPYCACHEPREFIX="$XDG_CACHE_HOME/python"
export PLAYWRIGHT_BROWSERS_PATH=/Volumes/D/VEX-CI/cache/local-user/ms-playwright
export MCONNECT_PLAYWRIGHT='/Volumes/D/Codex Storage/runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright'
export MCONNECT_TEST_TIMING_LIBRARY="$PWD/build/libMConnectBrowserTiming.dylib"
export MCONNECT_INSTALLED_HOST="$PWD/build/MConnectInstalledHarness_artefacts/Release/MConnect installed test host.app/Contents/MacOS/MConnect installed test host"
export MCONNECT_INSTALLED_PLUGIN="$HOME/Library/Audio/Plug-Ins/VST3/MConnect.vst3"

python3 scripts/bootstrap.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMCONNECT_BUILD_TESTS=ON -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build --config Release --target MConnectInstalledHarness MConnectBrowserTiming -j 4
```

В первом терминале запустите установленный плагин. Хост остаётся работать,
пока открыто его окно; DAW не требуется. Перед каждым новым запуском удалите
оставшиеся info/control-файлы прошлого прогона.

```sh
rm -f output/installed-check/info.json output/installed-check/control.json
"$MCONNECT_INSTALLED_HOST" --plugin "$MCONNECT_INSTALLED_PLUGIN" \
  --seconds 900 --info "$PWD/output/installed-check/info.json" \
  --control "$PWD/output/installed-check/control.json"
```

Во втором терминале с теми же переменными дождитесь `info.json`, затем
запустите проверки последовательно. `MCONNECT_TEST_CONTROL` включает
восьмисекундную остановку сервера с проверкой декодированного звука после
восстановления. JSON с результатом выводится в stdout; сохраните его:

```sh
MCONNECT_TEST_CONTROL="$PWD/output/installed-check/control.json" \
  node tests/receiver.cjs output/installed-check/info.json chromium \
  > output/installed-check/chromium.jsonl
MCONNECT_TEST_CONTROL="$PWD/output/installed-check/control.json" \
  node tests/receiver.cjs output/installed-check/info.json webkit \
  > output/installed-check/webkit.jsonl
```

Для AU закройте тестовый хост, задайте в обоих терминалах
`MCONNECT_INSTALLED_PLUGIN="$HOME/Library/Audio/Plug-Ins/Components/MConnect.component"`
и повторите запуск. Чтобы повторить robustness на установленном VST3,
закройте ручной хост и используйте адаптер для существующего runner:

```sh
export MCONNECT_INSTALLED_PLUGIN="$HOME/Library/Audio/Plug-Ins/VST3/MConnect.vst3"
cat > output/installed-check/host.sh <<'SH'
#!/bin/sh
exec "$MCONNECT_INSTALLED_HOST" --plugin "$MCONNECT_INSTALLED_PLUGIN" "$@"
SH
chmod +x output/installed-check/host.sh
MCONNECT_TEST_HOST="$PWD/output/installed-check/host.sh" \
  MCONNECT_TEST_HOST_LOG="$PWD/output/installed-check/robust-host.log" \
  node tests/robustness.cjs > output/installed-check/robustness.jsonl
```

Robustness runner сам запускает и завершает хост. Готовый установленный
плагин должен быть версии 0.1.5; пересборка тестового хоста его не заменяет.
Команды управления: `stream`, `rate`, `block`, `pause`, `gain`, `quit`.
Для ручной проверки тихого сигнала есть `--gain 0.04`; автоматические
проверки амплитуды используют исходный уровень 0.25.

В тестовом хосте необходимы `PLUGINHOST_AU TRUE` и скрытая видимость
символов C++/Objective-C++. Начальная конфигурация с открытыми символами
JUCE аварийно завершалась при создании редактора; после изоляции символов
оба установленных формата прошли все перечисленные прогоны. Это исправление
тестового инструмента; производственный плагин не менялся.

В Git сохранён [машинный отчёт](validation/installed-0.1.5.json) с результатами
измерений. Локальные абсолютные пути обобщены, частный LAN-адрес заменён
результатом его сравнения с QR; ключи подключения и QR в Git не включены.

Исходный машинный отчёт и логи: `output/installed-stream-0.1.5/verification.json`,
`*-chromium.jsonl`, `*-webkit.jsonl`, `robustness.jsonl`, `pluginval.log`,
`iphone-confirmation.json`. Тестовые хосты и браузеры завершены штатно.
