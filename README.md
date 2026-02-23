# ClickGit Button — Custom Firmware

Custom firmware for the [ClickGit Button](https://clickgit.com) that turns it into a **remote LED status indicator** and **focus timer**. Use it with Claude Code, CI/CD pipelines, or anything that can send a curl request.

<!-- Add your own photo here -->
<!-- ![ClickGit Button](photo.jpg) -->

## Features

- **HTTP LED API** — control all 6 LEDs remotely via `curl http://clickgit.local/led -d "color=green"`
- **Claude Code integration** — button spins blue while Claude works, turns green when it's done
- **Focus Timer** — double-tap to start a 20/40/60 minute deep work session with LED countdown
- **Party Mode** — strobing rainbow light show with a single press
- **Custom Macros** — program the button to type anything (HID keyboard)
- **Password protection** — HTTP Basic Auth so nobody on your network can mess with it
- **WiFi station mode** — connects to your home WiFi so you don't have to switch networks
- **mDNS** — reachable at `http://clickgit.local`
- **OTA updates** — flash new firmware over WiFi, no cables needed
- **Pin auto-detection** — built-in GPIO test to find your LED and button pins
- **Factory reset** — hold the button for 10 seconds to clear all settings

## Hardware

| Component | Details |
|---|---|
| MCU | ESP32-S3 (8MB flash, no PSRAM) |
| LEDs | 6x WS2812B addressable RGB (GRB order) |
| Switch | Cherry MX-style mechanical key switch |
| Connection | USB-C (HID keyboard + power) |
| WiFi | 2.4GHz, runs as AP + optionally connects to home network |

## Quick start

### 1. Flash the firmware

The stock ClickGit firmware already has OTA updates built in — no serial cable or PlatformIO needed.

1. Plug in the button via USB-C (it needs power)
2. On your computer, connect to the **clickgit** WiFi network (no password)
3. Open **http://192.168.4.1/update** in your browser
4. Upload `firmware.bin` from this repo
5. Wait for the reboot — LEDs go purple (uploading), then green progress bar, then all green (success)
6. The button reboots and shows green for 3 seconds

If the update page doesn't load, try **http://192.168.4.1** first to confirm you're connected to the button's WiFi.

### 2. Find the right LED pin

After flashing, the LEDs should light up green for 3 seconds on boot. If they don't, the default GPIO pin (3) doesn't match your board:

1. Reconnect to the **clickgit** WiFi network
2. Open **http://192.168.4.1/pins**
3. Use **Test LED Pin** — enter a GPIO number and click to test (the button reboots each time)
4. Try common pins: `3`, `48`, `47`, `38`, `35`, `18`, `8`
5. When you find the one that lights up green on boot, save it with **Save Pin Config**

You can also run a full sweep from the terminal (takes ~60 seconds, watch the LEDs):
```bash
curl http://192.168.4.1/pinsweep
```

### 3. Find the right button pin

If pressing the button does nothing, the button GPIO pin needs to be configured:

1. Open the main page (**http://192.168.4.1**)
2. Scroll to **Button Pin Test**
3. **Hold the physical button down** and click the Test button on the page
4. It will report which GPIO detected a press (e.g. "Pressed on GPIO: 4")
5. Go to **Pin Config** and save that number as the Button GPIO

### 4. Connect to your home WiFi

So you don't have to keep switching to the clickgit WiFi network:

1. Open **http://192.168.4.1/wifi**
2. Enter your home WiFi network name and password
3. Click **Save & Connect**
4. LEDs turn blue (connecting) → green (success) or red (failed)
5. Switch back to your normal WiFi

The button is now reachable at `http://clickgit.local` (or check your router for its IP address). The clickgit AP stays active as a fallback.

### 5. Set a password

Anyone on your network can access the button's web interface by default. Set a password:

1. Open the main page (now at `http://clickgit.local` or the button's IP)
2. Scroll to **Security**
3. Enter a password and click Save
4. Username is `admin`, works with HTTP Basic Auth

From now on, all web pages and API calls require authentication. Your browser will prompt for credentials. For curl commands, add `-u admin:YOUR_PASSWORD`.

## Button controls

All button features work via physical presses — no web UI needed.

### Single press

Depends on the selected mode (configurable in the web UI):

- **Party Mode** (default) — toggles a rainbow light show with strobes, color flashes, and a bouncing trail
- **Custom Macro** — runs your saved macro (types keys, controls LEDs)

### Double-tap → Focus Timer

Double-tap the button anytime to enter focus mode, regardless of which mode is selected:

1. **Double-tap** → LEDs pulse blue (waiting for duration)
2. **Tap for duration**: 1 tap = 20 min, 2 taps = 40 min, 3 taps = 60 min
3. LEDs show your tap count in emerald, then a 5-second clockwise fill animation confirms the start
4. **During the session** → LEDs pulse bright emerald, turning off one by one as time passes
5. **Time's up** → rainbow party flash until you tap to dismiss
6. **Double-tap during a session** to cancel (single taps are ignored to prevent accidents)

Focus mode takes priority over Claude Code hooks — the LED API won't interrupt your countdown.

If you don't tap within 10 seconds of entering setup, it exits back to idle.

### Factory reset

Hold the button down for **10 seconds**. LEDs turn red, all settings (WiFi, pins, macros, password) are cleared, and the device reboots into AP-only mode.

## LED API

Control the LEDs remotely via HTTP. All endpoints require authentication if a password is set.

### Basic usage

```bash
# Named colors
curl http://clickgit.local/led -d "color=green"
curl http://clickgit.local/led -d "color=blue"
curl http://clickgit.local/led -d "color=off"

# With authentication
curl -u admin:YOUR_PASSWORD http://clickgit.local/led -d "color=green"

# Hex color
curl http://clickgit.local/led -d "color=#10b981"

# RGB values
curl http://clickgit.local/led -d "r=255&g=0&b=128"

# Effects
curl http://clickgit.local/led -d "color=blue&effect=spin"
curl http://clickgit.local/led -d "color=emerald&effect=pulse"

# Auto-off after timeout (milliseconds)
curl http://clickgit.local/led -d "color=green&timeout=5000"
```

### Available colors

`red` `green` `blue` `yellow` `magenta` `cyan` `white` `orange` `purple` `emerald` `off`

Plus any `#RRGGBB` hex code or `r`/`g`/`b` integer params (0-255).

### Effects

| Effect | Description |
|---|---|
| (none) | Solid color |
| `spin` | Colored trail chasing around the ring |
| `pulse` | Breathing/pulsing effect |

### GET /led

Returns device info:
```json
{"firmware":"2.4.0","leds":6,"pin":3}
```

## Claude Code integration

Add these hooks to `~/.claude/settings.json` to use the button as a Claude Code status indicator:

```json
{
  "hooks": {
    "PreToolUse": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "curl -s -u admin:YOUR_PASSWORD --connect-timeout 1 --max-time 2 http://clickgit.local/led -d 'color=blue&effect=spin'",
            "timeout": 3
          }
        ]
      }
    ],
    "PostToolUse": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "curl -s -u admin:YOUR_PASSWORD --connect-timeout 1 --max-time 2 http://clickgit.local/led -d 'color=blue&effect=spin'",
            "timeout": 3
          }
        ]
      }
    ],
    "Stop": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "curl -s -u admin:YOUR_PASSWORD --connect-timeout 1 --max-time 2 http://clickgit.local/led -d 'color=green&timeout=60000'",
            "timeout": 3
          }
        ]
      }
    ],
    "Notification": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "curl -s -u admin:YOUR_PASSWORD --connect-timeout 1 --max-time 2 http://clickgit.local/led -d 'color=green&timeout=60000'",
            "timeout": 3
          }
        ]
      }
    ]
  }
}
```

Replace `YOUR_PASSWORD` with the password you set (or remove the `-u admin:YOUR_PASSWORD` part if you haven't set one). Replace `clickgit.local` with the button's IP address if mDNS doesn't resolve on your network.

| Hook | Color | Meaning |
|---|---|---|
| `PreToolUse` | Blue (spinning) | Claude is about to use a tool |
| `PostToolUse` | Blue (spinning) | Claude finished a tool, likely doing more |
| `Stop` | Green (60s) | Claude is done, waiting for your input |
| `Notification` | Green (60s) | Claude needs your attention |

Both `PreToolUse` and `PostToolUse` keep the button blue throughout chains of tool calls. The only gap is the initial thinking phase before the first tool call — there's no Claude Code hook for that yet.

If a focus timer is active, the LED API calls succeed silently but don't interrupt the focus countdown.

## Custom macros

Set the single-press mode to **Custom Macro** in the web UI, then write your macro in the editor. Each line is a command, executed sequentially when the button is pressed.

The button acts as a USB HID keyboard — macros can type text and press keys on whatever computer it's plugged into.

### Commands

| Command | Description |
|---|---|
| `// comment` | Ignored |
| `TYPE text` | Type text (no Enter) |
| `PRINT text` | Type text + press Enter |
| `KEY RETURN` | Press a single key |
| `COMBO GUI+SPACE` | Press a key combination |
| `LED GREEN` | Set all LEDs to a color |
| `LED RGB,255,0,128` | Set LEDs to custom RGB |
| `DELAY 1000` | Wait (ms) |
| `SPIN 1000` | Green loading animation (ms) |

### Available keys

`UP` `DOWN` `LEFT` `RIGHT` `HOME` `END` `TAB` `RETURN` `ESC` `DELETE` `BACKSPACE` `SPACE` `F1`-`F12`

### Combo modifiers

`CTRL` `ALT` `SHIFT` `GUI` (Cmd on Mac, Win on PC)

Use lowercase for the final key: `COMBO GUI+c` (copy) vs `COMBO GUI+C` (Shift+Cmd+C).

### Example macro

```
// Open Spotlight and launch Terminal
LED BLUE
COMBO GUI+SPACE
DELAY 400
TYPE Terminal
KEY RETURN
DELAY 1000

// Run a command
LED CYAN
PRINT echo "Hello from ClickGit!"
DELAY 500

// Done
LED GREEN
DELAY 2000
LED OFF
```

## Web endpoints

All endpoints require HTTP Basic Auth if a password is set (username: `admin`).

| Method | Path | Description |
|---|---|---|
| GET | `/` | Main dashboard |
| GET | `/led` | Device info (JSON) |
| POST | `/led` | Set LED color/effect |
| POST | `/setmode` | Save button mode and macro |
| POST | `/password` | Set or remove password |
| GET | `/wifi` | WiFi settings page |
| POST | `/wifi` | Save WiFi credentials |
| GET | `/btn/test` | Scan GPIO pins for button press |
| GET | `/pins` | Pin configuration page |
| POST | `/pins` | Save pin config (reboots) |
| POST | `/pins/test` | Test a LED pin (reboots) |
| GET | `/pinsweep` | Sweep all GPIO pins |
| GET | `/update` | OTA firmware update page |
| POST | `/update` | Upload new firmware |

## Building from source

### Prerequisites

- [PlatformIO CLI](https://platformio.org/install/cli) or PlatformIO IDE
- Python 3.x

### Build

```bash
git clone https://github.com/YOUR-USERNAME/button-project.git
cd button-project
pio run
```

The firmware binary will be at `.pio/build/esp32s3/firmware.bin`.

### Flash via OTA

No serial connection needed. With the button on your network:

1. Open `http://clickgit.local/update` (or `http://192.168.4.1/update` via the AP)
2. Upload the `.bin` file
3. Wait for reboot

### Configuration

Edit `src/main.cpp` defaults if needed:

```cpp
#define DEFAULT_LED_PIN  3        // GPIO for NeoPixel data line
#define DEFAULT_BTN_PIN  0        // GPIO for mechanical switch
#define NUM_LEDS         6        // Number of WS2812B LEDs
#define LED_BRIGHTNESS   80       // 0-255
#define AP_SSID          "clickgit"
#define MDNS_HOST        "clickgit"
```

## License

MIT
