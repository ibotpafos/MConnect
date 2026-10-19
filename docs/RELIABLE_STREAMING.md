# Повторная передача и автоматический буфер

Локальная реализация от 2026-10-19. Дополняет исходное исследование
[POOR_NETWORK.md](POOR_NETWORK.md). Опубликованные пакеты и установленный
плагин этой проверкой не заменяются. DAW не запускалась.

## Результаты

Все девять принятых сетевых прогонов используют настоящие пакеты и нативные
AU/VST3 из новой сборки. В установившихся плохих окнах и после восстановления
маскировка пропусков составила 0%. При первом ухудшении остаётся время
адаптации: максимальная доля маскировки за этот этап — 0,756%.

| Формат | Браузер | Ухудшение | После адаптации |
| --- | --- | --- | --- |
| VST3 | Chromium, WebKit | 10% потерь | 0% маскировки |
| VST3 | Chromium, WebKit | Каждый пятый пакет +350 мс | 0% |
| VST3 | Chromium, WebKit | Потери +350 мс + RTT 160 мс | 0% |
| VST3 | Chromium, WebKit | То же с момента подключения | 0% |
| AU | WebKit | То же с момента подключения | 0% |

Также прошли:

- Четыре проверки восстановления: оба формата в Chromium и WebKit после
  разрыва сигналинга и полного отключения сервера на восемь секунд, без нового
  нажатия «Слушать». Stop освобождает слот и прекращает отправку, включая
  отмену незавершённого стартового накопления.
- Четыре одновременных слушателя и освобождение занятого слота для пятого;
  рост/уменьшение буфера, page cache, позднее событие pause, живые переходы
  192/44,1/48 kHz, блок 2048, тишина при остановленном источнике и штатное
  завершение с активными соединениями.
- CTest: core/editor/release_tool на Apple Silicon; core под Rosetta x86_64.
  Исходный сигнал DAW остаётся неизменным, задержка в DAW — ноль.
- HTTP/WS: ключ, Origin, неправильные типы/глубина JSON, размер и фрагментация
  сообщений, повторное освобождение слотов. Пять вариантов согласования NACK.
- VST3 pluginval: strictness 10, два повтора, 48/192 kHz, блоки 256/2048,
  случайный порядок с seed 101019. Обе universal-сборки имеют корректную
  локальную ad-hoc подпись; установленный плагин не заменялся.

На Apple M4 Pro (14 логических CPU) обычная передача добавила примерно 0,83%
одного ядра относительно того же тестового хоста без слушателя: 1,67% → 2,50%.
При 10% потерь весь тестовый процесс, включая генератор, использовал 3,39%
одного ядра за десятисекундное окно. За это время кодировалось и отправлялось
1002 исходных стереопакета, ошибки/потери исходных кадров — ноль. Это замеры
процесса с тестовым хостом, а не гарантия нагрузки на любом компьютере.

Полные измерения, хэши проверенных AU/VST3 и исходников:
[машинный отчёт](validation/reliable-streaming-2026-10-19.json).
Ссылки подключения, QR, IP-адреса и пользовательские пути из него удалены.
Артефакты остаются в `output/reliable-stream-2026-10-19/final/` на диске D.
Версия 0.1.6 в метаданных — локальная настройка проекта; новый релиз не
назначался и не публиковался.

## Поведение

Приёмник предлагает generic NACK для Opus; нативный отправитель подтверждает
его только для предложенного аудиопотока. При пропуске RTP браузер запрашивает
повтор, а существующий `RtcpNackResponder` отправляет уже закодированный пакет
из ограниченного кэша на 512 пакетов. В аудиоколбэке DAW новых операций нет,
повторное кодирование не требуется. Для старого приёмника без NACK кэш не
включается. `nack pli` и обратная связь другого кодека не включают audio NACK.
Допустимая wildcard-запись обратной связи преобразуется в выбранный Opus PT
перед обработкой библиотекой, которая сама её не принимает.

После разрешения воспроизведения отправитель сначала накапливает 600 мс
закодированного звука и передаёт начальный запас в правильном порядке, затем
работает в обычном темпе. Это заполняет настоящий пакетный буфер браузера:
при уже плохой сети одна подсказка на 600 мс оставляла его почти пустым.
Стартовая очередь ограничена 60 пакетами по 4096 байт на слушателя, память
выделяется до передачи, а обработка выполняется в сетевом потоке. При Stop,
потере готовности или разрыве исходного аудио незавершённый запас сбрасывается.
Это даёт короткое ожидание звука при старте, без новых действий пользователя
и без задержки сигнала в DAW.

Телефон начинает с подсказки буфера 300 мс. Каждые две секунды приёмник
проверяет интервальные потери/маскировку, джиттер и RTT активного соединения.
При ухудшении запас быстро растёт до 600 мс. После 12 секунд здорового звука
уменьшается максимум на 20 мс за такой интервал. Используется
`jitterBufferTarget`, затем `playoutDelayHint`, если доступен; иначе остаётся
встроенное управление буфером браузера. Новых кнопок, настроек и фоновых
приложений нет. Число миллисекунд — подсказка браузеру, а не гарантия сквозной
задержки. Полное отсутствие сети этим не компенсируется; работает прежнее
автовосстановление соединения.
Семантика подсказки описана в [WebRTC Extensions](https://w3c.github.io/webrtc-extensions/#dom-rtcrtpreceiver-jitterbuffertarget).

## Метод проверки

`tests/network-reliability.cjs` пропускает настоящее зашифрованное RTP через
две локальные UDP-точки. Меняются только ICE-адреса в тестовом сигналинге.
Не подменяются SDP feedback, `getStats()` и сеттеры буфера. Используются
страница и нативный ответ из новой сборки.

- `loss`: примерно 10% RTP действительно отбрасывается, включая повторы.
- `jitter`: каждая пятая RTP-датаграмма задерживается на 350 мс.
- `combined`: оба ухудшения одновременно, плюс 80 мс в каждом направлении,
  включая RTCP-запросы; добавленный RTT составляет 160 мс.

Каждый прогон: 8 с контроля, 20 с адаптации, 12 с установившейся плохой сети,
16 с восстановления. Начальные сбои учитываются отдельно: буфер не может
исправить уже проигранный пропуск. Измеряются реальные счётчики и фактическое
возвращение ранее отброшенных RTP sequence numbers. Декодируются два канала
997/1499 Гц; проверяются их амплитуды, отсутствие смешивания и энергия звука.
Соединение должно оставаться подключённым. Условия принятия включают здоровый
контроль, меньше 1% маскировки в установившемся плохом окне и меньше 0,5%
после восстановления. Маскировка — доля `concealedSamples`, не число слышимых
щелчков и не оценка качества на слух.

Восемь основных записей VST3 сохраняют исходный замер корреляции в одном окне.
Текущий сетевой тест, дополнительный AU-прогон и проверки восстановления
используют три независимых окна: энергия, спектр и отсутствие смешивания
проверяются в каждом, исходные пороги тонов — минимум в двух. Все окна
сохраняются. Это учитывает изменение фазы при временной подстройке NetEq;
одиночный 43-мс замер может занизить корреляцию при правильной частоте и
непрерывной энергии. Счётчики потерь/маскировки при этом не подменяются.

Начальная подсказка 100 мс была отклонена по результату комбинированного
теста: несмотря на запрос 600 мс, во время непрерывных сбоев браузер не успел
накопить запас (23,70% маскировки в установившемся окне). Диагностический
вариант с началом от 300 мс дал 0,083%. Поэтому окончательная реализация
начинает с 300 мс. Диагностические прогоны не включаются в приёмку новой сборки.

На macOS тестовый `libMConnectBrowserTiming.dylib` применяется и к программному
источнику, и к headless-браузеру. Иначе фоновые таймеры источника могут быть
сильно замедлены системой, что даёт ложное ухудшение даже в контрольном окне.
Эта библиотека не входит в плагин и не устанавливается пользователю.
Перед измерениями тест дополнительно ждёт три секунды устойчивого
декодирования не менее 40 тысяч сэмплов/с и достаточной энергии. Прогон WebKit
с медленно стартовавшим headless-аудиодвижком был отклонён как неверный
контроль, а не принят за реальный сбой сети или успешную проверку.

## Повторение

Все рабочие каталоги и кэши на этом Mac располагаются на `/Volumes/D`.
Нужны уже используемые проектом Playwright и браузеры; новых зависимостей нет.
Сначала собрать `MConnectHarness`, `MConnectTests`, `MConnectEditorTests`,
`MConnectBrowserTiming`, `MConnectInstalledHarness`, AU и VST3; запустить
CTest. Затем в первом терминале загрузить именно новый VST3, без установки:

```sh
cd /Volumes/D/Projects/PLUGINS/MConnect
mkdir -p output/reliable-stream/tmp
export TMPDIR="$PWD/output/reliable-stream/tmp"
export XDG_CACHE_HOME=/Volumes/D/VEX-CI/cache/xdg
export CLANG_MODULE_CACHE_PATH=/Volumes/D/VEX-CI/cache/clang
export PYTHONPYCACHEPREFIX=/Volumes/D/VEX-CI/cache/python
export MCONNECT_TEST_TIMING_LIBRARY="$PWD/build/libMConnectBrowserTiming.dylib"
DYLD_INSERT_LIBRARIES="$MCONNECT_TEST_TIMING_LIBRARY" \
  'build/MConnectInstalledHarness_artefacts/Release/MConnect installed test host.app/Contents/MacOS/MConnect installed test host' \
  --plugin "$PWD/build/MConnect_artefacts/Release/VST3/MConnect.vst3" \
  --no-editor --rate 48000 --block 256 --seconds 1200 \
  --info "$PWD/output/reliable-stream/info.json" \
  --control "$PWD/output/reliable-stream/control.json"
```

Для нового AU указать `build/MConnect_artefacts/Release/AU/MConnect.component`
в `--plugin` и добавить `--register-au`. При необходимости сначала подписать
этот собственный тестовый bundle командой `codesign --force --sign -`.
Опция регистрирует factory точного файла под частными тестовыми ID только
в процессе хоста (`AudioComponentRegister`); системная регистрация и папки
установленных плагинов не меняются. Обычный загрузчик JUCE выбирает AU по
системному реестру даже при переданном пути и мог бы проверить старую версию.
Хост дополнительно проверяет путь загруженного образа.

Во втором терминале, задав такие же переменные каталогов и пути к установленному
Playwright/его браузерам (пути ниже соответствуют этой машине):

```sh
export MCONNECT_PLAYWRIGHT='/Volumes/D/Codex Storage/runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright'
export PLAYWRIGHT_BROWSERS_PATH=/Volumes/D/VEX-CI/cache/local-user/ms-playwright
export MCONNECT_TEST_TIMING_LIBRARY="$PWD/build/libMConnectBrowserTiming.dylib"
node tests/nack-negotiation.cjs output/reliable-stream/info.json
node tests/network-reliability.cjs output/reliable-stream/info.json chromium loss
node tests/network-reliability.cjs output/reliable-stream/info.json chromium jitter
node tests/network-reliability.cjs output/reliable-stream/info.json chromium combined
node tests/network-reliability.cjs output/reliable-stream/info.json webkit loss
node tests/network-reliability.cjs output/reliable-stream/info.json webkit jitter
node tests/network-reliability.cjs output/reliable-stream/info.json webkit combined
node tests/network-reliability.cjs output/reliable-stream/info.json chromium combined --bad-start
node tests/network-reliability.cjs output/reliable-stream/info.json webkit combined --bad-start
MCONNECT_TEST_CONTROL="$PWD/output/reliable-stream/control.json" \
  node tests/receiver.cjs output/reliable-stream/info.json chromium
MCONNECT_TEST_CONTROL="$PWD/output/reliable-stream/control.json" \
  node tests/receiver.cjs output/reliable-stream/info.json webkit
```

Прогоны последовательные. Использовать код возврата и `result: PASS`, а не
только наличие JSON: при провале измерения также сохраняются для диагностики.
`tests/nack-negotiation.cjs` отдельно проверяет настоящий нативный ответ для
обычного NACK, wildcard, отсутствия NACK, `nack pli` и другого PT.
Старый `network-impairment.cjs` сохраняется для воспроизведения исходных
экспериментов; его SDP/buffer overrides не являются приёмкой реализации.

После проверок штатно завершить свой тестовый хост через `{"quit":true}` в
файле управления; не оставлять сигнал включённым. QR/пароль подключения из
`info.json` и реальные LAN-адреса не включать в публичные отчёты.

Затем отдельно запустить lifecycle/CPU-проверки: им нужен исполняемый wrapper,
который передаёт `--plugin` тестовому хосту и сохраняет его остальные аргументы.
Например, в `output/reliable-stream/vst3-host.sh`:

```sh
#!/bin/zsh
mconnectRoot='/Volumes/D/Projects/PLUGINS/MConnect'
exec env DYLD_INSERT_LIBRARIES="$mconnectRoot/build/libMConnectBrowserTiming.dylib" \
  "$mconnectRoot/build/MConnectInstalledHarness_artefacts/Release/MConnect installed test host.app/Contents/MacOS/MConnect installed test host" \
  --plugin "$mconnectRoot/build/MConnect_artefacts/Release/VST3/MConnect.vst3" "$@"
```

Сделать wrapper исполняемым, сохранить переменные каталогов/Playwright из
примеров выше и выполнить последовательно:

```sh
chmod +x output/reliable-stream/vst3-host.sh
MCONNECT_TEST_HOST="$PWD/output/reliable-stream/vst3-host.sh" node tests/robustness.cjs
MCONNECT_TEST_HOST="$PWD/output/reliable-stream/vst3-host.sh" node tests/performance.cjs 48000
```

Проверка pluginval при наличии того же локального валидатора:

```sh
output/update-tools/pluginval.app/Contents/MacOS/pluginval \
  --strictness-level 10 --sample-rates 48000,192000 --block-sizes 256,2048 \
  --repeat 2 --randomise --random-seed 101019 --timeout-ms 60000 \
  --output-dir "$PWD/output/reliable-stream/pluginval" \
  --validate "$PWD/build/MConnect_artefacts/Release/VST3/MConnect.vst3"
```

## iPhone

Плохая сеть воспроизводится программным UDP-прокси на Mac. Конкретный роутер,
радиопомехи, Android и другие операционные системы в этой проверке не
испытывались. Успех модели не означает отсутствие сбоев при любой сети.

Физическая проверка Safari на iPhone отложена пользователем до появления
телефона под рукой. Desktop WebKit не заменяет такую проверку. Для неё
запустить новый тестовый хост с окном (без `--no-editor`), открыть актуальный
QR в Safari, нажать «Слушать», проверить звук и восстановление после
отключения Wi-Fi на 8–10 секунд без повторного нажатия/перезагрузки.
До подтверждения не считать тест нового кода на iPhone пройденным.
