# Проверка MConnect 0.1.1

Дата: 13–14 октября 2026 года. Среда: macOS 26.6.2, Apple Silicon, Mac16,7.
DAW в ходе этой оптимизации не запускалась. Проверки используют программный
источник аудио, валидаторы плагинов и настоящие браузеры.

## Поставка

AU и VST3 для macOS 11+, universal `arm64 + x86_64`. Основная поставка:
`dist/MConnect-0.1.1-macos-universal.pkg`. Обычный macOS Installer устанавливает
оба формата для текущего пользователя; установочных скриптов нет. Документация,
исходники и полные лицензии устанавливаются в
`~/Library/Application Support/MConnect/`.

Для ручной установки сохранён `dist/MConnect-0.1.1-macos-universal.zip`
с Install.command. Плагины имеют локальную ad-hoc подпись. У `.pkg` нет
Developer ID Installer; нотарификация Apple здесь не выполнялась.

Установленные пути:

- `~/Library/Audio/Plug-Ins/Components/MConnect.component`
- `~/Library/Audio/Plug-Ins/VST3/MConnect.vst3`

Старый Install.command сохранял предыдущие версии с суффиксом `.backup-*`.
Новый Installer заменяет пакеты на месте; старые резервные копии сохраняются.
Пакет не содержит отдельного приложения или тестового модуля часов.

## Проверка установщика

14 октября: PASS для `tests/installer.py`. Проверены home domain, минимальная
macOS 11, universal AU/VST3, права исполнения, `codesign --verify --deep --strict`,
документы и лицензии, русский/английский ресурсы Installer. Исходники в `.pkg`
совпадают с репозиторием и побайтно присутствуют в обоих бинарниках.

Установка и повторная установка через `/usr/sbin/installer` в
`CurrentUserHomeDirectory` прошли успешно без root/sudo. Повторная установка
удаляет проверочные файлы, отсутствующие в новом пакете; установленные файлы
и подписи проверены. Релокация отключена, версия проверяется, используется
штатный `BundleOverwriteAction=upgrade`. Результат: `test-results/installer.log`.

AU и VST3 имеют отдельные внутренние компоненты/receipts. Это исправляет
обнаруженный при проверке случай, когда два одинаковых bundle ID в одном
PackageInfo оставляли устаревшие файлы VST3. Внешний установщик — один `.pkg`.

Визуальный проход по окнам Installer не выполнялся: Mac был заблокирован.
Защита Gatekeeper на чужом Mac и нотарификация не проверены.

## Проверки

| Проверка | Результат и границы |
| --- | --- |
| Core arm64 / Rosetta x86_64 | PASS: битовый float/double passthrough, bypass/offline, mono, state, конкурентный FIFO и overflow, нулевая задержка в DAW |
| SRC | PASS: 32/44.1/48/88.2/96/192 kHz → 48 kHz, разные тоны L/R, подавление алиасинга, сброс истории после 48 kHz passthrough |
| Некорректная конфигурация хоста | PASS: NaN sample rate и предельный размер блока не приводят к некорректной диагностике/переполнению расчётов |
| AU auval, arm64 / Rosetta x86_64 | PASS; это не проверка на отдельном Intel Mac |
| VST3 pluginval 1.0.4, strictness 5 | PASS: scan, создание/закрытие, editor, state, 44.1/48/96/192 kHz, блоки 64/256/1024/2048 |
| Chromium / WebKit, 48 kHz | PASS: декодированное стерео, autoplay fallback, автоматическое восстановление WebSocket и 3 ручных reconnect |
| Chromium, 44.1/96/192 kHz и 48 kHz с блоками 1024/2048 | PASS: те же проверки декодирования и переподключения |
| 4 параллельных слушателя | PASS; пятый подключается после освобождения слота |
| Page cache / устаревший pause | PASS: страница восстанавливает связь; старое событие не отключает играющий поток |
| Отключение сервера на 8 s | PASS: восстановление без нового нажатия |
| Live смена частоты, блок 2048 | PASS: 192 → 44.1 → 48 kHz, раздельные декодированные тоны |
| Остановка аудиоколбэков / закрытие с приёмником | PASS: пакеты тишины продолжаются; нормальное завершение без зависания |
| HTTP / WebSocket | PASS: ключ и Origin, встроенные исходники/лицензия, заголовки, неверные типы/JSON, 2000 уровней вложенности, фрагментированные/бинарные/70 KiB кадры, повторное освобождение слотов |

Тестовый источник генерирует 997 Hz слева и 1499 Hz справа. После WebRTC/Opus
проверяются амплитуда и разделение каналов. Наличие статуса «connected» само по
себе не считается успешным аудиотестом. Результаты:
`test-results/browser-matrix.json`, `robustness.json`, `protocol.log`,
`pluginval-0.1.1.log`, `auval-*.log`.

## Производительность

Измеритель сравнивает CPU одного закрытого тестового процесса при работающем
генераторе без слушателей и с одним Chromium-приёмником. Браузер работает в
другом процессе. Окна измерения — 6 s после прогрева. 100% означает одно ядро.

| Частота источника | Базовый / с передачей, % одного ядра | Добавочный CPU | RSS всего тестового процесса | Средний буфер браузера за окно |
| --- | --- | --- | --- | --- |
| 48 kHz | 0.83 / 2.66 | 1.83% | 66.6 MiB | 118.8 ms |
| 192 kHz | 0.67 / 4.50 | 3.83% | 66.8 MiB | 116.8 ms |

Получено 796 и 784 пакета соответственно, с 2 и 11 потерями. Замер проверяет
получение сотен пакетов и ненулевую декодированную энергию. Ненулевые потери
и короткое окно не позволяют считать это доказательством работы при любой
нагрузке. RSS включает тестовый JUCE-хост, генератор и сеть, а не только плагин.
Полные данные: `test-results/performance-48000.json` и `performance-192000.json`.

Буфер рассчитан по разности `jitterBufferDelay` и `jitterBufferEmittedCount`
за окно. Это **не полная акустическая задержка от DAW до телефона**. Начальная
подсказка 30 ms адаптируется к сети; браузер может установить больший буфер.
Размер блока DAW, аудиовыход телефона и Bluetooth добавляют задержку.

Без готовых слушателей аудиоколбэк не копирует звук и кодер не работает.
Индикаторы DAW вычисляются при открытом редакторе. Один кодер обслуживает всех
слушателей; FIFO фиксирован на 512 KiB. Сеть/кодирование остаются на worker.

## Особенности стенда

Скрытые процессы macOS могут наследовать ограничение точности таймеров. Это
вызывало отставание программного источника и аудиочасов WebKit при нормальном
получении RTP. Стенд удерживает локальную активность программного источника.
Для тестового браузера отдельно загружается `MConnectBrowserTiming`; через
`__XPC_DYLD_INSERT_LIBRARIES` настройка достигает его WebContent-процесса.

Модуль действует только в запущенных тестовых процессах. Настройки ОС/DAW и
обычных браузеров не меняются, модуль не связан с кодом плагина и не включён
в установку. Это настройка испытательного стенда, не средство улучшения
работы телефона. Исходник — `tests/MacosBrowserTiming.mm`.
Основания: [Apple activities](https://developer.apple.com/library/archive/documentation/Performance/Conceptual/power_efficiency_guidelines_osx/PrioritizeWorkAtTheAppLevel.html),
[WebKit audio polling](https://github.com/WebKit/WebKit/blob/main/Source/WebCore/platform/mediastream/libwebrtc/LibWebRTCAudioModule.cpp).

## Границы совместимости

- Реальный iPhone: пользователь подтвердил звук в версии 0.1.0. Повторной
  физической проверки 0.1.1 здесь не проводилось.
- Физический Android и отдельные DAW не проходили матрицу устройств.
  Chromium/WebKit на Mac не заменяют проверку каждого телефона.
- Windows VST3 / Linux VST3 и LV2 настроены в CMake; бинарники этих платформ
  на этом Mac не собирались. Pro Tools AAX требует отдельного Avid SDK.
- Opus 320 kbps — сжатие с потерями. Равенство lossless-качеству или задержке
  Mix to Mobile не заявляется.
- Требуется доступная локальная сеть. Браузер может требовать одно нажатие
  «Слушать»; фоновые вкладки и блокировка экрана управляются ОС телефона.

## Повторение

```sh
ctest --test-dir build -C Release --output-on-failure
arch -x86_64 build/MConnectTests_artefacts/Release/MConnectTests
auval -v aufx Mcnt Mcnn
arch -x86_64 /usr/bin/auval -v aufx Mcnt Mcnn
MCONNECT_TEST_TIMING_LIBRARY="$PWD/build/libMConnectBrowserTiming.dylib" \
MCONNECT_PLAYWRIGHT=/absolute/path/to/node_modules/playwright \
python3 tests/run_receivers.py
MCONNECT_PLAYWRIGHT=/absolute/path/to/node_modules/playwright node tests/robustness.cjs
MCONNECT_TEST_TIMING_LIBRARY="$PWD/build/libMConnectBrowserTiming.dylib" \
MCONNECT_PLAYWRIGHT=/absolute/path/to/node_modules/playwright \
node tests/performance.cjs 48000
```

ПО для испытаний не является runtime-зависимостью плагина.
