/*
 * ClickGit Button Firmware v2.0.0
 *
 * ESP32-S3 USB macro button with remote LED control API.
 * Original modes (Party, YOLO, AI Commit, Custom Macro) preserved.
 * NEW: HTTP API at /led for remote LED control (for Claude Code hooks, etc.)
 * NEW: WiFi station mode to connect to home network.
 * NEW: Pin configuration via web interface.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <Adafruit_NeoPixel.h>
#include "USB.h"
#include "USBHIDKeyboard.h"

// ── Defaults ────────────────────────────────────────────────
#define FW_VERSION       "2.4.1"
#define AP_SSID          "clickgit"
#define MDNS_HOST        "clickgit"
#define NUM_LEDS         6
#define DEFAULT_LED_PIN  3
#define DEFAULT_BTN_PIN  0
#define LED_BRIGHTNESS   80
#define DEBOUNCE_MS      50
#define TAP_WINDOW       400    // Max ms between taps for multi-tap
#define TAP_SETTLE       600    // Ms after last tap before processing
#define SETUP_TIMEOUT    10000  // Focus setup timeout (10s)

// ── Globals ─────────────────────────────────────────────────
WebServer server(80);
Preferences prefs;
USBHIDKeyboard Keyboard;
Adafruit_NeoPixel* strip = nullptr;

int ledPin, btnPin;
int currentMode = 0;
String macroText = "";
String wifiSSID = "";
String wifiPass = "";
String authPassword = ""; // Empty = no auth required

bool lastBtnState = HIGH;
unsigned long lastDebounce = 0;
unsigned long ledAutoOff = 0;
bool staConnected = false;

// Animation state
enum LedEffect { EFFECT_SOLID, EFFECT_SPIN, EFFECT_PULSE, EFFECT_PARTY, EFFECT_FOCUS_START, EFFECT_FOCUS };
LedEffect currentEffect = EFFECT_SOLID;
uint8_t effectR = 0, effectG = 0, effectB = 0;
unsigned long lastEffectUpdate = 0;
int effectPos = 0;

// Focus timer state
unsigned long focusStartTime = 0;
unsigned long focusDuration = 20 * 60 * 1000UL; // Default 20 minutes

// Tap detection & UI state machine
enum UIState { UI_IDLE, UI_FOCUS_SETUP, UI_FOCUS_ACTIVE, UI_FOCUS_ALARM };
UIState uiState = UI_IDLE;
int tapCount = 0;
unsigned long lastTapTime = 0;
unsigned long focusSetupStart = 0;

// ── Color helpers ───────────────────────────────────────────
struct NamedColor { const char* name; uint8_t r, g, b; };
const NamedColor COLORS[] = {
  {"red",255,0,0}, {"green",0,255,0}, {"blue",0,0,255},
  {"yellow",255,255,0}, {"magenta",255,0,255}, {"cyan",0,255,255},
  {"white",255,255,255}, {"orange",255,165,0}, {"purple",128,0,128},
  {"emerald",16,185,129}, {"off",0,0,0},
};

void setAllLeds(uint8_t r, uint8_t g, uint8_t b) {
  if (!strip) return;
  for (int i = 0; i < NUM_LEDS; i++)
    strip->setPixelColor(i, strip->Color(r, g, b));
  strip->show();
}

bool parseColor(String s, uint8_t &r, uint8_t &g, uint8_t &b) {
  s.trim(); s.toLowerCase();
  for (auto &c : COLORS) {
    if (s == c.name) { r = c.r; g = c.g; b = c.b; return true; }
  }
  if (s.startsWith("#") && s.length() == 7) {
    long v = strtol(s.substring(1).c_str(), nullptr, 16);
    r = (v >> 16) & 0xFF; g = (v >> 8) & 0xFF; b = v & 0xFF;
    return true;
  }
  if (s.startsWith("rgb,") || s.startsWith("rgb(")) {
    int c1 = s.indexOf(',');
    int c2 = s.indexOf(',', c1 + 1);
    int c3 = s.indexOf(',', c2 + 1);
    if (c2 > 0 && c3 > 0) {
      r = s.substring(c1+1, c2).toInt();
      g = s.substring(c2+1, c3).toInt();
      b = s.substring(c3+1).toInt();
      return true;
    }
  }
  return false;
}

// ── LED effects ─────────────────────────────────────────────
void spinEffect(int ms) {
  unsigned long start = millis();
  int pos = 0;
  while (millis() - start < (unsigned long)ms) {
    for (int i = 0; i < NUM_LEDS; i++) {
      strip->setPixelColor(i, (i == pos) ? strip->Color(0,255,0) : strip->Color(0,30,0));
    }
    strip->show();
    pos = (pos + 1) % NUM_LEDS;
    delay(100);
  }
}

void pulseEffect(uint8_t r, uint8_t g, uint8_t b, int ms) {
  unsigned long start = millis();
  while (millis() - start < (unsigned long)ms) {
    float t = (millis() - start) % 1000 / 1000.0;
    float bright = (sin(t * 2 * PI) + 1.0) / 2.0;
    setAllLeds(r * bright, g * bright, b * bright);
    delay(20);
  }
}

// ── Rainbow color from wheel position (0-255) ──────────────
uint32_t colorWheel(uint8_t pos) {
  pos = 255 - pos;
  if (pos < 85)  return strip->Color(255 - pos*3, 0, pos*3);
  if (pos < 170) { pos -= 85; return strip->Color(0, pos*3, 255 - pos*3); }
  pos -= 170;
  return strip->Color(pos*3, 255 - pos*3, 0);
}

// ── Animation tick (called from loop) ───────────────────────
void tickEffect() {
  if (currentEffect == EFFECT_SOLID || !strip) return;
  unsigned long now = millis();

  // Spin: colored trail chasing around the ring
  if (currentEffect == EFFECT_SPIN && now - lastEffectUpdate > 80) {
    lastEffectUpdate = now;
    for (int i = 0; i < NUM_LEDS; i++) {
      int dist = (effectPos - i + NUM_LEDS) % NUM_LEDS;
      if (dist == 0)      strip->setPixelColor(i, strip->Color(effectR, effectG, effectB));
      else if (dist == 1) strip->setPixelColor(i, strip->Color(effectR/3, effectG/3, effectB/3));
      else if (dist == 2) strip->setPixelColor(i, strip->Color(effectR/8, effectG/8, effectB/8));
      else                strip->setPixelColor(i, 0);
    }
    strip->show();
    effectPos = (effectPos + 1) % NUM_LEDS;
  }

  // Pulse: breathing effect
  if (currentEffect == EFFECT_PULSE && now - lastEffectUpdate > 20) {
    lastEffectUpdate = now;
    float t = (now % 1200) / 1200.0;
    float bright = (sin(t * 2 * PI) + 1.0) / 2.0;
    bright = 0.15 + bright * 0.85;
    setAllLeds(effectR * bright, effectG * bright, effectB * bright);
  }

  // Party: rotating flashes with strobes and random colors
  if (currentEffect == EFFECT_PARTY && now - lastEffectUpdate > 30) {
    lastEffectUpdate = now;
    effectPos++;
    int phase = (effectPos / 25) % 4; // Switch every ~0.75s

    if (phase == 0) {
      // Fast rainbow spin
      for (int i = 0; i < NUM_LEDS; i++)
        strip->setPixelColor(i, colorWheel(((i * 256 / NUM_LEDS) + effectPos * 10) & 255));
    } else if (phase == 1) {
      // Strobe: all LEDs flash bright color then off
      if (effectPos % 4 < 2) {
        uint32_t c = colorWheel((effectPos * 37) & 255);
        for (int i = 0; i < NUM_LEDS; i++) strip->setPixelColor(i, c);
      } else {
        for (int i = 0; i < NUM_LEDS; i++) strip->setPixelColor(i, 0);
      }
    } else if (phase == 2) {
      // Each LED a different random shifting color
      for (int i = 0; i < NUM_LEDS; i++)
        strip->setPixelColor(i, colorWheel(((i * 97 + effectPos * 13) & 255)));
    } else {
      // Ping-pong bounce with trail
      int pos = effectPos % (NUM_LEDS * 2 - 2);
      if (pos >= NUM_LEDS) pos = NUM_LEDS * 2 - 2 - pos;
      for (int i = 0; i < NUM_LEDS; i++) {
        int dist = abs(i - pos);
        if (dist == 0)      strip->setPixelColor(i, colorWheel((effectPos * 8) & 255));
        else if (dist == 1) strip->setPixelColor(i, colorWheel(((effectPos * 8) + 80) & 255));
        else                strip->setPixelColor(i, 0);
      }
    }
    strip->show();
  }

  // Focus start: 5-second clockwise confirmation animation
  if (currentEffect == EFFECT_FOCUS_START) {
    unsigned long elapsed = now - focusSetupStart;
    if (elapsed >= 5000) {
      // Confirmation done — start actual focus timer
      focusStartTime = millis();
      currentEffect = EFFECT_FOCUS;
      lastEffectUpdate = 0;
    } else if (now - lastEffectUpdate > 40) {
      lastEffectUpdate = now;
      // Clockwise wipe: LEDs light up one by one over 5 seconds
      int lit = (int)((float)elapsed / 5000.0f * NUM_LEDS);
      // Spinning bright trail on top
      int trail = (effectPos++) % NUM_LEDS;
      for (int i = 0; i < NUM_LEDS; i++) {
        if (i <= lit) {
          // Already-filled LEDs: bright emerald
          strip->setPixelColor(i, strip->Color(16, 255, 160));
        } else if (i == trail) {
          // Spinning head: white flash
          strip->setPixelColor(i, strip->Color(255, 255, 255));
        } else {
          strip->setPixelColor(i, 0);
        }
      }
      strip->show();
    }
  }

  // Focus: pulsing emerald with countdown (LEDs turn off one by one)
  if (currentEffect == EFFECT_FOCUS && now - lastEffectUpdate > 30) {
    lastEffectUpdate = now;
    unsigned long elapsed = now - focusStartTime;

    if (elapsed >= focusDuration) {
      // Timer expired — switch to party alarm
      uiState = UI_FOCUS_ALARM;
      currentEffect = EFFECT_PARTY;
      effectPos = 0;
      return;
    }

    // How many LEDs should still be on (countdown)
    int ledsOn = NUM_LEDS - (int)((float)elapsed / focusDuration * NUM_LEDS);
    if (ledsOn < 1) ledsOn = 1;

    // Pulse brightness (bright so it's visible through green cover)
    float t = (now % 2000) / 2000.0;
    float bright = (sin(t * 2 * PI) + 1.0) / 2.0;
    bright = 0.3 + bright * 0.7;

    uint8_t r = 30 * bright, g = 255 * bright, b = 180 * bright; // Bright emerald
    for (int i = 0; i < NUM_LEDS; i++) {
      if (i < ledsOn) strip->setPixelColor(i, strip->Color(r, g, b));
      else            strip->setPixelColor(i, 0);
    }
    strip->show();
  }
}

// ── HID key mapping ────────────────────────────────────────
uint8_t mapSpecialKey(String key) {
  key.trim(); key.toUpperCase();
  if (key == "RETURN" || key == "ENTER") return KEY_RETURN;
  if (key == "ESCAPE" || key == "ESC")   return KEY_ESC;
  if (key == "TAB")        return KEY_TAB;
  if (key == "BACKSPACE")  return KEY_BACKSPACE;
  if (key == "DELETE")     return KEY_DELETE;
  if (key == "UP")         return KEY_UP_ARROW;
  if (key == "DOWN")       return KEY_DOWN_ARROW;
  if (key == "LEFT")       return KEY_LEFT_ARROW;
  if (key == "RIGHT")      return KEY_RIGHT_ARROW;
  if (key == "HOME")       return KEY_HOME;
  if (key == "END")        return KEY_END;
  if (key == "SPACE")      return ' ';
  if (key.startsWith("F") && key.length() <= 3) {
    int n = key.substring(1).toInt();
    if (n >= 1 && n <= 12) return KEY_F1 + n - 1;
  }
  return 0;
}

// ── Macro executor ──────────────────────────────────────────
void execLine(String line) {
  line.trim();
  if (line.length() == 0 || line.startsWith("//")) return;

  // Strip inline comments
  int commentPos = line.indexOf(" //");
  if (commentPos > 0) line = line.substring(0, commentPos);
  line.trim();

  if (line.startsWith("TYPE ")) {
    Keyboard.print(line.substring(5));
  }
  else if (line.startsWith("PRINT ")) {
    Keyboard.println(line.substring(6));
  }
  else if (line.startsWith("KEY ")) {
    uint8_t k = mapSpecialKey(line.substring(4));
    if (k) { Keyboard.press(k); delay(30); Keyboard.releaseAll(); }
  }
  else if (line.startsWith("COMBO ")) {
    String combo = line.substring(6);
    combo.trim();
    int lastPlus = combo.lastIndexOf('+');
    if (lastPlus > 0) {
      String mods = combo.substring(0, lastPlus);
      String key  = combo.substring(lastPlus + 1);
      mods.toUpperCase();
      if (mods.indexOf("CTRL") >= 0)  Keyboard.press(KEY_LEFT_CTRL);
      if (mods.indexOf("ALT") >= 0)   Keyboard.press(KEY_LEFT_ALT);
      if (mods.indexOf("SHIFT") >= 0) Keyboard.press(KEY_LEFT_SHIFT);
      if (mods.indexOf("GUI") >= 0)   Keyboard.press(KEY_LEFT_GUI);
      uint8_t sk = mapSpecialKey(key);
      if (sk) Keyboard.press(sk);
      else if (key.length() == 1) Keyboard.press(key.charAt(0));
      delay(50);
      Keyboard.releaseAll();
    }
  }
  else if (line.startsWith("LED ")) {
    String cs = line.substring(4); cs.trim();
    if (cs.startsWith("RGB,") || cs.startsWith("rgb,")) {
      int c1 = cs.indexOf(',');
      int c2 = cs.indexOf(',', c1+1);
      int c3 = cs.indexOf(',', c2+1);
      if (c2 > 0 && c3 > 0)
        setAllLeds(cs.substring(c1+1,c2).toInt(), cs.substring(c2+1,c3).toInt(), cs.substring(c3+1).toInt());
    } else {
      uint8_t r,g,b;
      if (parseColor(cs, r, g, b)) setAllLeds(r, g, b);
    }
  }
  else if (line.startsWith("DELAY ")) {
    int ms = line.substring(6).toInt();
    if (ms > 0 && ms <= 30000) delay(ms);
  }
  else if (line.startsWith("SPIN ")) {
    int ms = line.substring(5).toInt();
    if (ms > 0 && ms <= 30000) spinEffect(ms);
  }
  // Backward compat: [CTRL]+[SHIFT]+key
  else if (line.startsWith("[")) {
    // Parse old-style [MOD]+[MOD]+key
    bool ctrl = line.indexOf("[CTRL]") >= 0;
    bool shift = line.indexOf("[SHIFT]") >= 0;
    bool alt = line.indexOf("[ALT]") >= 0;
    bool gui = line.indexOf("[GUI]") >= 0 || line.indexOf("[CMD]") >= 0;
    // Get the last segment after the last +
    int lp = line.lastIndexOf('+');
    String key = (lp >= 0) ? line.substring(lp+1) : "";
    key.trim();
    // Remove brackets if present
    key.replace("[", ""); key.replace("]", "");
    if (ctrl)  Keyboard.press(KEY_LEFT_CTRL);
    if (shift) Keyboard.press(KEY_LEFT_SHIFT);
    if (alt)   Keyboard.press(KEY_LEFT_ALT);
    if (gui)   Keyboard.press(KEY_LEFT_GUI);
    if (key.length() == 1) Keyboard.press(key.charAt(0));
    delay(50);
    Keyboard.releaseAll();
  }
}

void execMacro(String macro) {
  int start = 0;
  while (start < (int)macro.length()) {
    int end = macro.indexOf('\n', start);
    if (end < 0) end = macro.length();
    execLine(macro.substring(start, end));
    start = end + 1;
  }
}

// ── Tap-based button actions ─────────────────────────────────
void handleSinglePress() {
  if (currentMode == 1) {
    // Custom macro
    execMacro(macroText);
  } else {
    // Party toggle (default for mode 0 and any unknown mode)
    if (currentEffect == EFFECT_PARTY) {
      currentEffect = EFFECT_SOLID;
      setAllLeds(0, 0, 0);
    } else {
      currentEffect = EFFECT_PARTY;
      effectPos = 0;
      lastEffectUpdate = 0;
    }
  }
}

void enterFocusSetup() {
  uiState = UI_FOCUS_SETUP;
  focusSetupStart = millis();
  // Blue pulse = "waiting for duration taps"
  currentEffect = EFFECT_PULSE;
  effectR = 0; effectG = 100; effectB = 255;
  lastEffectUpdate = 0;
}

void onFocusTapRegistered(int count) {
  // Show tap count: light up N LEDs in emerald, rest off
  if (!strip) return;
  for (int i = 0; i < NUM_LEDS; i++)
    strip->setPixelColor(i, i < count ? strip->Color(16, 185, 129) : 0);
  strip->show();
}

void startFocusTimer(int minutes) {
  if (minutes < 1) minutes = 1;
  if (minutes > 120) minutes = 120;
  focusDuration = (unsigned long)minutes * 60 * 1000UL;
  // Start 5-second confirmation animation, then timer begins
  focusSetupStart = millis();
  uiState = UI_FOCUS_ACTIVE;
  currentEffect = EFFECT_FOCUS_START;
  effectPos = 0;
  lastEffectUpdate = 0;
}

void cancelFocusTimer() {
  uiState = UI_IDLE;
  currentEffect = EFFECT_SOLID;
  setAllLeds(0, 0, 0);
}

void dismissFocusAlarm() {
  uiState = UI_IDLE;
  currentEffect = EFFECT_SOLID;
  setAllLeds(0, 0, 0);
}

// ── Preferences ─────────────────────────────────────────────
void loadPrefs() {
  prefs.begin("btn", true);
  ledPin     = prefs.getInt("ledPin", DEFAULT_LED_PIN);
  btnPin     = prefs.getInt("btnPin", DEFAULT_BTN_PIN);
  currentMode = prefs.getInt("mode", 0);
  if (currentMode == 3) currentMode = 1; // Migrate old macro mode
  if (currentMode > 1) currentMode = 0;  // Default to party
  macroText  = prefs.getString("macro", "LED GREEN\nDELAY 1000\nLED OFF");
  wifiSSID   = prefs.getString("wifiSSID", "");
  wifiPass   = prefs.getString("wifiPass", "");
  authPassword = prefs.getString("authPass", "");
  prefs.end();
}

void savePref(const char* key, int val) {
  prefs.begin("btn", false);
  prefs.putInt(key, val);
  prefs.end();
}

void savePref(const char* key, String val) {
  prefs.begin("btn", false);
  prefs.putString(key, val);
  prefs.end();
}

// ── Reinitialize LEDs with new pin ──────────────────────────
void initLeds(int pin) {
  if (strip) delete strip;
  strip = new Adafruit_NeoPixel(NUM_LEDS, pin, NEO_GRB + NEO_KHZ800);
  strip->begin();
  strip->setBrightness(LED_BRIGHTNESS);
  strip->show();
}

// ── Authentication ──────────────────────────────────────────
bool checkAuth() {
  if (authPassword.length() == 0) return true; // No password set — open access
  if (!server.authenticate("admin", authPassword.c_str())) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

// ── Web: LED API (the main new feature) ─────────────────────
void handleLedGet() {
  if (!checkAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json",
    "{\"firmware\":\"" FW_VERSION "\",\"leds\":" + String(NUM_LEDS) +
    ",\"pin\":" + String(ledPin) + "}");
}

void handleLedPost() {
  if (!checkAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String color = server.arg("color");
  String rs = server.arg("r"), gs = server.arg("g"), bs = server.arg("b");
  String effect = server.arg("effect");
  int timeout = server.arg("timeout").toInt();

  uint8_t r = 0, g = 0, b = 0;
  bool ok = false;

  if (color.length() > 0) {
    ok = parseColor(color, r, g, b);
  } else if (rs.length() > 0) {
    r = rs.toInt(); g = gs.toInt(); b = bs.toInt(); ok = true;
  }

  if (ok) {
    // During focus mode, don't override LEDs — respond OK but keep focus visuals
    if (uiState == UI_FOCUS_ACTIVE || uiState == UI_FOCUS_ALARM) {
      server.send(200, "application/json", "{\"ok\":true,\"focus\":true}");
      return;
    }

    effectR = r; effectG = g; effectB = b;
    effectPos = 0;
    lastEffectUpdate = 0;

    if (effect == "spin") {
      currentEffect = EFFECT_SPIN;
    } else if (effect == "pulse") {
      currentEffect = EFFECT_PULSE;
    } else {
      currentEffect = EFFECT_SOLID;
      setAllLeds(r, g, b);
    }

    if (timeout > 0) ledAutoOff = millis() + timeout;
    else ledAutoOff = 0;
    server.send(200, "application/json", "{\"ok\":true}");
  } else {
    server.send(400, "application/json", "{\"error\":\"bad color\"}");
  }
}

void handleLedOptions() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204);
}

// ── Web: Main page ──────────────────────────────────────────
const char PAGE_MAIN[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<style>
body{font-family:system-ui;margin:20px;text-align:center;background:#111;color:#eee}
.c{max-width:700px;margin:0 auto}
h1{color:#34d399}
select,button,textarea,input{padding:10px;margin:5px;font-size:15px;border-radius:6px;border:1px solid #333;background:#222;color:#eee}
button{background:#34d399;color:#111;cursor:pointer;border:none;font-weight:bold}
button:hover{background:#10b981}
.docs{text-align:left;background:#1a1a2e;padding:15px;border-radius:8px;margin:10px 0}
.docs code{background:#333;padding:2px 5px;border-radius:3px;font-family:monospace}
textarea{width:90%;font-family:monospace}
.status{background:#1a2e1a;padding:10px;border-radius:8px;margin:10px 0;font-size:13px}
a{color:#34d399}
.grid{display:flex;gap:10px;justify-content:center;flex-wrap:wrap;margin:10px 0}
.grid button{flex:1;min-width:80px}
.ok{color:#34d399;font-weight:bold;display:none}
</style>
<script>
function showMacro(){
  var v=document.getElementById('m').value;
  document.getElementById('mf').style.display=v=='1'?'block':'none';
}
function testLed(color){fetch('/led',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'color='+color});}
function testBtn(){
  fetch('/btn/test').then(r=>r.json()).then(d=>{
    var el=document.getElementById('btnResult');
    if(d.pressed.length>0) el.innerHTML='<span style="color:#34d399">Pressed on GPIO: '+d.pressed.join(', ')+'</span>';
    else el.innerHTML='<span style="color:#ef4444">No press detected. Try holding the button while clicking Test.</span>';
  });
}
</script>
</head><body><div class='c'>
<h1>ClickGit Button</h1>
<div class='status'>
  Firmware %FW% | LED pin: %LEDPIN% | Btn pin: %BTNPIN%<br>
  AP: clickgit (192.168.4.1) %STAINFO%<br>
  <a href='/wifi'>WiFi Settings</a> | <a href='/pins'>Pin Config</a> | <a href='/update'>Firmware Update</a>
</div>

<h3>Quick LED Test</h3>
<div class='grid'>
  <button onclick="testLed('red')">Red</button>
  <button onclick="testLed('green')">Green</button>
  <button onclick="testLed('blue')">Blue</button>
  <button onclick="testLed('purple')">Purple</button>
  <button onclick="testLed('emerald')">Emerald</button>
  <button onclick="testLed('off')">Off</button>
</div>

<h3>Single Press Action</h3>
<form action='/setmode' method='post'>
<select id='m' name='mode' onchange='showMacro()'>
  <option value='0' %S0%>Party Mode</option>
  <option value='1' %S1%>Custom Macro</option>
</select>
<button type='submit'>Save</button>

<div class='docs' style='margin-top:10px'>
<h3 style='color:#34d399;margin-top:0'>Focus Timer (double-tap anytime)</h3>
<p>Double-tap the button to enter focus mode. LEDs pulse blue while waiting. Then tap for duration:</p>
<p><strong>1 tap</strong> = 20 min &nbsp; <strong>2 taps</strong> = 40 min &nbsp; <strong>3 taps</strong> = 60 min</p>
<p>LEDs pulse emerald and count down. When time's up, rainbow party flash until you tap to dismiss. Tap once during a session to cancel.</p>
</div>

<h3>Button Pin Test</h3>
<p style='font-size:13px'>If the button doesn't respond, the GPIO pin might be wrong. Hold the button and click Test:</p>
<button type='button' onclick='testBtn()'>Test Button Pin</button>
<div id='btnResult' style='margin:8px 0;font-size:13px'></div>

<div id='mf' style='display:none'>
<h3>Macro Editor</h3>
<textarea name='macro' rows='12'>%MACRO%</textarea>
<div class='docs'>
<p>Commands: <code>TYPE text</code>, <code>PRINT text</code> (with Enter),
<code>KEY RETURN</code>, <code>COMBO GUI+SPACE</code>,
<code>LED GREEN</code>, <code>LED RGB,r,g,b</code>,
<code>DELAY ms</code>, <code>SPIN ms</code>, <code>// comment</code></p>
<p>Colors: RED GREEN BLUE YELLOW MAGENTA CYAN WHITE ORANGE PURPLE EMERALD OFF</p>
<p>Keys: UP DOWN LEFT RIGHT HOME END TAB RETURN ESC DELETE BACKSPACE SPACE F1-F12</p>
<p>Modifiers in COMBO: CTRL ALT SHIFT GUI</p>
</div>
</div>
</form>

<h3>Security</h3>
<div class='docs' style='font-size:13px'>
<p>Status: <strong style='color:%PWCOLOR%'>%PWSTATUS%</strong></p>
<form action='/password' method='post' style='margin:8px 0'>
  <input name='current' type='password' placeholder='Current password' style='width:60%'><br>
  <input name='password' type='password' placeholder='New password (blank to remove)' style='width:60%'>
  <button type='submit'>Save</button>
</form>
<p>Username is <code>admin</code>. You must enter the current password to change it. Leave new password blank to disable auth. If set, the LED API also requires auth — update your curl commands with <code>-u admin:password</code>.</p>
</div>

<h3>LED API (for Claude Code hooks)</h3>
<div class='docs' style='font-size:13px'>
<p>Control LEDs remotely via HTTP:</p>
<code>curl http://%HOST%/led -d "color=green"</code><br>
<code>curl http://%HOST%/led -d "color=blue&timeout=5000"</code><br>
<code>curl http://%HOST%/led -d "r=255&g=0&b=128"</code><br>
<code>curl http://%HOST%/led -d "color=off"</code><br>
<p style='margin-top:10px'>Colors: red green blue yellow cyan magenta purple orange emerald off, or #hex, or r/g/b params.<br>
Optional <code>timeout</code> in ms to auto-turn-off.</p>
</div>
</div>
<script>showMacro();</script>
</body></html>
)rawliteral";

void handleRoot() {
  if (!checkAuth()) return;
  String html = FPSTR(PAGE_MAIN);
  html.replace("%FW%", FW_VERSION);
  html.replace("%LEDPIN%", String(ledPin));
  html.replace("%BTNPIN%", String(btnPin));
  String staInfo = staConnected ?
    "| WiFi: " + wifiSSID + " (" + WiFi.localIP().toString() + ")" : "";
  html.replace("%STAINFO%", staInfo);
  html.replace("%S0%", currentMode==0 ? "selected" : "");
  html.replace("%S1%", currentMode==1 ? "selected" : "");
  html.replace("%MACRO%", macroText);
  html.replace("%PWSTATUS%", authPassword.length() > 0 ? "Protected" : "No password set");
  html.replace("%PWCOLOR%", authPassword.length() > 0 ? "#34d399" : "#ef4444");
  String host = staConnected ? String(MDNS_HOST) + ".local" : "192.168.4.1";
  html.replace("%HOST%", host);
  server.send(200, "text/html", html);
}

// ── Web: Save mode ──────────────────────────────────────────
void handleSetMode() {
  if (!checkAuth()) return;
  currentMode = server.arg("mode").toInt();
  if (currentMode == 1) {
    macroText = server.arg("macro");
    savePref("macro", macroText);
  }
  savePref("mode", currentMode);
  server.sendHeader("Location", "/?saved=1");
  server.send(302);
}

// ── Web: Button pin test ──────────────────────────────────────
void handleBtnTest() {
  if (!checkAuth()) return;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  // Read common GPIO pins to find which one is being pressed (LOW)
  int testPins[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 14, 21};
  int count = sizeof(testPins) / sizeof(testPins[0]);
  String pressed = "[";
  bool first = true;
  for (int i = 0; i < count; i++) {
    int p = testPins[i];
    if (p == ledPin) continue; // Skip the LED data pin
    pinMode(p, INPUT_PULLUP);
    delay(5);
    if (digitalRead(p) == LOW) {
      if (!first) pressed += ",";
      pressed += String(p);
      first = false;
    }
  }
  pressed += "]";
  // Restore button pin
  pinMode(btnPin, INPUT_PULLUP);
  server.send(200, "application/json", "{\"pressed\":" + pressed + "}");
}

// ── Web: Password ────────────────────────────────────────────
void handlePasswordPost() {
  if (!checkAuth()) return;
  String currentPass = server.arg("current");
  String newPass = server.arg("password");

  // If a password is already set, require the current password to change it
  if (authPassword.length() > 0 && currentPass != authPassword) {
    server.send(200, "text/html",
      "<html><body style='background:#111;color:#eee;text-align:center;font-family:system-ui'>"
      "<h2 style='color:#ef4444'>Current password is incorrect</h2>"
      "<a href='/' style='color:#34d399'>Back</a></body></html>");
    return;
  }

  authPassword = newPass;
  savePref("authPass", authPassword);
  server.sendHeader("Location", "/?pw=1");
  server.send(302);
}

// ── Web: WiFi config ────────────────────────────────────────
const char PAGE_WIFI[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<style>
body{font-family:system-ui;margin:20px;text-align:center;background:#111;color:#eee}
.c{max-width:500px;margin:0 auto}
h1{color:#34d399}
input,button{padding:10px;margin:5px;font-size:15px;border-radius:6px;border:1px solid #333;background:#222;color:#eee;width:80%}
button{background:#34d399;color:#111;cursor:pointer;border:none;font-weight:bold;width:auto}
.info{background:#1a2e1a;padding:10px;border-radius:8px;margin:10px 0;font-size:13px}
a{color:#34d399}
</style></head><body><div class='c'>
<h1>WiFi Settings</h1>
<div class='info'>
  Current: %STATUS%<br>
  Connect to your home WiFi so Claude Code can reach the button.
</div>
<form action='/wifi' method='post'>
  <input name='ssid' placeholder='WiFi Network Name' value='%SSID%'><br>
  <input name='pass' type='password' placeholder='Password' value='%PASS%'><br>
  <button type='submit'>Save & Connect</button>
</form>
<br><a href='/'>Back</a>
</div></body></html>
)rawliteral";

void handleWifiGet() {
  if (!checkAuth()) return;
  String html = FPSTR(PAGE_WIFI);
  String status = staConnected ?
    "Connected to " + wifiSSID + " (" + WiFi.localIP().toString() + ")" :
    (wifiSSID.length() > 0 ? "Saved but not connected: " + wifiSSID : "Not configured");
  html.replace("%STATUS%", status);
  html.replace("%SSID%", wifiSSID);
  html.replace("%PASS%", wifiPass);
  server.send(200, "text/html", html);
}

void handleWifiPost() {
  if (!checkAuth()) return;
  wifiSSID = server.arg("ssid");
  wifiPass = server.arg("pass");
  savePref("wifiSSID", wifiSSID);
  savePref("wifiPass", wifiPass);

  // Try connecting
  if (wifiSSID.length() > 0) {
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
    unsigned long start = millis();
    setAllLeds(0, 100, 255); // Blue while connecting
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(500);
    }
    if (WiFi.status() == WL_CONNECTED) {
      staConnected = true;
      setAllLeds(0, 255, 0); // Green on success
      delay(1000);
      setAllLeds(0, 0, 0);
    } else {
      setAllLeds(255, 0, 0); // Red on failure
      delay(1000);
      setAllLeds(0, 0, 0);
    }
  }
  server.sendHeader("Location", "/wifi");
  server.send(302);
}

// ── Web: Pin config ─────────────────────────────────────────
const char PAGE_PINS[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<style>
body{font-family:system-ui;margin:20px;text-align:center;background:#111;color:#eee}
.c{max-width:500px;margin:0 auto}
h1{color:#34d399}
input,button,select{padding:10px;margin:5px;font-size:15px;border-radius:6px;border:1px solid #333;background:#222;color:#eee}
button{background:#34d399;color:#111;cursor:pointer;border:none;font-weight:bold}
.info{background:#1a1a2e;padding:15px;border-radius:8px;margin:10px 0;font-size:13px;text-align:left}
a{color:#34d399}
.ok{color:#34d399;font-weight:bold}
</style></head><body><div class='c'>
<h1>Pin Configuration</h1>
<div class='info'>
  Current LED pin: <strong>%LEDPIN%</strong> | Button pin: <strong>%BTNPIN%</strong><br><br>
  If LEDs don't work, try different GPIO numbers. Common ESP32-S3 LED pins: 48, 47, 38, 35, 18, 8.<br>
  Common button pins: 0, 1, 2, 3, 4, 5.
</div>

<h3>Test LED Pin</h3>
<form action='/pins/test' method='post'>
  <input name='pin' type='number' min='0' max='48' value='%LEDPIN%' style='width:80px'>
  <button type='submit'>Flash LEDs on this pin</button>
</form>

<h3>Save Pin Config</h3>
<form action='/pins' method='post'>
  LED GPIO: <input name='ledpin' type='number' min='0' max='48' value='%LEDPIN%' style='width:80px'><br>
  Button GPIO: <input name='btnpin' type='number' min='0' max='48' value='%BTNPIN%' style='width:80px'><br>
  <button type='submit'>Save & Reboot</button>
</form>
<br><a href='/'>Back</a>
</div></body></html>
)rawliteral";

void handlePinsGet() {
  if (!checkAuth()) return;
  String html = FPSTR(PAGE_PINS);
  html.replace("%LEDPIN%", String(ledPin));
  html.replace("%BTNPIN%", String(btnPin));
  server.send(200, "text/html", html);
}

void handlePinsPost() {
  if (!checkAuth()) return;
  int newLed = server.arg("ledpin").toInt();
  int newBtn = server.arg("btnpin").toInt();
  savePref("ledPin", newLed);
  savePref("btnPin", newBtn);
  server.send(200, "text/html",
    "<html><body style='background:#111;color:#eee;text-align:center;font-family:system-ui'>"
    "<h2 style='color:#34d399'>Saved! Rebooting...</h2>"
    "<script>setTimeout(function(){window.location='/';},5000);</script></body></html>");
  delay(500);
  ESP.restart();
}

void handlePinTest() {
  if (!checkAuth()) return;
  int testPin = server.arg("pin").toInt();
  // Save pin and reboot for a clean RMT initialization
  savePref("ledPin", testPin);
  server.send(200, "application/json", "{\"ok\":true,\"pin\":" + String(testPin) + ",\"rebooting\":true}");
  delay(500);
  ESP.restart();
}

void handlePinSweep() {
  if (!checkAuth()) return;
  server.send(200, "text/plain", "Sweeping all GPIO pins... watch the LEDs. ~60 seconds.");
  // Skip GPIO 19/20 (USB), 22-25 (not on S3), 26-32 (flash/PSRAM)
  int pins[] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48};
  int count = sizeof(pins)/sizeof(pins[0]);
  for (int i = 0; i < count; i++) {
    Serial.printf("Testing pin %d (%d/%d)\n", pins[i], i+1, count);
    initLeds(pins[i]);
    setAllLeds(0, 255, 0);
    delay(1500);
    setAllLeds(0, 0, 0);
    delay(300);
  }
  // Restore saved pin
  initLeds(ledPin);
  Serial.println("Sweep done");
}

// ── Web: OTA update ─────────────────────────────────────────
const char PAGE_UPDATE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<style>
body{font-family:system-ui;margin:20px;text-align:center;background:#111;color:#eee}
.c{max-width:500px;margin:0 auto}
h1{color:#34d399}
button{background:#ef4444;color:white;border:none;padding:10px 20px;border-radius:6px;cursor:pointer;font-size:15px}
button:hover{background:#dc2626}
input[type='file']{margin:20px 0;color:#eee}
.warn{color:#ef4444;background:#2a1a1a;padding:15px;border-radius:8px;margin:20px 0}
.info{background:#1a2e1a;padding:15px;border-radius:8px;margin:20px 0;text-align:left;font-size:13px}
a{color:#34d399}
</style></head><body><div class='c'>
<h1>Firmware Update</h1>
<p>Current version: <strong>%FW%</strong></p>
<div class='warn'><strong>Warning:</strong> Only upload trusted .bin firmware files.</div>
<div class='info'>
  1. Select firmware .bin file<br>
  2. Click Update<br>
  3. Wait for reboot (~10s)<br>
  4. Reconnect to clickgit WiFi
</div>
<form method='POST' enctype='multipart/form-data'>
  <input type='file' name='update' accept='.bin' required><br><br>
  <button type='submit' onclick="return confirm('Update firmware?')">Update Firmware</button>
</form>
<br><a href='/'>Back</a>
</div></body></html>
)rawliteral";

void handleUpdateGet() {
  if (!checkAuth()) return;
  String html = FPSTR(PAGE_UPDATE);
  html.replace("%FW%", FW_VERSION);
  server.send(200, "text/html", html);
}

void handleUpdatePost() {
  if (!checkAuth()) return;
  server.sendHeader("Connection", "close");
  bool ok = !Update.hasError();
  server.send(200, "text/html",
    String("<html><body style='background:#111;color:#eee;text-align:center;font-family:system-ui'>"
    "<h2 style='color:") + (ok ? "#34d399'>Update successful!" : "#ef4444'>Update failed!") +
    "</h2><script>setTimeout(function(){window.location='/';},5000);</script></body></html>");
  delay(500);
  if (ok) ESP.restart();
}

void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    setAllLeds(128, 0, 255); // Purple = updating
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    // Green progress (estimate based on typical firmware size ~1.5MB)
    int progress = (int)((float)upload.totalSize / 1500000.0f * NUM_LEDS);
    if (progress > NUM_LEDS) progress = NUM_LEDS;
    for (int i = 0; i < NUM_LEDS; i++)
      strip->setPixelColor(i, i < progress ? strip->Color(0,255,0) : strip->Color(0,30,0));
    strip->show();
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
      Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      setAllLeds(0, 255, 0);
    } else {
      setAllLeds(255, 0, 0);
      Update.printError(Serial);
    }
  }
}

// ── Web: 404 ────────────────────────────────────────────────
void handleNotFound() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(404, "text/plain", "Not found: " + server.uri());
}

// ── Setup ───────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n\nClickGit Button v" FW_VERSION);

  // Load saved config
  loadPrefs();

  // Init LEDs
  initLeds(ledPin);
  setAllLeds(0, 100, 255); // Blue on boot

  // Init USB HID
  USB.productName("ClickGit Button");
  USB.manufacturerName("ClickGit");
  USB.begin();
  Keyboard.begin();

  // Init button
  pinMode(btnPin, INPUT_PULLUP);

  // WiFi: always start AP, optionally also connect to home WiFi
  WiFi.mode(wifiSSID.length() > 0 ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());

  if (wifiSSID.length() > 0) {
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(500);
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      staConnected = true;
      Serial.print("\nWiFi connected: "); Serial.println(WiFi.localIP());
    } else {
      Serial.println("\nWiFi connection failed, AP only");
    }
  }

  // mDNS
  if (MDNS.begin(MDNS_HOST)) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS: http://clickgit.local");
  }

  // Web server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/led", HTTP_GET, handleLedGet);
  server.on("/led", HTTP_POST, handleLedPost);
  server.on("/led", HTTP_OPTIONS, handleLedOptions);
  server.on("/setmode", HTTP_POST, handleSetMode);
  server.on("/password", HTTP_POST, handlePasswordPost);
  server.on("/wifi", HTTP_GET, handleWifiGet);
  server.on("/wifi", HTTP_POST, handleWifiPost);
  server.on("/btn/test", HTTP_GET, handleBtnTest);
  server.on("/pins", HTTP_GET, handlePinsGet);
  server.on("/pins", HTTP_POST, handlePinsPost);
  server.on("/pins/test", HTTP_POST, handlePinTest);
  server.on("/pinsweep", HTTP_GET, handlePinSweep);
  server.on("/update", HTTP_GET, handleUpdateGet);
  server.on("/update", HTTP_POST, handleUpdatePost, handleUpdateUpload);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Web server started on port 80");

  // Boot complete — hold green for 3s so correct pin is obvious
  setAllLeds(0, 255, 0);
  delay(3000);
  setAllLeds(0, 0, 0);
  Serial.println("Ready!");
}

// ── Loop ────────────────────────────────────────────────────
void loop() {
  server.handleClient();

  // Run LED animation
  tickEffect();

  // Auto-off LEDs
  if (ledAutoOff > 0 && millis() > ledAutoOff) {
    currentEffect = EFFECT_SOLID;
    setAllLeds(0, 0, 0);
    ledAutoOff = 0;
  }

  // Button press detection (debounce + multi-tap)
  bool state = digitalRead(btnPin);
  if (state != lastBtnState && millis() - lastDebounce > DEBOUNCE_MS) {
    lastDebounce = millis();
    lastBtnState = state;
    if (state == LOW) { // Pressed (active low with pullup)
      unsigned long now = millis();
      if (uiState == UI_IDLE) {
        if (tapCount > 0 && now - lastTapTime < TAP_WINDOW) {
          // Second tap within window → double-tap → focus setup
          tapCount = 0;
          lastTapTime = 0;
          enterFocusSetup();
        } else {
          tapCount = 1;
          lastTapTime = now;
        }
      } else if (uiState == UI_FOCUS_SETUP) {
        tapCount++;
        lastTapTime = now;
        onFocusTapRegistered(tapCount);
      } else if (uiState == UI_FOCUS_ACTIVE) {
        // Double-tap to cancel (ignore single taps)
        if (tapCount > 0 && now - lastTapTime < TAP_WINDOW) {
          tapCount = 0;
          lastTapTime = 0;
          cancelFocusTimer();
        } else {
          tapCount = 1;
          lastTapTime = now;
        }
      } else if (uiState == UI_FOCUS_ALARM) {
        dismissFocusAlarm();
      }
    }
  }

  // Process single tap after settle (in IDLE state)
  if (uiState == UI_IDLE && tapCount > 0 && millis() - lastTapTime > TAP_SETTLE) {
    handleSinglePress();
    tapCount = 0;
  }

  // Process duration taps after settle (in FOCUS_SETUP state)
  if (uiState == UI_FOCUS_SETUP && tapCount > 0 && millis() - lastTapTime > TAP_SETTLE) {
    startFocusTimer(tapCount * 20);
    tapCount = 0;
  }

  // Reset stale single tap during focus (so it doesn't linger)
  if (uiState == UI_FOCUS_ACTIVE && tapCount > 0 && millis() - lastTapTime > TAP_SETTLE) {
    tapCount = 0;
  }

  // Focus setup timeout (no taps within 10 seconds → exit)
  if (uiState == UI_FOCUS_SETUP && tapCount == 0 && millis() - focusSetupStart > SETUP_TIMEOUT) {
    uiState = UI_IDLE;
    currentEffect = EFFECT_SOLID;
    setAllLeds(0, 0, 0);
  }

  // Factory reset: hold button for 10 seconds
  static unsigned long holdStart = 0;
  if (digitalRead(btnPin) == LOW) {
    if (holdStart == 0) holdStart = millis();
    if (millis() - holdStart > 10000) {
      setAllLeds(255, 0, 0);
      prefs.begin("btn", false);
      prefs.clear();
      prefs.end();
      delay(1000);
      ESP.restart();
    }
  } else {
    holdStart = 0;
  }
}
