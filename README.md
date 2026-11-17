# Uptime Monitor

A pocket-sized internet uptime monitor built on a $10 ESP32 board with a
1.14" colour screen. It pings my router and two public DNS servers every two
seconds, works out whether a failure is my ISP's fault or just local WiFi,
keeps a log of every outage on flash, and posts to Discord when the internet
comes back.

All firmware in this repository was written from scratch for this project. It
is not a fork of an existing monitor.

<!-- PHOTO: hero shot of the device on a desk showing the STATUS screen -->
![Uptime Monitor showing the STATUS screen](docs/photos/hero.jpg)

## What it does

- Sends ICMP echo requests to the default gateway, `1.1.1.1` and `8.8.8.8`
  every 2 seconds (raw sockets on lwIP, no ping library).
- Tells an **ISP outage** apart from a **WiFi** problem and only declares an
  outage after three failed rounds in a row, so a single lost packet never
  raises an alarm.
- Saves each outage (start time, duration, cause) to LittleFS. The last 200
  are kept and survive power loss.
- Shows live status, a latency graph, the outage log and uptime percentages
  on four screens.
- Optionally posts one Discord message when an ISP outage ends:
  `Internet was down 1m 12s (ISP side). Back at 14:03.`
- Configured from a phone through a captive portal. No WiFi password or
  webhook URL is ever stored in the source code.

## Hardware

| Part | Notes |
|---|---|
| LilyGO TTGO T-Display, or a clone such as the TENSTAR T-Display | Original ESP32 (not S3), 16 MB flash, CH9102F USB bridge |
| Screen (built in) | 1.14" ST7789, 135x240, used in landscape (240x135) |
| Buttons (built in) | GPIO0 and GPIO35 |
| USB-C cable and any 5 V supply | |

No wiring is needed. The display pins used are MOSI 19, SCLK 18, CS 5, DC 16,
RST 23 and backlight 4.

<!-- PHOTO: the bare board, front and back -->
![The T-Display board](docs/photos/board.jpg)

## Flashing

You need Python 3 and a USB cable. PlatformIO downloads the compiler and
libraries on the first build.

```
python -m venv .venv
.venv\Scripts\pip install platformio
.venv\Scripts\pio run -t upload
.venv\Scripts\pio device monitor
```

The upload port is set to `COM7` in `platformio.ini`; change `upload_port`
and `monitor_port` to match your machine, or delete those lines to let
PlatformIO auto-detect.

On Windows, `pio.ps1` wraps the same commands and keeps PlatformIO's
toolchains, caches and temp files on the project's drive instead of the
user profile: `.\pio.ps1 run -t upload`.

## First-time setup

1. Power the board. With no saved WiFi it starts an access point called
   **UptimeMonitor-Setup** and shows that name on screen.
2. Join it from a phone. A setup page opens by itself (or browse to
   `192.168.4.1`).
3. Pick your 2.4 GHz network, enter its password, and optionally paste a
   Discord webhook URL.
4. The board restarts and joins your network. If the password was wrong, the
   setup network comes back after 30 seconds.

When a webhook is saved for the first time the monitor posts
`Uptime monitor online` so you can confirm it works.

To wipe the saved WiFi and webhook, hold **both buttons for 5 seconds**. A
countdown appears on screen; release to cancel. (Press the buttons after the
screen lights up. Holding GPIO0 while plugging in puts an ESP32 into its
bootloader instead.)

<!-- PHOTO: phone showing the setup page next to the device -->
![Setup page on a phone](docs/photos/setup.jpg)

## The four screens

Button 1 moves to the next screen. Button 2 does something specific to the
screen you are on. Only the parts of the screen that changed are redrawn, so
nothing flickers.

### 1. STATUS

A large ONLINE (green) or OFFLINE (orange), the current internet latency,
packet loss over the last five minutes, and the latest reply time from the
router, `1.1.1.1` and `8.8.8.8`.

<!-- PHOTO: STATUS screen -->
![STATUS screen](docs/photos/screen-status.jpg)

### 2. LATENCY

A bar graph of internet latency. Green bars are scaled to the slowest reply
in view; a failed ping is a full-height orange bar. MIN, AVG, MAX and LOSS
for the range are along the bottom. Button 2 switches between the last
5 minutes and the last hour.

<!-- PHOTO: LATENCY screen -->
![LATENCY screen](docs/photos/screen-latency.jpg)

### 3. OUTAGES

The outage log, newest first: when it started, how long it lasted, and an
orange `ISP` or grey `WIFI` tag. Button 2 scrolls four rows at a time.

<!-- PHOTO: OUTAGES screen -->
![OUTAGES screen](docs/photos/screen-outages.jpg)

### 4. UPTIME

Uptime over the last 24 hours and 7 days, the number of ISP outages, the
longest one, and how long the monitor has been watching.

<!-- PHOTO: UPTIME screen -->
![UPTIME screen](docs/photos/screen-uptime.jpg)

## How outages are classified

Every 2 seconds one round of three pings goes out, each with a 1 second
timeout.

| Router | 1.1.1.1 / 8.8.8.8 | The round counts as |
|---|---|---|
| replies | at least one replies | OK |
| replies | both fail | ISP failure |
| fails | both fail | WIFI failure (local link only) |
| fails | at least one replies | OK (the reply had to come through the router) |

- An outage is declared after **3 consecutive failed rounds**, and is
  back-dated to the first of them.
- It ends at the first OK round. It is then written to the log, and if it was
  an ISP outage and a webhook is saved, one Discord message is sent.
- If the cause changes part-way through (say the ISP drops, then the WiFi
  does too), the outage is split so each log entry has the right tag.

Everything important in my house is wired, so **uptime percentages and outage
counts use ISP outages only**. WIFI events are listed in the log but never
affect the statistics.

Details worth knowing:

- Time comes from NTP (timezone America/New_York). If the board boots with
  no internet, monitoring starts straight away and any outage that ends
  before the clock is set is saved with the correct timestamp once NTP
  answers.
- While an outage is in progress a small marker file is refreshed once a
  minute. If the power goes out mid-outage, the next boot logs the outage
  with the duration seen up to that point.
- The monitor cannot observe anything while it is unpowered, so time spent
  switched off counts as neither up nor down in the log.

## Serial console

At 115200 baud the monitor prints one line per round and every state change:

```
[ping] router=2ms 1.1.1.1=14ms 8.8.8.8=13ms -> OK | ONLINE
[monitor] ISP outage declared
[monitor] ISP outage ended after 14s
[log] 11/16 14:02 14s ISP -> saved (3 stored)
```

Commands, for testing without unplugging a modem:

| Command | Effect |
|---|---|
| `sim isp` | Pretend both internet targets stopped replying |
| `sim wifi` | Pretend nothing replies |
| `sim off` | Back to real results |
| `log` | Print the outage log |
| `stats` | Print the uptime figures |
| `clearlog` | Erase the outage log |

## Project layout

| File | Job |
|---|---|
| `src/ping.cpp` | ICMP echo on raw sockets, in its own task |
| `src/monitor.cpp` | Outage logic and latency history |
| `src/storage.cpp` | Outage log on LittleFS |
| `src/stats.cpp` | Uptime percentages (ISP outages only) |
| `src/timekeeper.cpp` | NTP, timezone, time formatting |
| `src/settings.cpp` | WiFi and webhook settings in NVS |
| `src/portal.cpp` | Setup access point and captive portal |
| `src/alerts.cpp` | Discord webhook over HTTPS |
| `src/display.cpp` | Drawing primitives and change tracking |
| `src/screens.cpp` | The four screens |
| `src/buttons.cpp` | Debounce and the wipe gesture |
| `tools/` | Font and certificate generators, serial log reader |

Built with PlatformIO, the Arduino framework and
[TFT_eSPI](https://github.com/Bodmer/TFT_eSPI).

## Credits

The on-screen font is [JetBrains Mono](https://www.jetbrains.com/lp/mono/),
converted to bitmaps by `tools/make_fonts.py` and used under the SIL Open
Font License 1.1 (`tools/fonts/OFL.txt`).
