# Uptime Sentinel

A tiny internet uptime monitor that runs on a $10 ESP32 board. It pings your
router and two public servers every two seconds, works out whether a failure
is your ISP's fault or just your WiFi, keeps a log of every outage, and can
message you on Discord when the internet comes back.

No account, no cloud service, no subscription. Plug it into any USB charger
and leave it running.

<!-- PHOTO: hero shot of the device on a desk showing the STATUS screen -->
![Uptime Sentinel showing the STATUS screen](docs/photos/hero.jpg)

**Contents**

- [What it does](#what-it-does)
- [What you need](#what-you-need)
- [Step 1: Find your board](#step-1-find-your-board)
- [Step 2: Install the tools](#step-2-install-the-tools)
- [Step 3: Download this project](#step-3-download-this-project)
- [Step 4: Flash the board](#step-4-flash-the-board)
- [Step 5: Connect it to your WiFi](#step-5-connect-it-to-your-wifi)
- [Step 6 (optional): Discord alerts](#step-6-optional-discord-alerts)
- [Using it](#using-it)
- [The four screens](#the-four-screens)
- [How outages are classified](#how-outages-are-classified)
- [Troubleshooting](#troubleshooting)
- [Serial console](#serial-console)
- [Adding another board](#adding-another-board)
- [How the code is organised](#how-the-code-is-organised)

## What it does

- Pings your router, `1.1.1.1` (Cloudflare) and `8.8.8.8` (Google) every
  2 seconds.
- Tells an **ISP outage** (router answers, internet does not) apart from a
  **WiFi problem** (router does not answer).
- Waits for three failed rounds in a row before calling it an outage, so one
  lost packet never raises a false alarm.
- Saves every outage (start time, duration, cause) to the board's flash
  memory. The last 200 are kept and survive power cuts.
- Shows live status, a latency graph, the outage log and uptime percentages
  on four screens.
- Optionally posts one Discord message when an ISP outage ends:
  `Internet was down 1m 12s (ISP side). Back at 14:03.`
- Set up from your phone. Your WiFi password and webhook URL are stored only
  on the device, never in the code.

All firmware here was written from scratch for this project.

## What you need

- One supported ESP32 board (see the table in Step 1). About $10.
- A USB data cable that fits it. Some cheap cables are charge-only and will
  not work for flashing.
- A computer running Windows, macOS or Linux, for the one-time flashing.
- A 2.4 GHz WiFi network. ESP32 boards cannot join 5 GHz-only networks.
- Any USB charger or USB port to power it afterwards.

No soldering and no wiring.

## Step 1: Find your board

Find your board in this table and note its **build name**. You will type it
in Step 4.

| Board | Build name | Screen | Controls | Status |
|---|---|---|---|---|
| LilyGO TTGO T-Display, or a clone such as TENSTAR (16 MB flash) | `tdisplay` | 240x135 | 2 buttons | Tested on real hardware |
| LilyGO TTGO T-Display (4 MB flash) | `tdisplay-4mb` | 240x135 | 2 buttons | Same code as above; compiles |
| LilyGO T-Display-S3 | `tdisplay-s3` | 320x170 | 2 buttons | Compiles; not yet tested on hardware |
| Cheap Yellow Display (ESP32-2432S028R, one USB port) | `cyd` | 320x240 | Touch + BOOT button | Compiles; not yet tested on hardware |
| Cheap Yellow Display with two USB ports | `cyd2usb` | 320x240 | Touch + BOOT button | Compiles; not yet tested on hardware |
| Any ESP32 dev board, no screen | `esp32-headless` | none | BOOT button | Compiles; not yet tested on hardware |
| Any ESP32-S3 dev board, no screen | `esp32s3-headless` | none | BOOT button | Compiles; not yet tested on hardware |
| Any ESP32-C3 dev board, no screen | `esp32c3-headless` | none | BOOT button | Compiles; not yet tested on hardware |

Notes:

- **Not sure whether your T-Display is 4 MB or 16 MB?** Use `tdisplay-4mb`.
  It works on both.
- **"Not yet tested"** means the build compiles and uses the board's
  published pinout, but nobody has confirmed it on that hardware yet. If you
  try one, please open an issue saying whether it worked. If the picture is
  upside down or the colours are wrong, see [Troubleshooting](#troubleshooting).
- **No screen?** The headless builds do everything except draw: pinging,
  outage logging and Discord alerts all work, and you can read the log over
  USB (see [Serial console](#serial-console)).
- **Different board?** See [Adding another board](#adding-another-board).

<!-- PHOTO: the bare board, front and back -->
![The T-Display board](docs/photos/board.jpg)

## Step 2: Install the tools

You need two free programs.

1. **Visual Studio Code**: download and install it from
   <https://code.visualstudio.com>.
2. **PlatformIO**: open VS Code, click the Extensions icon in the left bar
   (four squares), search for `PlatformIO IDE`, and click **Install**. Wait
   for it to finish, then restart VS Code when it asks.

If your computer does not recognise the board when you plug it in (no new
COM port on Windows), install the USB driver for its chip:

- **CH340 / CH9102** (most clones, CYD, TENSTAR): search for "CH343 driver"
  on the WCH website, <https://www.wch-ic.com>.
- **CP210x** (some older boards): search for "CP210x VCP driver" on
  <https://www.silabs.com>.

<details>
<summary>Prefer the command line?</summary>

With Python 3 installed:

```
python -m venv .venv
.venv\Scripts\pip install platformio          (Windows)
.venv/bin/pip install platformio              (macOS / Linux)
```

Then use `.venv\Scripts\pio` (or `.venv/bin/pio`) wherever this guide says
`pio`. On Windows, the included `pio.ps1` does the same and keeps
PlatformIO's downloads on the project's drive instead of your user profile.
</details>

## Step 3: Download this project

**Easy way:** on this page click the green **Code** button, then
**Download ZIP**. Unzip it somewhere you can find it.

**With Git:**

```
git clone https://github.com/TradeSolana/uptime-sentinel.git
```

Then in VS Code choose **File > Open Folder** and pick the `uptime-sentinel`
folder. PlatformIO takes a minute to set itself up the first time.

## Step 4: Flash the board

1. Plug the board into your computer with the USB cable.
2. In VS Code open a terminal: **Terminal > New Terminal**.
3. Type this, replacing `tdisplay` with your build name from Step 1, and
   press Enter:

   ```
   pio run -e tdisplay -t upload
   ```

4. Wait. The first run downloads the compiler and takes several minutes.
   Later runs take under a minute.
5. When you see `SUCCESS`, the board restarts on its own and shows a
   **SETUP** screen.

If the upload cannot find or connect to the board:

- Hold the **BOOT** button on the board while the terminal says
  `Connecting...`, and let go once it starts writing.
- To pick the port yourself, add it to the command:
  `pio run -e tdisplay -t upload --upload-port COM7` (Windows) or
  `--upload-port /dev/ttyUSB0` (Linux) or `/dev/cu.usbserial-...` (macOS).

## Step 5: Connect it to your WiFi

1. The board creates its own WiFi network called **UptimeMonitor-Setup**.
   The screen shows that name.
2. On your phone, open WiFi settings and join **UptimeMonitor-Setup**. It
   has no password.
3. A setup page should open by itself. If it does not, open your browser and
   go to `192.168.4.1`. (If your phone warns that the network has no
   internet, choose to stay connected.)
4. Pick your home WiFi from the list and type its password.
5. Optional: paste a Discord webhook URL (see Step 6). You can leave it
   empty.
6. Tap **Save and connect**.

The board restarts, joins your WiFi and starts monitoring. If the password
was wrong, the setup network comes back after about 30 seconds so you can
try again.

<!-- PHOTO: phone showing the setup page next to the device -->
![Setup page on a phone](docs/photos/setup.jpg)

## Step 6 (optional): Discord alerts

A webhook lets the monitor post into one Discord channel. It is not a bot
and cannot read anything.

1. In Discord, open a server you own (or create a private one).
2. Right-click the channel you want alerts in and choose **Edit Channel**.
3. Go to **Integrations > Webhooks > New Webhook**.
4. Click the new webhook, then **Copy Webhook URL**.
5. Paste that URL into the setup page in Step 5.

When a webhook is saved for the first time the monitor posts
`Uptime monitor online`, so you know it works. After that it posts once each
time an ISP outage ends. WiFi-only problems never send a message.

Treat the webhook URL like a password: anyone who has it can post to that
channel. It is stored only on the device.

To add or change the webhook later, wipe the settings (below) and run the
setup again.

## Using it

| Board | Next screen | Screen action | Wipe settings |
|---|---|---|---|
| T-Display, T-Display-S3 | Button 1 | Button 2 | Hold both buttons 5 s |
| Cheap Yellow Display | Tap the screen | Touch and hold about 1 s | Hold BOOT 5 s |
| Headless boards | n/a | n/a | Hold BOOT 5 s |

On one-button boards, a short press of BOOT also moves to the next screen
and a longer press (about 1 second) is the screen action.

**Screen action** means: on the LATENCY screen it switches between the last
5 minutes and the last hour; on the OUTAGES screen it scrolls four rows down
(and wraps back to the newest).

**Wiping settings** erases the saved WiFi and webhook and returns to the
setup screen. A countdown appears; let go to cancel. The outage log is kept.
Press the buttons after the board has started: holding BOOT while plugging
in puts an ESP32 into its bootloader instead.

## The four screens

Only the parts of the screen that changed are redrawn, so nothing flickers.

### 1. STATUS

A large ONLINE (green) or OFFLINE (orange), the current internet latency,
packet loss over the last five minutes, and the latest reply time from the
router, `1.1.1.1` and `8.8.8.8`.

<!-- PHOTO: STATUS screen -->
![STATUS screen](docs/photos/screen-status.jpg)

### 2. LATENCY

A bar graph of internet latency. Green bars are scaled to the slowest reply
in view; a failed ping is a full-height orange bar. MIN, AVG, MAX and LOSS
for the range are along the bottom.

<!-- PHOTO: LATENCY screen -->
![LATENCY screen](docs/photos/screen-latency.jpg)

### 3. OUTAGES

The outage log, newest first: when it started, how long it lasted, and an
orange `ISP` or grey `WIFI` tag.

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

- An outage is declared after **3 failed rounds in a row**, and is
  back-dated to the first of them.
- It ends at the first OK round. It is then written to the log, and if it
  was an ISP outage and a webhook is saved, one Discord message is sent.
- If the cause changes part-way through (the ISP drops, then the WiFi does
  too), the outage is split so each log entry has the right tag.

**Uptime percentages and outage counts use ISP outages only.** WIFI events
are listed in the log but never affect the statistics. The idea is that the
monitor sits on WiFi while the things you care about may be wired, so a weak
WiFi signal at the monitor should not count against your ISP.

Good to know:

- **Time** comes from the internet (NTP). The timezone is America/New_York;
  to change it, edit `TIMEZONE` in `src/config.h` (it is a POSIX TZ string,
  for example `GMT0BST,M3.5.0/1,M10.5.0` for the UK) and flash again.
- **Booting with no internet** is fine. Monitoring starts straight away and
  any outage that ends before the clock is set is saved with the right
  timestamp once the time arrives.
- **Power cut during an outage:** a small marker is refreshed once a minute
  while an outage lasts, so the next boot still logs it, with the duration
  seen up to that point.
- **While unplugged** the monitor sees nothing, so that time counts as
  neither up nor down.
- **Your router must answer pings** for the ISP / WIFI distinction to work.
  Nearly all home routers do. If yours does not, every outage will be tagged
  WIFI.

## Troubleshooting

| Problem | What to try |
|---|---|
| The board does not show up as a port | Try another USB cable (many are charge-only), then install the USB driver from Step 2. |
| Upload fails at `Connecting...` | Hold the BOOT button while it says `Connecting...`. |
| My WiFi is not in the list | The ESP32 only sees 2.4 GHz networks. Tap "Scan for networks again", or type the name by hand. |
| The setup page does not open | Stay connected to UptimeMonitor-Setup and browse to `192.168.4.1`. Turn off mobile data if your phone keeps leaving the network. |
| It keeps going back to SETUP | The WiFi password was wrong, or the network is 5 GHz only. |
| Screen is blank after flashing | You may have flashed the wrong build name. Check Step 1 and flash again. |
| Picture is upside down | In `platformio.ini`, add `-DTFT_ROTATION=3` to your board's `build_flags` and flash again. |
| Colours look inverted or red and blue are swapped (CYD) | Try the other CYD build (`cyd` or `cyd2usb`). CYD clones ship with two different panels. |
| No Discord messages | Check the webhook URL starts with `https://discord.com/api/webhooks/`. Wipe settings and set up again to re-enter it. |
| Stuck showing OFFLINE with ROUTER `--` | The board has lost WiFi. Move it closer to the router. |
| I want to start over | Hold the wipe gesture for 5 seconds (see [Using it](#using-it)). |

## Serial console

Connect over USB at 115200 baud (`pio device monitor`) to watch it work. It
prints one line per round and every state change:

```
[ping] router=2ms 1.1.1.1=14ms 8.8.8.8=13ms -> OK | ONLINE
[monitor] ISP outage declared
[monitor] ISP outage ended after 14s
[log] 11/16 14:02 14s ISP -> saved (3 stored)
[alert] delivered (HTTP 204): Internet was down 14s (ISP side). Back at 14:02.
```

Type a command and press Enter:

| Command | Effect |
|---|---|
| `sim isp` | Pretend both internet targets stopped replying |
| `sim wifi` | Pretend nothing replies |
| `sim off` | Back to real results |
| `log` | Print the outage log |
| `stats` | Print the uptime figures |
| `clearlog` | Erase the outage log |

`sim isp` followed by `sim off` a few seconds later is the easy way to test
your Discord webhook without unplugging your modem.

## Adding another board

Each board is one block in `platformio.ini`; no code changes are needed.
Copy the block closest to your hardware, give it a new name, and change the
flags:

| Flag | Meaning |
|---|---|
| `TFT_*` pins and driver | Standard [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) settings for your panel |
| `SCREEN_W`, `SCREEN_H` | Panel size in landscape |
| `TFT_ROTATION` | 0 to 3; add 2 if the picture is upside down |
| `UI_SCALE_NUM`, `UI_SCALE_DEN` | UI size as a fraction of the 240x135 design: `1/1`, `5/4` or `4/3` |
| `BTN1_PIN`, `BTN2_PIN` | Button pins (active low). Leave `BTN2_PIN` out for one-button boards |
| `TOUCH_IRQ_PIN` | XPT2046 touch interrupt pin, if the board has one |
| `POWER_ON_PIN` | A pin that must be held high to power the screen, if any |
| `HEADLESS` | Build with no display at all |

The UI is drawn on a 240x135 grid and scaled, so pick the largest scale that
fits: the scaled size is 240x135, 300x169 or 320x180. It is centred on
larger panels. To add a new scale, add it to `SCALES` in
`tools/make_fonts.py` and run the script.

Pull requests for new boards are welcome. Please say whether you tested on
real hardware.

## How the code is organised

| File | Job |
|---|---|
| `src/ping.cpp` | ICMP echo on raw sockets, in its own task (no ping library) |
| `src/monitor.cpp` | Outage logic and latency history |
| `src/storage.cpp` | Outage log on LittleFS |
| `src/stats.cpp` | Uptime percentages (ISP outages only) |
| `src/timekeeper.cpp` | NTP, timezone, time formatting |
| `src/settings.cpp` | WiFi and webhook settings in NVS |
| `src/portal.cpp` | Setup access point and captive portal |
| `src/alerts.cpp` | Discord webhook over verified HTTPS |
| `src/display.cpp` | Drawing primitives, scaling and change tracking |
| `src/screens.cpp` | The four screens |
| `src/buttons.cpp` | Buttons, touch and the wipe gesture |
| `src/board.h` | Per-board options |
| `tools/` | Font and certificate generators, serial log reader |

Built with PlatformIO, the Arduino framework and
[TFT_eSPI](https://github.com/Bodmer/TFT_eSPI).

## Licence and credits

Released under the MIT licence (see `LICENSE`).

The on-screen font is [JetBrains Mono](https://www.jetbrains.com/lp/mono/),
converted to bitmaps by `tools/make_fonts.py` and used under the SIL Open
Font License 1.1 (`tools/fonts/OFL.txt`).
