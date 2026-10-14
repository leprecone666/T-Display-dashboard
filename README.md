# T-Display dashboard

Информационный дашборд на **LilyGo T-Display** (ESP32, ST7789 135×240, CH9102) + агент на ПК с Windows.
ПК шлёт данные по USB (serial 115200), плата рисует их на экране.

## Страницы (IO35 листает, долгое нажатие включает автолистание, IO0 меняет яркость)

| # | Страница | Что показывает |
|---|---|---|
| 0 | PC | CPU, RAM, GPU, диск, скорость сети |
| 1 | Claude | статус Claude Code (WORKING / NEEDS YOU / DONE), инструмент, таймер |
| 2 | Usage | лимиты плана (5 ч, неделя) и заполнение контекста |
| 3 | Top | 4 самых загруженных процесса |
| 4 | GPU | температура, загрузка, VRAM, мощность, вентилятор, частота (NVIDIA) |
| 5 | Network | графики входящего/исходящего трафика, uptime |
| 6 | Ping | задержки до узлов из `pc/targets.json` |
| 7 | Clock | время, дата, погода (Киев, open-meteo.com) |
| 8 | Music | что играет в Windows (Spotify, YouTube и др.), кириллица транслитерируется |

Автодиммер: ночью (22–07), при блокировке ПК и при потере связи яркость плавно падает.

## Состав

- `src/main.cpp`, `platformio.ini` — прошивка (PlatformIO, Arduino, TFT_eSPI).
- `pc/agent.py` — агент: собирает данные и отправляет строками `PC,… CL,… US,… TP,… PG,… TM,… MD,… GP,… WX,… LK,…` (формат описан в шапке `main.cpp`).
- `pc/claude_hook.py` — хук Claude Code (`UserPromptSubmit`, `PreToolUse`, `Notification`, `Stop`), пишет состояние в `%TEMP%`.
- `pc/claude_statusline.py` — statusLine для лимитов (работает в терминальном `claude`; десктопное приложение его не вызывает).
- `pc/media.ps1` — чтение медиа-сессии Windows.
- `pc/targets.json` — до 4 хостов для страницы Ping.

## Установка

```
pip install pyserial psutil platformio
python -m platformio run
pythonw pc/agent.py
```

Хуки добавляются в `~/.claude/settings.json` (команда `python pc/claude_hook.py working|tool|waiting|done`).

## Прошивка этой платы — особенности

- Автосброс esptool по умолчанию не работает (DTR тоже сбрасывает чип). Нужен конфиг `esptool.cfg`:
  `custom_reset_sequence = D0|R1|W0.2|R0|W0.3` (только RTS), а вход в загрузчик — зажатая кнопка IO0.
- Агент открывает порт с `dtr=False, rts=False`, иначе плата сбрасывается.
- На дисплее нет кириллицы: тексты транслитерируются в `agent.py`.
- Классический ESP32 не умеет перезагружаться в загрузчик программно.
- На экземпляре автора линия EN имеет утечку на землю (~3 кОм): после сброса плату может понадобиться запустить, коснувшись EN проводом от 3V3. Лечится резистором 470–680 Ом между 3V3 и EN.
