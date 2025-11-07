/*
 *  ──────────────────────────────────────────────────────────────
 *  Project: Axiometa Binary Clock
 *  File:    Axiometa_Binary_Clock_1.5.0.ino
 *  Version: 1.5.0
 *  Authors:  Tomas Krukovski / Axiometa
 *  Authors:  Povilas Dumčius / Axiometa
 *  Date:    October 2025
 *  Board:   ESP-S3 (tested with Axiometa PIXIE M1)

 *  Description:
 *  --------------------------------------------------------------
 *  This firmware drives the Axiometa Binary Clock, a Wi-Fi-enabled
 *  NeoPixel clock that displays hours, minutes, and seconds using
 *  binary LEDs. It provides a built-in web configuration interface
 *  for Wi-Fi setup, timezone selection, color customization, and
 *  real-time or manual time display.
 *
 *  Features:
 *    - Wi-Fi setup portal (AP + captive DNS)
 *    - Real-time clock sync via NTP
 *    - Configurable LED colors and brightness
 *    - On-device web control panel
 *    - Wi-Fi status indicator LED
 *
 *  License: CERN-OHL-S-2.0
 *  --------------------------------------------------------------
 *  This source describes Open Hardware and is licensed under the
 *  CERN-OHL-S v2.0.
 *
 *  You may redistribute and modify this source and make products
 *  using it under the terms of the CERN-OHL-S v2.0 license.
 *  (https://ohwr.org/cern_ohl_s_v2.txt)
 *
 *  This documentation and associated materials must be provided
 *  under the same license when distributed. You must include a
 *  copy of the license and preserve all notices.
 *
 *  Source location: https://axiometa.io
 *  © 2025 Axiometa
 *  ──────────────────────────────────────────────────────────────
 */

#include <WiFi.h>
#include <WebServer.h>
#include <FastLED.h>
#include <time.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <ESPmDNS.h>

/* ---------- Hardware Pins ---------- */
#define FIRMWARE_VERSION "1.4.0"

/* ---------- Hardware Pins ---------- */
#define DATA_PIN 1

/* ---------- LED layout ---------- */
#define NUM_LEDS 18

/* ---------- WiFI Status LEDs ---------- */
#define WIFI_LED_PIN 21
#define NUM_WIFI_LEDS 1


CRGB wifi_led[NUM_WIFI_LEDS];
CRGB leds[NUM_LEDS];

/* lookup tables based on schematic - Row 1: modules 1-6, Row 2: modules 7-12, Row 3: modules 13-18 */
const uint8_t HOUR_LED[5] = { 5, 4, 3, 2, 1 };
const uint8_t MIN_LED[6] = { 11, 10, 9, 8, 7, 6 };
const uint8_t SEC_LED[6] = { 17, 16, 15, 14, 13, 12 };

/* ---------- Web Server & DNS ---------- */
WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

/* ---------- WiFi Configuration ---------- */
const char* apSSID = "Axiometa Binary Clock";
const char* apPassword = "";
IPAddress apIP(192, 168, 4, 1);
IPAddress netMsk(255, 255, 255, 0);

bool isConfigMode = false;
String savedSSID = "";
String savedPassword = "";
const char* ntpServer = "pool.ntp.org";
volatile bool pendingRestart = false;
unsigned long restartAt = 0;

/* ---------- Settings ---------- */
struct ClockSettings {
  String timezone = "EET-2EEST,M3.5.0/3,M10.5.0/4";
  uint8_t brightness = 50;
  CRGB hourColor = CRGB::White;
  CRGB minuteColor = CRGB::Blue;
  CRGB secondColor = CRGB::Red;
  bool use24Hour = true;
  bool manualMode = false;
  uint8_t manualHour = 12;
  uint8_t manualMinute = 0;
  uint8_t manualSecond = 0;
} settings;

time_t lastDisplayedSecond = -1;
unsigned long lastManualUpdate = 0;

/* ---------- Forward Declarations ---------- */
void spinningPerimeterEffectNonBlocking();
void greenPulseEffect();
void spinningPerimeterEffect();
void errorRedBlinkEffect();
void startConfigMode();
void startNormalMode();
void updateWiFiLED();
void setupWebServer();
void setupConfigServer();
void drawClock(uint8_t hour, uint8_t minute, uint8_t second, bool isPM);
void loadSettings();
void saveSettings();
void loadWiFiCredentials();
void saveWiFiCredentials(String ssid, String password);
CRGB hexToRGB(String hex);
String rgbToHex(CRGB color);

/* ---------- Setup ---------- */
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("Axiometa Binary Clock Firmware");
  Serial.println("Version: " FIRMWARE_VERSION);
  Serial.println("------------------------------");


  Serial.println("\n\n=== Axiometa Binary Clock ===");

  preferences.begin("clock", false);
  loadSettings();
  loadWiFiCredentials();

  FastLED.addLeds<WS2812B, WIFI_LED_PIN, GRB>(wifi_led, NUM_WIFI_LEDS);
  wifi_led[0] = CRGB::Red;  // start as red (disconnected)
  FastLED.show();

  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(settings.brightness);
  FastLED.clear();
  FastLED.show();

  if (savedSSID.length() > 0) {
    Serial.println("Attempting to connect to saved WiFi: " + savedSSID);
    spinningPerimeterEffect();
    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSSID.c_str(), savedPassword.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nConnected to WiFi!");
      updateWiFiLED();
      greenPulseEffect();
      startNormalMode();
    } else {
      Serial.println("\nFailed to connect. Starting config mode...");
      startConfigMode();
      updateWiFiLED();
    }
  } else {
    Serial.println("No saved WiFi credentials. Starting config mode...");
    startConfigMode();
    updateWiFiLED();
  }
}

/* ---------- Main Loop ---------- */
void loop() {
  if (isConfigMode) {
    dnsServer.processNextRequest();
    spinningPerimeterEffectNonBlocking();
  }


  server.handleClient();

  if (!isConfigMode) {
    if (settings.manualMode) {
      unsigned long now = millis();
      if (now - lastManualUpdate >= 1000) {
        lastManualUpdate = now;

        settings.manualSecond++;
        if (settings.manualSecond >= 60) {
          settings.manualSecond = 0;
          settings.manualMinute++;
          if (settings.manualMinute >= 60) {
            settings.manualMinute = 0;
            settings.manualHour++;
            if (settings.manualHour >= 24) {
              settings.manualHour = 0;
            }
          }
        }

        uint8_t displayHour = settings.manualHour;
        bool isPM = (settings.manualHour >= 12);

        if (!settings.use24Hour && displayHour > 12) {
          displayHour -= 12;
        } else if (!settings.use24Hour && displayHour == 0) {
          displayHour = 12;
        }

        drawClock(displayHour, settings.manualMinute, settings.manualSecond, isPM);
      }
    } else {
      time_t now = time(nullptr);
      if (now != lastDisplayedSecond) {
        lastDisplayedSecond = now;

        struct tm localTime;
        localtime_r(&now, &localTime);

        uint8_t displayHour = localTime.tm_hour;
        bool isPM = (localTime.tm_hour >= 12);

        if (!settings.use24Hour && displayHour > 12) {
          displayHour -= 12;
        } else if (!settings.use24Hour && displayHour == 0) {
          displayHour = 12;
        }

        drawClock(displayHour, localTime.tm_min, localTime.tm_sec, isPM);
      }
    }
  }


  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 2000) {
    lastWiFiCheck = millis();
    updateWiFiLED();
  }

  // Defer reboot until the success page has time to render
  if (pendingRestart && millis() >= restartAt) {
    ESP.restart();
  }
  delay(5);
}

/* ---------- WiFi Mode Functions ---------- */
void startConfigMode() {
  isConfigMode = true;

  Serial.println("Starting Access Point...");
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(apSSID, apPassword);

  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());
  Serial.println("Connect to: " + String(apSSID));

  dnsServer.start(53, "*", apIP);

  setupConfigServer();

  Serial.println("Config mode ready!");
}

void startNormalMode() {
  isConfigMode = false;

  Serial.print("Connected! IP: ");
  Serial.println(WiFi.localIP());

  if (MDNS.begin("binaryclock")) {
    Serial.println("mDNS started: http://binaryclock.local");
  }

  configTzTime(settings.timezone.c_str(), ntpServer);

  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo) && attempts < 10) {
    Serial.println("Waiting for NTP time...");
    delay(500);
    attempts++;
  }

  if (attempts < 10) {
    Serial.println("Time synchronized!");
  }

  setupWebServer();

  Serial.println("=== Clock Ready ===");
  Serial.println("Access at: http://" + WiFi.localIP().toString());
  Serial.println("Or: http://binaryclock.local");
}

/* ---------- Config Portal Server ---------- */
void setupConfigServer() {
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1", true);
    server.send(302, "text/plain", "");
  });

  server.on("/", HTTP_GET, []() {
    String html = R"rawliteral(<!DOCTYPE html>
<html>

<head>
    <title>Binary Clock Setup</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <meta charset="UTF-8">
    <link href="https://fonts.cdnfonts.com/css/pirulen" rel="stylesheet">
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }

        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: #000;
            min-height: 100vh;
            padding: 20px;
            color: #fff;
        }

        .header {
            text-align: center;
            margin-bottom: 0;
        }

        .text-logo {
            font-family: 'Pirulen', sans-serif;
            font-size: 2.6em;
            letter-spacing: 3px;
            color: #fff;
            margin-top: 16px;
        }

        .tagline {
            font-family: 'Pirulen', sans-serif;
            color: #999;
            font-size: 14px;
            letter-spacing: 2px;
        }

        .container {
            max-width: 1000px;
            margin: 0 auto;
            background: #000;
            padding: 30px;
            border-radius: 20px;
        }

        h2 {
            color: #fff;
            border-bottom: 3px solid #E2F14F;
            padding-bottom: 10px;
            margin: 10px 0 20px 0;
        }

        .section {
            margin: 30px 0;
            padding: 25px;
            background: #131313;
            border-radius: 12px;
            border: 2px solid #000;
        }

        .setting {
            margin: 20px 0;
        }

        label {
            display: block;
            margin-bottom: 8px;
            font-weight: 600;
            color: #fff;
        }

        input {
            width: 100%;
            padding: 12px;
            border: 2px solid #333;
            border-radius: 8px;
            font-size: 16px;
            background: #0a0a0a;
            color: #fff;
        }

        input:focus {
            outline: none;
            border-color: #131313;
            background: #000;
        }

        button {
            width: 100%;
            padding: 15px;
            background: #E2F14F;
            color: #000;
            border: none;
            border-radius: 8px;
            font-size: 16px;
            font-weight: 600;
            cursor: pointer;
            margin-top: 20px;
        }

        button:hover {
            transform: translateY(-2px);
            opacity: 0.9;
        }

        .info {
            background: #000;
            padding: 15px;
            border-radius: 8px;
            margin-bottom: 20px;
            border-left: 4px solid #E2F14F;
            font-size: 14px;
        }

        .networks {
            max-height: 200px;
            overflow-y: auto;
            border: 2px solid #333;
            border-radius: 8px;
            padding: 10px;
            margin-bottom: 20px;
            background: #0a0a0a;
        }

        .network-item {
            padding: 10px;
            cursor: pointer;
            border-radius: 5px;
            margin: 5px 0;
            transition: background 0.2s;
            color: #fff;
        }

        .network-item:hover {
            background: #2a2a2a;
        }

        .network-item.selected {
            background: #E2F14F;
            color: #000;
            font-weight: 600;
        }

        .signal {
            float: right;
            font-size: 12px;
            color: #999;
        }

        .loading {
            text-align: center;
            padding: 20px;
            color: #666;
        }

        .password-section {
            display: none;
            margin-top: 20px;
        }

        .error {
            color: #ff3b3b;
            font-size: 14px;
            margin-top: 10px;
            display: none;
        }

        @media (max-width: 768px) {
            body {
                padding: 10px;
            }

            .container {
                padding: 15px;
            }

            .section {
                padding: 15px;
                margin: 15px 0;
            }

            h2 {
                font-size: 1.2em;
            }

            input,
            button {
                font-size: 15px;
                padding: 12px;
            }
        }
    </style>
</head>

<body>
    <div class="header">
        <div class="text-logo">AXIOMETA</div>
        <div class="tagline">BINARY CLOCK - SETUP</div>
    </div>
    <div class="container">
        <div class="section">
            <h2>Step 1: Choose a WiFi Network</h2>
            <div class="info">Select your WiFi network below.</div>
            <div class="setting">
                <label>Available Networks:</label>
                <div class="networks" id="networks">
                    <div class="loading">Scanning for networks...</div>
                </div>
            </div>
            <div class="password-section" id="passwordSection">
<div class="setting" id="pwBlock">
  <label for="password">Wi-Fi Password:</label>
  <input 
      type="text" 
      id="password" 
      placeholder="Enter Wi-Fi password"
      inputmode="text"
      autocomplete="new-password"
      autocorrect="off"
      autocapitalize="none"
      spellcheck="false"
      data-form-type="none"
      data-lpignore="true"
      form="none"
      name="wifi_pass_field"
      style="-webkit-text-security: disc;"
  >
</div>

    <div class="error" id="error"></div>
    <button onclick="saveWiFi()">Connect to WiFi</button>
</div>

        </div>
    </div>
    <script>
        var selectedSSID = "";
        var needsPassword = false;

        function showError(message) {
            var errorDiv = document.getElementById('error');
            errorDiv.textContent = message;
            errorDiv.style.display = 'block';
        }

        function scanNetworks() {
            fetch('/scan').then(function(r) {
                return r.json();
            }).then(function(data) {
                var container = document.getElementById('networks');
                container.innerHTML = '';
                data.networks.forEach(function(network) {
                    var div = document.createElement('div');
                    div.className = 'network-item';
                    div.innerHTML = network.ssid + '<span class="signal">' + network.rssi + ' dBm</span>';
                    div.onclick = function() {
                        document.querySelectorAll('.network-item').forEach(function(i) {
                            i.classList.remove('selected');
                        });
                        div.classList.add('selected');
                        selectedSSID = network.ssid;
                        needsPassword = network.password === 1;
                        document.getElementById('error').style.display = 'none';
                        document.getElementById('passwordSection').style.display = 'block';
                        if (needsPassword) {
                            //document.getElementById('password').focus();
                        }
                    };
                    container.appendChild(div);
                });
                if (data.networks.length === 0) container.innerHTML = '<div class="loading">No networks found</div>';
            }).catch(function() {
                document.getElementById('networks').innerHTML = '<div class="loading">Scan failed</div>';
            });
        }

        function saveWiFi() {
            if (!selectedSSID) {
                showError('Please select a network first.');
                return;
            }
            var password = document.getElementById('password').value;
            if (needsPassword && !password) {
                showError('Password is required for this network.');
                return;
            }

            fetch('/save', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify({
                    ssid: selectedSSID,
                    password: needsPassword ? password : ""
                })
            }).then(function(res) {
                if (res.ok) return res.text();
                throw new Error('Network response was not ok');
            }).then(function(html) {
                document.open();
                document.write(html);
                document.close();
            }).catch(function(err) {
                console.error('Error:', err);
                showError('Error saving WiFi. Please try again.');
            });
        }

        scanNetworks();
    </script>
</body>

</html>
)rawliteral";
    server.send(200, "text/html", html);
  });

  server.on("/scan", HTTP_GET, []() {
    int n = WiFi.scanNetworks();
    String json = "{\"networks\":[";
    String listed = "";
    bool first = true;

    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;
      if (listed.indexOf("|" + ssid + "|") != -1) continue;
      listed += "|" + ssid + "|";

      if (!first) json += ",";
      first = false;

      bool secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"password\":" + String(secure ? 1 : 0) + "}";
    }

    json += "]}";
    server.send(200, "application/json", json);
  });

  server.on("/save", HTTP_POST, []() {
    if (!server.hasArg("plain")) {
      server.send(400, "text/plain", "No data");
      return;
    }

    String body = server.arg("plain");

    int ssidStart = body.indexOf("ssid\":\"") + 7;
    int ssidEnd = body.indexOf("\"", ssidStart);
    String ssid = body.substring(ssidStart, ssidEnd);

    int passStart = body.indexOf("password\":\"") + 11;
    int passEnd = body.indexOf("\"", passStart);
    String password = body.substring(passStart, passEnd);

    saveWiFiCredentials(ssid, password);

    server.sendHeader("Connection", "close");
    server.sendHeader("Cache-Control", "no-cache");
    server.send(200, "text/html", R"rawliteral(<!DOCTYPE html>
<html>

<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <title>Wi-Fi Connected</title>
    <link href="https://fonts.cdnfonts.com/css/pirulen" rel="stylesheet">
    <style>
        body {
            background: #000;
            color: #fff;
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            margin: 0;
            padding: 24px;
            text-align: center;
        }

        .wrap {
            max-width: 720px;
            margin: 0 auto;
        }

        .logo {
            font-family: 'Pirulen', sans-serif;
            letter-spacing: 3px;
            margin: 16px 0 8px 0;
            font-size: 2em;
        }

        .tagline {
            font-family: 'Pirulen', sans-serif;
            color: #999;
            font-size: 14px;
            letter-spacing: 2px;
        }

        .card {
            background: #131313;
            border: 2px solid #000;
            border-radius: 16px;
            padding: 24px;
            margin-top: 16px;
        }

        .spinner {
            border: 4px solid #333;
            border-top: 4px solid #E2F14F;
            border-radius: 50%;
            width: 50px;
            height: 50px;
            animation: spin 1s linear infinite;
            margin: 20px auto;
        }

        @keyframes spin {
            0% {
                transform: rotate(0deg);
            }

            100% {
                transform: rotate(360deg);
            }
        }

        .checkmark {
            display: none;
            width: 50px;
            height: 50px;
            border-radius: 50%;
            border: 4px solid #E2F14F;
            margin: 20px auto;
            position: relative;
        }

        .checkmark:after {
            content: '';
            position: absolute;
            left: 14px;
            top: 6px;
            width: 12px;
            height: 22px;
            border: solid #E2F14F;
            border-width: 0 4px 4px 0;
            transform: rotate(45deg);
        }

        .status {
            font-size: 18px;
            margin: 16px 0;
            color: #E2F14F;
        }

        .link-box {
            margin: 20px 0;
            padding: 15px;
            background: #0a0a0a;
            border: 2px solid #e2f14f;
            border-radius: 8px;
            font-family: monospace;
            font-size: 24px;
            color: #E2F14F;
            cursor: pointer;
            transition: background 0.2s;
        }

        .link-box:hover {
            background: #1a1a1a;
        }

        .instruction {
            font-size: 14px;
            color: #fff;
            margin-top: 15px;
            line-height: 1.6;
            text-align: left;

        }

        .instruction strong {
            color: #E2F14F;
        }

    </style>
</head>

<body>
    <div class="wrap">
        <div class="logo">AXIOMETA</div>
         <div class="tagline">BINARY CLOCK - SETUP</div>
        <div class="card">
            <div class="spinner" id="spinner"></div>
            <div class="checkmark" id="checkmark"></div>
            <div class="status" id="status">Connecting to WiFi...</div>
            <div id="content" style="display:none;">
                <div class="link-box" id="linkBox" onclick="copyLink()">binaryclock.local</div>
                <div class="instruction">
                    <strong>What's next ?</strong><br>
                    1. Copy the above link by clicking it.<br>
                    2. Close this window and open a new browser tab.<br>
                    3. Enter the link to open the control panel.<br><br>

                    Please wait 10 seconds for the device to reboot.
                </div>
            </div>
        </div>
    </div>
    <script>
        function copyLink() {
            var link = 'binaryclock.local';
            if (navigator.clipboard) {
                navigator.clipboard.writeText(link).then(function() {
                    var linkBox = document.getElementById('linkBox');
                    linkBox.textContent = 'Copied!';
                    setTimeout(function() {
                        linkBox.textContent = link;
                    }, 300);
                });
            } else {
                var input = document.createElement('input');
                input.value = link;
                document.body.appendChild(input);
                input.select();
                document.execCommand('copy');
                document.body.removeChild(input);
                var linkBox = document.getElementById('linkBox');
                linkBox.textContent = 'Copied!';
                setTimeout(function() {
                    linkBox.textContent = link;
                }, 1500);
            }
        }

        setTimeout(function() {
            document.getElementById('spinner').style.display = 'none';
            document.getElementById('checkmark').style.display = 'block';
            document.getElementById('status').textContent = 'WiFi Connected Successfully!';
            document.getElementById('content').style.display = 'block';
        }, 3000);
    </script>
</body>

</html>
)rawliteral");

    server.client().flush();
    delay(100);

    pendingRestart = true;
    restartAt = millis() + 8000;

    Serial.println("WiFi credentials saved. Rebooting in 8 seconds...");
  });

  server.begin();
}

/* ---------- Normal Operation Server ---------- */
void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html>

<head>
  <title>Axiometa Binary Clock</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta charset="UTF-8">
  <link href="https://fonts.cdnfonts.com/css/pirulen" rel="stylesheet">
  <style>
    * {
      margin: 0;
      padding: 0;
      box-sizing: border-box;
    }

    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, sans-serif;
      background: #000000;
      min-height: 100vh;
      padding: 20px;
      color: #ffffff;
    }

    .header {
      text-align: center;
      margin-bottom: 0px;
    }
    
    .logo {
      text-align: center;
      margin-bottom: 10px;
      padding-top: 16px;
    }
      
    .logo-img {
      width: 300px;
      max-width: 100%;
      height: auto;
      filter: brightness(1.1) contrast(1.05);
    }

    .tagline {
      font-family: 'Pirulen', sans-serif;
      color: #999999;
      font-size: 14px;
      letter-spacing: 2px;
    }

    .container {
      max-width: 1000px;
      margin: 0 auto;
      background: #000000;
      padding: 30px;
      border-radius: 20px;
      border: 3px solid #000000;
    }

    h1 {
      color: #ffffff;
      text-align: center;
      margin-bottom: 30px;
      font-size: 2em;
    }

    h2 {
      color: #ffffff;
      border-bottom: 3px solid #E2F14F;
      padding-bottom: 10px;
      margin: 10px 0 20px 0;
    }

    .current-time {
      text-align: center;
      font-size: 32px;
      margin: 20px 0;
      padding: 25px;
      background: #000000;
      border-radius: 10px;
      font-weight: 100;
      color: #E2F14F;
      border: 3px solid #131313;
    }

    .setting {
      margin: 20px 0;
    }

    label {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 8px;
      font-weight: 600;
      color: #ffffff;
    }

    label.checkbox-label {
      display: block;
    }

    input,
    select {
      width: 100%;
      padding: 12px;
      border: 2px solid #333333;
      border-radius: 8px;
      box-sizing: border-box;
      font-size: 16px;
      transition: border-color 0.3s;
      background: #0a0a0a;
      color: #ffffff;
    }

    input:focus,
    select:focus {
      outline: none;
      border-color: #131313;
      background: #000000;
    }

    input[type="color"] {
      height: 50px;
      cursor: pointer;
      padding: 4px;
    }

    input[type="range"] {
      margin: 10px 0;
      cursor: pointer;
      -webkit-appearance: none;
      appearance: none;
      height: 8px;
      background: #333333;
      border-radius: 5px;
      outline: none;
    }

    input[type="range"]::-webkit-slider-thumb {
      -webkit-appearance: none;
      appearance: none;
      width: 20px;
      height: 20px;
      background: #E2F14F;
      cursor: pointer;
      border-radius: 50%;
    }

    input[type="range"]::-moz-range-thumb {
      width: 20px;
      height: 20px;
      background: #E2F14F;
      cursor: pointer;
      border-radius: 50%;
      border: none;
    }

    input[type="checkbox"] {
      width: auto;
      margin-right: 10px;
      accent-color: #E2F14F;
    }

    .brightness-value {
      font-weight: bold;
      color: #E2F14F;
    }

    button {
      background: #E2F14F;
      color: #000000;
      padding: 15px 30px;
      border: none;
      border-radius: 8px;
      cursor: pointer;
      width: 100%;
      font-size: 16px;
      font-weight: 600;
      margin-top: 20px;
      transition: transform 0.2s, opacity 0.3s;
    }

    button:hover {
      transform: translateY(-2px);
      opacity: 0.9;
    }

    button:active {
      transform: scale(0.98);
    }

    button.danger {
      background: #9e111c;
      color: #ffffff;
    }

    .color-preview {
      width: 40px;
      height: 40px;
      border: 2px solid #333333;
      border-radius: 5px;
      flex-shrink: 0;
    }

    .section {
      margin: 30px 0;
      padding: 25px;
      background: #131313;
      border-radius: 12px;
      border: 2px solid #000000;
    }

    .info-box {
      background: #131313;
      padding: 15px;
      border-radius: 8px;
      margin-bottom: 20px;
      color: #ffffff;
      font-weight: 300;
    }

    .note {
      background: #000000;
      padding: 12px;
      border-radius: 6px;
      margin-top: 10px;
      font-size: 14px;
      border-left: 4px solid #E2F14F;
      color: #ffffff;
    }

    .warning {
      background: #2a0a0a;
      padding: 12px;
      border-radius: 6px;
      margin-top: 10px;
      font-size: 14px;
      border-left: 4px solid #ff081d;
      color: #ffffff;
    }

    .manual-time-inputs {
      display: flex;
      gap: 10px;
      margin-top: 10px;
    }

    .manual-time-inputs input {
      flex: 1;
    }

    :root {
      --cell: clamp(26px, 5.2vw, 40px);
      --gap: clamp(6px, 1.2vw, 12px);
    }

    .binary-table {
      display: grid;
      grid-template-columns: repeat(6, var(--cell)) 60px;
      grid-auto-rows: var(--cell);
      gap: var(--gap);
      justify-content: center;
      align-items: center;
      margin: 20px auto;
      max-width: fit-content;
    }

    .legend-cell {
      width: var(--cell);
      height: var(--cell);
      display: flex;
      align-items: center;
      justify-content: center;
      font-size: clamp(12px, 2.6vw, 16px);
      color: #E2F14F;
      font-weight: 700;
    }

    .binary-cell {
      width: var(--cell);
      height: var(--cell);
      border-radius: 6px;
      background: #0a0a0a;
      border: 2px solid #2d2d2d;
      display: flex;
      align-items: center;
      justify-content: center;
      font-size: clamp(12px, 2.6vw, 16px);
      font-weight: 700;
      color: #000;
      transition: background 0.25s, border-color 0.25s, transform 0.15s;
    }

    .binary-cell.on {
      background: #E2F14F;
      border-color: #E2F14F;
      transform: scale(1.04);
    }

    .binary-cell.off {
      background: #000;
      border-color: #333;
      color: #222;
    }

    .binary-sum {
      width: 60px;
      text-align: left;
      font-size: clamp(12px, 2.6vw, 16px);
      color: #E2F14F;
      font-weight: 700;
    }

    .binary-table .legend-sum-spacer {
      width: 60px;
    }

    #binaryExplanation {
      margin-top: 20px;
      text-align: center;
      color: #E2F14F;
      font-weight: bold;
    }
    :root {
      --cell: clamp(26px, 5.2vw, 40px);
      --gap: clamp(6px, 1.2vw, 12px);
    }

    .binary-table {
      display: grid;
      grid-template-columns: repeat(6, var(--cell)) 60px;
      grid-auto-rows: var(--cell);
      gap: var(--gap);
      justify-content: center;
      align-items: center;
      margin-top: 20px;
    }

    .legend-cell {
      width: var(--cell);
      height: var(--cell);
      display: flex;
      align-items: center;
      justify-content: center;
      font-size: clamp(12px, 2.6vw, 16px);
      color: #E2F14F;
      font-weight: 700;
    }

    .binary-cell {
      width: var(--cell);
      height: var(--cell);
      border-radius: 6px;
      background: #0a0a0a;
      border: 2px solid #2d2d2d;
      display: flex;
      align-items: center;
      justify-content: center;
      font-size: clamp(12px, 2.6vw, 16px);
      font-weight: 700;
      color: #000;
      transition: background 0.25s, border-color 0.25s, transform 0.15s;
    }

    .binary-cell.on {
      background: #E2F14F;
      border-color: #E2F14F;
      transform: scale(1.04);
    }

    .binary-cell.off {
      background: #000;
      border-color: #333;
      color: #222;
    }

    .binary-sum {
      width: 60px;
      text-align: left;
      font-size: clamp(12px, 2.6vw, 16px);
      color: #E2F14F;
      font-weight: 700;
    }

    .binary-table .legend-sum-spacer {
      width: 60px;
    }

    .manual-time-inputs select#manualAmPm {
  flex: 1;
  padding: 12px;
  border: 2px solid #333333;
  border-radius: 8px;
  background: #0a0a0a;
  color: #ffffff;
  font-size: 16px;
  transition: border-color 0.3s;
}

.manual-time-inputs select#manualAmPm:focus {
  outline: none;
  border-color: #131313;
  background: #000000;
}


    #binaryExplanation {
      margin-top: 20px;
      text-align: center;
      color: #E2F14F;
      font-weight: bold;
    }

    ::-webkit-scrollbar {
      width: 10px;
    }

    ::-webkit-scrollbar-track {
      background: #0a0a0a;
    }

    ::-webkit-scrollbar-thumb {
      background: #333333;
      border-radius: 5px;
    }

    ::-webkit-scrollbar-thumb:hover {
      background: #E2F14F;
    }

    @media (max-width: 768px) {
      body {
        padding: 10px;
      }

      .container {
        padding: 15px;
      }

      .section {
        padding: 15px;
        margin: 15px 0;
      }

      h1 {
        font-size: 1.6em;
      }

      h2 {
        font-size: 1.2em;
      }

      input,
      select,
      button {
        font-size: 15px;
        padding: 12px;
      }
    }

  #toast {
  visibility: hidden;
  min-width: 200px;
  background-color: #E2F14F;
  color: #000;
  text-align: center;
  border-radius: 8px;
  padding: 14px;
  position: fixed;
  left: 50%;
  bottom: 30px;
  transform: translateX(-50%);
  font-weight: 600;
  font-family: 'DM Sans', sans-serif;
  z-index: 9999;
  opacity: 0;
  transition: opacity 0.4s, bottom 0.4s;
}

#toast.show {
  visibility: visible;
  opacity: 1;
  bottom: 50px;
}

  </style>
</head>

<body>
  <div class="header">

  <div class="logo">
  <img src="https://cdn.shopify.com/s/files/1/0966/7756/0659/files/white_logo.png" 
       alt="Axiometa Logo" class="logo-img">
       </div>
    <div class="tagline">BINARY CLOCK - Control Panel</div>
  </div>

  <div class="container">


<div class="section">
      <h2>How to Read the Binary Clock</h2>
      <div class="info-box">
        <p>The clock uses <strong>three rows of LEDs</strong> to display the current time in <strong>binary</strong> (base-2) form.</p>
        <ul style="margin-top:10px; line-height:1.6; padding-left:20px; list-style-position:inside;">
          <li><strong>Top Row:</strong> Hours (0–23)</li>
          <li><strong>Middle Row:</strong> Minutes (0–59)</li>
          <li><strong>Bottom Row:</strong> Seconds (0–59)</li>
        </ul>
        <p style="margin-top:12px;">Each LED represents a binary digit:</p>
        <div class="note" style="margin-top:6px;">
          <strong>LED ON = 1</strong> (add this value) • <strong>LED OFF = 0</strong> (ignore this value)
        </div>
        <p style="margin-top:12px;">To read the time, add the values of all lit LEDs in each row.</p>
      </div>
      <div id="binaryTable" class="binary-table"></div>
      <div id="binaryExplanation">Loading binary visualization...</div>
    </div>

<div class="section">
  <h2>Time Settings</h2>

  <!-- Current Time + Manual Mode -->
  <div class="setting">
    <div class="current-time" id="currentTime">Loading...</div>
    <label class="checkbox-label">
      <input type="checkbox" id="manualMode"> ENABLE MANUAL TIME SETTINGS
    </label>

  </div>

  <!-- Manual Time Input Section -->
  <div class="setting" id="manualTimeSection" style="display:none;">
    <label class="checkbox-label">Set Custom Time (HH:MM:SS):</label>
    <div class="manual-time-inputs">
      <input type="number" id="manualHour" min="0" max="23" value="12">
      <input type="number" id="manualMinute" min="0" max="59" value="0">
      <input type="number" id="manualSecond" min="0" max="59" value="0">
    </div>
  </div>

  <!-- Time Zone Selector -->
  <div class="setting">
    <label class="checkbox-label">Select Your City/Time Zone:</label>
    <select id="timezone" name="timezone">
<!-- 🌍 EUROPE -->
<optgroup label="Europe">
  <option value="GMT0BST,M3.5.0/1,M10.5.0">London, UK</option>
  <option value="WET0WEST,M3.5.0/1,M10.5.0">Lisbon, Portugal</option>
  <option value="GMT0">Reykjavik, Iceland</option>
  <option value="GMT0BST,M3.5.0/1,M10.5.0">Dublin, Ireland</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Amsterdam, Netherlands</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Berlin, Germany</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Brussels, Belgium</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Paris, France</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Monaco</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Zurich, Switzerland</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Vienna, Austria</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Madrid, Spain</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Andorra la Vella, Andorra</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Rome, Italy</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">San Marino</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Vatican City</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Malta</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Prague, Czech Republic</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Warsaw, Poland</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Budapest, Hungary</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Copenhagen, Denmark</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Stockholm, Sweden</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Oslo, Norway</option>
  <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Helsinki, Finland</option>
  <option value="EET-2EEST,M3.5.0/3,M10.5.0/4" selected>Vilnius, Lithuania</option>
  <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Riga, Latvia</option>
  <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Tallinn, Estonia</option>
  <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Kyiv, Ukraine</option>
  <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Bucharest, Romania</option>
  <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Sofia, Bulgaria</option>
  <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Athens, Greece</option>
  <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Nicosia, Cyprus</option>
  <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Chisinau, Moldova</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Zagreb, Croatia</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Ljubljana, Slovenia</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Belgrade, Serbia</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Sarajevo, Bosnia & Herzegovina</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Podgorica, Montenegro</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Tirana, Albania</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Skopje, North Macedonia</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Pristina, Kosovo</option>
  <option value="TRT-3">Istanbul, Türkiye</option>
  <option value="GMT0WEST,M3.5.0/1,M10.5.0">Faroe Islands</option>
  <option value="GMT0BST,M3.5.0/1,M10.5.0">Isle of Man</option>
  <option value="GMT0BST,M3.5.0/1,M10.5.0">Guernsey</option>
  <option value="GMT0BST,M3.5.0/1,M10.5.0">Jersey</option>
  <option value="CET-1CEST,M3.5.0,M10.5.0/3">Gibraltar</option>
  <option value="AZT-4">Baku, Azerbaijan</option>
  <option value="GET-4">Tbilisi, Georgia</option>
  <option value="AMT-4">Yerevan, Armenia</option>
</optgroup>

<!-- 🇺🇸 UNITED STATES -->
<optgroup label="United States">
  <option value="EST5EDT,M3.2.0,M11.1.0">Eastern Time (New York, Washington D.C., Florida, Georgia, Ohio, Virginia)</option>
  <option value="CST6CDT,M3.2.0,M11.1.0">Central Time (Chicago, Dallas, Houston, Minneapolis, St. Louis, Kansas City)</option>
  <option value="MST7MDT,M3.2.0,M11.1.0">Mountain Time (Denver, Salt Lake City, Albuquerque, Boise)</option>
  <option value="MST7">Arizona (No DST)</option>
  <option value="PST8PDT,M3.2.0,M11.1.0">Pacific Time (Los Angeles, San Francisco, Seattle, Portland)</option>
  <option value="AKST9AKDT,M3.2.0,M11.1.0">Alaska Time (Anchorage, Fairbanks, Juneau)</option>
  <option value="HST10">Hawaii (No DST)</option>
  <option value="CHST-10">Guam</option>
  <option value="SST11">American Samoa</option>
  <option value="AST4">Puerto Rico / U.S. Virgin Islands</option>
</optgroup>

<!-- 🇨🇦 CANADA -->
<optgroup label="Canada">
  <option value="NST3:30NDT,M3.2.0,M11.1.0">Newfoundland (St. John’s)</option>
  <option value="AST4ADT,M3.2.0,M11.1.0">Atlantic Time (Halifax, Moncton)</option>
  <option value="EST5EDT,M3.2.0,M11.1.0">Eastern Time (Toronto, Ottawa, Montreal)</option>
  <option value="CST6CDT,M3.2.0,M11.1.0">Central Time (Winnipeg, Regina)</option>
  <option value="MST7MDT,M3.2.0,M11.1.0">Mountain Time (Calgary, Edmonton)</option>
  <option value="PST8PDT,M3.2.0,M11.1.0">Pacific Time (Vancouver, Victoria)</option>
</optgroup>

<!-- 🌎 SOUTH AMERICA -->
<optgroup label="South America">
  <option value="<-03>3">São Paulo, Brazil</option>
  <option value="<-03>3">Buenos Aires, Argentina</option>
  <option value="<-04>4">Santiago, Chile</option>
  <option value="<-05>5">Lima, Peru</option>
  <option value="<-05>5">Bogotá, Colombia</option>
  <option value="<-04>4">Caracas, Venezuela</option>
  <option value="<-03>3">Montevideo, Uruguay</option>
  <option value="<-03>3">Asunción, Paraguay</option>
  <option value="<-05>5">Quito, Ecuador</option>
</optgroup>

<!-- 🌏 ASIA & OCEANIA -->
<optgroup label="Asia & Oceania">
  <option value="GST-4">Dubai, UAE</option>
  <option value="AST-3">Riyadh, Saudi Arabia</option>
  <option value="PKT-5">Karachi, Pakistan</option>
  <option value="IST-5:30">New Delhi, India</option>
  <option value="BST-6">Dhaka, Bangladesh</option>
  <option value="MMT-6:30">Yangon, Myanmar</option>
  <option value="ICT-7">Bangkok, Thailand</option>
  <option value="WIB-7">Jakarta, Indonesia</option>
  <option value="CST-8">Beijing, China</option>
  <option value="SGT-8">Singapore</option>
  <option value="HKT-8">Hong Kong</option>
  <option value="PST-8">Manila, Philippines</option>
  <option value="KST-9">Seoul, South Korea</option>
  <option value="JST-9">Tokyo, Japan</option>
  <option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Sydney, Australia</option>
  <option value="ACST-9:30ACDT,M10.1.0,M4.1.0/3">Adelaide, Australia</option>
  <option value="AWST-8">Perth, Australia</option>
  <option value="NZST-12NZDT,M9.5.0,M4.1.0/3">Auckland, New Zealand</option>
  <option value="CHAST-12:45CHADT,M9.5.0,M4.1.0/3">Chatham Islands</option>
</optgroup>

<!-- 🌍 AFRICA -->
<optgroup label="Africa">
  <option value="WAT-1">Lagos, Nigeria</option>
  <option value="WAT-1">Accra, Ghana</option>
  <option value="CAT-2">Cairo, Egypt</option>
  <option value="CAT-2">Khartoum, Sudan</option>
  <option value="CAT-2">Harare, Zimbabwe</option>
  <option value="SAST-2">Cape Town, South Africa</option>
  <option value="EAT-3">Nairobi, Kenya</option>
  <option value="EAT-3">Addis Ababa, Ethiopia</option>
  <option value="EAT-3">Dar es Salaam, Tanzania</option>
  <option value="EAT-3">Kampala, Uganda</option>
</optgroup>

<!-- 🌴 PACIFIC -->
<optgroup label="Pacific Islands">
  <option value="SST11">Pago Pago, American Samoa</option>
  <option value="HST10">Honolulu, Hawaii (USA)</option>
  <option value="FJT-12">Suva, Fiji</option>
  <option value="TOT-13">Nuku’alofa, Tonga</option>
  <option value="WST-13">Apia, Samoa</option>
  <option value="LINT-14">Kiritimati, Kiribati</option>
</optgroup>

    </select>
  </div>

  <!-- Time Format Selector (always visible) -->
  <div class="setting">
    <label class="checkbox-label">Time Format:</label>
    <select id="timeFormat" name="timeFormat">
      <option value="1" selected>24 Hour</option>
      <option value="0">12 Hour (AM/PM)</option>
    </select>
    <button onclick="saveSettings()">Click to Save</button>
  </div>
</div>


    <div class="section">
      <h2>Display Settings</h2>

      <div class="setting">
        <label>
          <span>Brightness</span>
          <span class="brightness-value" id="brightnessValue">50</span>
        </label>
        <input type="range" id="brightness" name="brightness" min="1" max="255" value="50"
          oninput="document.getElementById('brightnessValue').textContent=this.value">
      </div>

      <div class="setting">
        <label>
          <span>Hour Color</span>
        </label>
        <input type="color" id="hourColor" name="hourColor" value="#ffffff">
      </div>

      <div class="setting">
        <label>
          <span>Minute Color</span>
        </label>
        <input type="color" id="minuteColor" name="minuteColor" value="#0000ff">
      </div>

      <div class="setting">
        <label>
          <span>Second Color</span>
        </label>
        <input type="color" id="secondColor" name="secondColor" value="#ff0000">
      </div>


      <button onclick="saveSettings()">Click to Save</button>

    </div>

    <div class="section">
      <h2>Connections Details</h2>

      <strong>Access URL:</strong> binaryclock.local<br>
      <strong>IP Address:</strong> <span id="ipAddress"></span><br>
      <strong>Mode:</strong> <span id="modeStatus">Real-Time</span><br>
      <strong>Firmware:</strong> <span id="fwVersion">Loading...</span>
    </div>

    <div class="section">
      <h2>WiFi Settings</h2>
      <button class="danger" onclick="resetWiFi()">Reset to Factory Settings</button>
      <div class="warning" style="margin-top: 10px;">
        <strong>Note:</strong> This will restart the clock in setup mode. You will need to reconnect to Axiometa Binary
        Clock WiFi to configure it again.
      </div>
    </div>

    <script>

    function showToast(message, color = "#E2F14F") {
  const toast = document.getElementById('toast');
  toast.textContent = message;
  toast.style.backgroundColor = color;
  toast.className = "show";
  setTimeout(() => {
    toast.className = toast.className.replace("show", "");
  }, 2500);
}


      function updateColorPreview(colorId, previewId) {
        const color = document.getElementById(colorId).value;
        document.getElementById(previewId).style.backgroundColor = color;
      }

      document.getElementById('hourColor').addEventListener('input', function () {
        updateColorPreview('hourColor', 'hourPreview');
      });
      document.getElementById('minuteColor').addEventListener('input', function () {
        updateColorPreview('minuteColor', 'minutePreview');
      });
      document.getElementById('secondColor').addEventListener('input', function () {
        updateColorPreview('secondColor', 'secondPreview');
      });

      function loadSettings() {
        fetch('/api/settings')
          .then(function (response) {
            return response.json();
          })
          .then(function (data) {
            document.getElementById('timezone').value = data.timezone;
            document.getElementById('timeFormat').value = data.timeFormat;
            document.getElementById('brightness').value = data.brightness;
            document.getElementById('brightnessValue').textContent = data.brightness;
            document.getElementById('hourColor').value = data.hourColor;
            document.getElementById('minuteColor').value = data.minuteColor;
            document.getElementById('secondColor').value = data.secondColor;
            document.getElementById('manualHour').value = data.manualHour;
            document.getElementById('manualMinute').value = data.manualMinute;
            document.getElementById('manualSecond').value = data.manualSecond;

            updateColorPreview('hourColor', 'hourPreview');
            updateColorPreview('minuteColor', 'minutePreview');
            updateColorPreview('secondColor', 'secondPreview');
          })
          .catch(function (error) {
            console.error('Error loading settings:', error);
          });
      }

      function setManualTime() {
        const hour = parseInt(document.getElementById('manualHour').value);
        const minute = parseInt(document.getElementById('manualMinute').value);
        const second = parseInt(document.getElementById('manualSecond').value);

        if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
          showToast('Invalid time format.', '#ff3b3b');
          return;
        }

        const manualTime = {
          manualHour: hour,
          manualMinute: minute,
          manualSecond: second
        };

        fetch('/api/manualtime', {
            method: 'POST',
            headers: {
              'Content-Type': 'application/json'
            },
            body: JSON.stringify(manualTime)
          })
          .then(function (response) {
            return response.text();
          })
          .then(function (data) {
            showToast('Custom time set!', '#E2F14F');
          })
          .catch(function (error) {
            showToast('Error setting custom time.', '#ff3b3b');
          });

          saveSettings();

      }

      function saveSettings() {
  const manualEnabled = document.getElementById('manualMode').checked;

  const settings = {
    timezone: document.getElementById('timezone').value,
    timeFormat: document.getElementById('timeFormat').value,
    brightness: document.getElementById('brightness').value,
    hourColor: document.getElementById('hourColor').value,
    minuteColor: document.getElementById('minuteColor').value,
    secondColor: document.getElementById('secondColor').value,
    manualMode: manualEnabled
  };

  // If manual mode is active, include the time fields and send to both APIs
  if (manualEnabled) {
    const hour = parseInt(document.getElementById('manualHour').value);
    const minute = parseInt(document.getElementById('manualMinute').value);
    const second = parseInt(document.getElementById('manualSecond').value);

    if (
      isNaN(hour) || hour < 0 || hour > 23 ||
      isNaN(minute) || minute < 0 || minute > 59 ||
      isNaN(second) || second < 0 || second > 59
    ) {
      showToast('Invalid manual time.', '#ff3b3b');
      return;
    }

    // Attach to settings object
    settings.manualHour = hour;
    settings.manualMinute = minute;
    settings.manualSecond = second;

    // Send manual time to backend
    fetch('/api/manualtime', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        manualHour: hour,
        manualMinute: minute,
        manualSecond: second
      })
    })
    .then(r => r.text())
    .then(() => console.log('Manual time set before saving full settings.'))
    .catch(() => showToast('Error setting manual time.', '#ff3b3b'));
  }

  // Then save all other settings as usual
  fetch('/api/settings', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(settings)
  })
    .then(res => res.text())
    .then(() => showToast('Settings saved!', '#E2F14F'))
    .catch(() => showToast('Error saving settings.', '#ff3b3b'));
}



      function resetWiFi() {
        if (confirm('Reset WiFi settings? The clock will restart in setup mode.')) {
          fetch('/api/resetwifi', {
              method: 'POST'
            })
            .then(function (response) {
              showToast('WiFi reset, restarting...', '#E2F14F');
            });
        }
      }

function updateAllTime() {
  fetch('/api/time').then(function(response) {
    return response.text();
  }).then(function(time) {
    // Update the large time display
    document.getElementById('currentTime').textContent = time;
    
    // Update binary table
    var match = time.match(/(\d{1,2}):(\d{2}):(\d{2})/);
    if (match) {
      var hour = parseInt(match[1]);
      var minute = parseInt(match[2]);
      var second = parseInt(match[3]);
      updateBinaryTable(hour, minute, second);
    }
  }).catch(function(error) {
    document.getElementById('currentTime').textContent = 'Time unavailable';
    console.warn('Clock sync error:', error);
  });
}

      function createBinaryTable() {
  var weights = [32, 16, 8, 4, 2, 1];
  var table = document.getElementById('binaryTable');
  table.innerHTML = '';
  for (var i = 0; i < weights.length; i++) {
    var c = document.createElement('div');
    c.className = 'legend-cell';
    c.textContent = weights[i];
    table.appendChild(c);
  }
  var legendSpacer = document.createElement('div');
  legendSpacer.className = 'legend-sum-spacer';
  table.appendChild(legendSpacer);
  for (var r = 0; r < 3; r++) {
    for (var c = 0; c < 6; c++) {
      var cell = document.createElement('div');
      cell.className = 'binary-cell off';
      cell.id = 'cell-' + r + '-' + c;
      cell.dataset.weight = weights[c];
      table.appendChild(cell);
    }
    var sum = document.createElement('div');
    sum.className = 'binary-sum';
    sum.id = 'sum-' + r;
    sum.textContent = '= 0';
    table.appendChild(sum);
  }
}

function updateBinaryTable(hour, minute, second) {
  var parts = [hour, minute, second];
  var weights = [32, 16, 8, 4, 2, 1];
  for (var r = 0; r < 3; r++) {
    var bin = parts[r].toString(2);
    while (bin.length < 6) bin = '0' + bin;
    var sum = 0;
    for (var c = 0; c < 6; c++) {
      var bit = bin[c];
      var cell = document.getElementById('cell-' + r + '-' + c);
      if (bit === '1') {
        cell.className = 'binary-cell on';
        cell.textContent = weights[c];
        sum += weights[c];
      } else {
        cell.className = 'binary-cell off';
        cell.textContent = '';
      }
    }
    document.getElementById('sum-' + r).textContent = '= ' + sum;
  }
  var hh = hour < 10 ? '0' + hour : '' + hour;
  var mm = minute < 10 ? '0' + minute : '' + minute;
  var ss = second < 10 ? '0' + second : '' + second;
  document.getElementById('binaryExplanation').innerHTML = '<strong>Now:</strong> ' + hh + ' h : ' + mm + ' m : ' + ss + ' s';
}

      fetch('/api/ip')
        .then(function (response) {
          return response.text();
        })
        .then(function (ip) {
          document.getElementById('ipAddress').textContent = ip;
        });

        fetch('/api/version')
        .then(r => r.text())
        .then(v => document.getElementById('fwVersion').textContent = v)
        .catch(() => document.getElementById('fwVersion').textContent = 'Unknown');

      loadSettings();
      createBinaryTable();
      setTimeout(() => {
        updateAllTime();
        setInterval(updateAllTime, 1000);
        }, 1000);

        document.addEventListener("DOMContentLoaded", function () {
  const manualCheckbox = document.getElementById("manualMode");
  const manualSection  = document.getElementById("manualTimeSection");
  const timezoneSelect = document.getElementById("timezone");
  const timeFormat     = document.getElementById("timeFormat");
  const hourInput      = document.getElementById("manualHour");

  // Create AM/PM dropdown dynamically
  let ampmSelect = document.createElement("select");
  ampmSelect.id = "manualAmPm";
  ampmSelect.style.display = "none";
  ampmSelect.innerHTML = `
    <option value="AM">AM</option>
    <option value="PM">PM</option>
  `;
  manualSection.querySelector(".manual-time-inputs").appendChild(ampmSelect);

  function updateVisibility() {
    const manualEnabled = manualCheckbox.checked;
    const is12h = timeFormat.value === "0";

    // Hide timezone when manual mode is active
    timezoneSelect.closest(".setting").style.display = manualEnabled ? "none" : "block";

    // Always keep Time Format visible
    timeFormat.closest(".setting").style.display = "block";

    // Toggle manual time inputs
    manualSection.style.display = manualEnabled ? "block" : "none";

    // Show AM/PM selector only when both manual + 12h are active
    ampmSelect.style.display = (manualEnabled && is12h) ? "block" : "none";

    // Adjust input limits (1–12 or 0–23)
    hourInput.min = is12h ? 1 : 0;
    hourInput.max = is12h ? 12 : 23;
  }

  // Event bindings
  manualCheckbox.addEventListener("change", updateVisibility);
  timeFormat.addEventListener("change", updateVisibility);

  // Initialize once on page load
  updateVisibility();
});
    </script>

    <div id="toast"></div>

</body>

</html>
)rawliteral";
    server.send(200, "text/html", html);
  });

  server.on("/api/settings", HTTP_GET, []() {
    String json = "{";
    json += "\"timezone\":\"" + settings.timezone + "\",";
    json += "\"timeFormat\":" + String(settings.use24Hour ? 1 : 0) + ",";
    json += "\"brightness\":" + String(settings.brightness) + ",";
    json += "\"hourColor\":\"" + rgbToHex(settings.hourColor) + "\",";
    json += "\"minuteColor\":\"" + rgbToHex(settings.minuteColor) + "\",";
    json += "\"secondColor\":\"" + rgbToHex(settings.secondColor) + "\",";
    json += "\"manualMode\":" + String(settings.manualMode ? "true" : "false") + ",";
    json += "\"manualHour\":" + String(settings.manualHour) + ",";
    json += "\"manualMinute\":" + String(settings.manualMinute) + ",";
    json += "\"manualSecond\":" + String(settings.manualSecond);
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/api/settings", HTTP_POST, []() {
    if (server.hasArg("plain")) {
      String body = server.arg("plain");

      if (body.indexOf("timezone") > -1) {
        int start = body.indexOf("timezone\":\"") + 11;
        int end = body.indexOf("\"", start);
        if (end > start) {
          settings.timezone = body.substring(start, end);
          configTzTime(settings.timezone.c_str(), ntpServer);
        }
      }

      if (body.indexOf("timeFormat") > -1) {
        int start = body.indexOf("timeFormat\":\"") + 13;
        int end = body.indexOf("\"", start);
        if (end == -1) {
          start = body.indexOf("timeFormat\":") + 12;
          end = body.indexOf(",", start);
          if (end == -1) end = body.indexOf("}", start);
        }
        settings.use24Hour = body.substring(start, end).toInt() == 1;
      }

      if (body.indexOf("brightness") > -1) {
        int start = body.indexOf("brightness\":\"") + 13;
        int end = body.indexOf("\"", start);
        if (end == -1) {
          start = body.indexOf("brightness\":") + 12;
          end = body.indexOf(",", start);
          if (end == -1) end = body.indexOf("}", start);
        }
        int brightnessValue = body.substring(start, end).toInt();
        if (brightnessValue >= 1 && brightnessValue <= 255) {
          settings.brightness = brightnessValue;
          FastLED.setBrightness(settings.brightness);
        }
      }

      if (body.indexOf("manualMode") > -1) {
        int start = body.indexOf("manualMode\":") + 12;
        int end = body.indexOf(",", start);
        if (end == -1) end = body.indexOf("}", start);
        String modeStr = body.substring(start, end);
        settings.manualMode = (modeStr == "true");
      }

      if (body.indexOf("hourColor") > -1) {
        int start = body.indexOf("hourColor\":\"") + 12;
        int end = body.indexOf("\"", start);
        settings.hourColor = hexToRGB(body.substring(start, end));
      }

      if (body.indexOf("minuteColor") > -1) {
        int start = body.indexOf("minuteColor\":\"") + 14;
        int end = body.indexOf("\"", start);
        settings.minuteColor = hexToRGB(body.substring(start, end));
      }

      if (body.indexOf("secondColor") > -1) {
        int start = body.indexOf("secondColor\":\"") + 14;
        int end = body.indexOf("\"", start);
        settings.secondColor = hexToRGB(body.substring(start, end));
      }

      saveSettings();
      server.send(200, "text/plain", "Settings saved");
    } else {
      server.send(400, "text/plain", "No data received");
    }
  });

  server.on("/api/manualtime", HTTP_POST, []() {
    if (!server.hasArg("plain")) {
      server.send(400, "text/plain", "No data received");
      return;
    }

    String body = server.arg("plain");

    auto extractNumber = [&](const char* key) -> int {
      int keyPos = body.indexOf(key);
      if (keyPos == -1) return -1;

      int colonPos = body.indexOf(":", keyPos);
      if (colonPos == -1) return -1;

      int endPos = body.indexOf(",", colonPos);
      if (endPos == -1) endPos = body.indexOf("}", colonPos);

      String part = body.substring(colonPos + 1, endPos);
      part.trim();

      // Remove all non-numeric characters
      String clean = "";
      for (int i = 0; i < part.length(); i++) {
        if (isDigit(part[i])) clean += part[i];
      }

      return clean.length() ? clean.toInt() : -1;
    };

    int h = extractNumber("manualHour");
    int m = extractNumber("manualMinute");
    int s = extractNumber("manualSecond");

    if (h >= 0) settings.manualHour = h;
    if (m >= 0) settings.manualMinute = m;
    if (s >= 0) settings.manualSecond = s;

    saveSettings();
    server.send(200, "text/plain", "Manual time set");
    lastManualUpdate = millis() - 1000;
  });


  server.on("/api/time", HTTP_GET, []() {
    char timeStr[64];

    if (settings.manualMode) {
      if (settings.use24Hour) {
        sprintf(timeStr, "%02d:%02d:%02d",
                settings.manualHour, settings.manualMinute, settings.manualSecond);
      } else {
        uint8_t displayHour = settings.manualHour;
        const char* ampm = "AM";
        if (settings.manualHour >= 12) {
          ampm = "PM";
          if (settings.manualHour > 12) displayHour -= 12;
        }
        if (displayHour == 0) displayHour = 12;
        sprintf(timeStr, "%02d:%02d:%02d %s",
                displayHour, settings.manualMinute, settings.manualSecond, ampm);
      }
    } else {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        if (settings.use24Hour) {
          strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
        } else {
          strftime(timeStr, sizeof(timeStr), "%I:%M:%S %p", &timeinfo);
        }
      } else {
        strcpy(timeStr, "Time not available");
      }
    }

    server.send(200, "text/plain", String(timeStr));
  });

  server.on("/api/ip", HTTP_GET, []() {
    server.send(200, "text/plain", WiFi.localIP().toString());
  });

  server.on("/api/resetwifi", HTTP_POST, []() {
    preferences.remove("wifiSSID");
    preferences.remove("wifiPass");
    server.send(200, "text/plain", "OK");
    delay(1000);
    ESP.restart();
  });

  server.on("/api/version", HTTP_GET, []() {
    server.send(200, "text/plain", FIRMWARE_VERSION);
  });


  server.begin();
}

/* ---------- Settings Management ---------- */
void loadSettings() {
  settings.timezone = preferences.getString("timezone", "EET-2EEST,M3.5.0/3,M10.5.0/4");
  settings.brightness = preferences.getUChar("brightness", 50);
  settings.use24Hour = preferences.getBool("use24Hour", true);
  settings.manualMode = preferences.getBool("manualMode", false);
  settings.manualHour = preferences.getUChar("manualHour", 12);
  settings.manualMinute = preferences.getUChar("manualMinute", 0);
  settings.manualSecond = preferences.getUChar("manualSecond", 0);

  uint32_t hourRGB = preferences.getULong("hourColor", 0xFFFFFF);
  uint32_t minuteRGB = preferences.getULong("minuteColor", 0xFFFFFF);
  uint32_t secondRGB = preferences.getULong("secondColor", 0xFFFFFF);

  settings.hourColor = CRGB(hourRGB);
  settings.minuteColor = CRGB(minuteRGB);
  settings.secondColor = CRGB(secondRGB);
}

void saveSettings() {
  preferences.putString("timezone", settings.timezone);
  preferences.putUChar("brightness", settings.brightness);
  preferences.putBool("use24Hour", settings.use24Hour);
  preferences.putBool("manualMode", settings.manualMode);
  preferences.putUChar("manualHour", settings.manualHour);
  preferences.putUChar("manualMinute", settings.manualMinute);
  preferences.putUChar("manualSecond", settings.manualSecond);
  preferences.putULong("hourColor", (uint32_t)settings.hourColor);
  preferences.putULong("minuteColor", (uint32_t)settings.minuteColor);
  preferences.putULong("secondColor", (uint32_t)settings.secondColor);
}

void updateWiFiLED() {
  if (WiFi.status() == WL_CONNECTED) {
    wifi_led[0] = CRGB::Green;  // connected
  } else {
    wifi_led[0] = CRGB::Red;  // disconnected or config mode
  }
  FastLED.show();
}


void loadWiFiCredentials() {
  savedSSID = preferences.getString("wifiSSID", "");
  savedPassword = preferences.getString("wifiPass", "");
}

void saveWiFiCredentials(String ssid, String password) {
  preferences.putString("wifiSSID", ssid);
  preferences.putString("wifiPass", password);
  Serial.println("WiFi credentials saved!");
}

CRGB hexToRGB(String hex) {
  if (hex.startsWith("#")) hex = hex.substring(1);
  long rgb = strtol(hex.c_str(), NULL, 16);
  return CRGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

String rgbToHex(CRGB color) {
  String hex = "#";
  if (color.r < 16) hex += "0";
  hex += String(color.r, HEX);
  if (color.g < 16) hex += "0";
  hex += String(color.g, HEX);
  if (color.b < 16) hex += "0";
  hex += String(color.b, HEX);
  return hex;
}

void spinningPerimeterEffect() {
  FastLED.clear();

  const uint8_t perimeterOrder[] = {
    0, 1, 2, 3, 4, 5,    // top row
    11, 17,              // right side down
    16, 15, 14, 13, 12,  // bottom row reversed
    6                    // left side up
  };
  const uint8_t count = sizeof(perimeterOrder);

  unsigned long start = millis();
  uint8_t pos = 0;

  while (millis() - start < 3000) {  // spin for 3 seconds
    FastLED.clear();

    uint8_t i = perimeterOrder[pos % count];
    uint8_t j = perimeterOrder[(pos + 1) % count];
    uint8_t k = perimeterOrder[(pos + 2) % count];

    FastLED.setBrightness(180);  // <-- override user brightness temporarily

    leds[i] = CRGB(0, 0, 255);  // bright head
    leds[j] = CRGB(0, 0, 80);   // trail 1
    leds[k] = CRGB(0, 0, 30);   // trail 2

    FastLED.show();
    pos = (pos + 1) % count;
    delay(60);
  }

  // settle on last shown frame
  FastLED.show();
}



void greenPulseEffect() {
  FastLED.setBrightness(100);
  for (int i = 0; i < 3; i++) {
    for (int b = 0; b < 100; b++) {
      fill_solid(leds, NUM_LEDS, CRGB(0, b * 2.5, 0));
      FastLED.show();
      delay(10);
    }
    for (int b = 100; b >= 0; b--) {
      fill_solid(leds, NUM_LEDS, CRGB(0, b * 2.5, 0));
      FastLED.show();
      delay(10);
    }
  }
  // settle on warm green
  fill_solid(leds, NUM_LEDS, CRGB(0, 250, 0));
  FastLED.show();
  delay(200);
}

void errorRedBlinkEffect() {
  for (int i = 0; i < 4; i++) {
    fill_solid(leds, NUM_LEDS, CRGB(255, 0, 0));
    FastLED.show();
    delay(200);
    FastLED.clear();
    FastLED.show();
    delay(200);
  }
}

void spinningPerimeterEffectNonBlocking() {
  static uint8_t pos = 0;
  static unsigned long lastUpdate = 0;
  const uint8_t perimeterOrder[] = {
    0, 1, 2, 3, 4, 5, 11, 17, 16, 15, 14, 13, 12, 6
  };
  const uint8_t count = sizeof(perimeterOrder);

  if (millis() - lastUpdate < 80) return;
  lastUpdate = millis();

  FastLED.setBrightness(180);  // <-- override user brightness temporarily
  FastLED.clear();

  uint8_t i = perimeterOrder[pos % count];
  uint8_t j = perimeterOrder[(pos + 1) % count];
  uint8_t k = perimeterOrder[(pos + 2) % count];

  leds[i] = CRGB(0, 0, 255);  // head
  leds[j] = CRGB(0, 0, 150);  // trail 1
  leds[k] = CRGB(0, 0, 60);   // trail 2

  FastLED.show();
  pos = (pos + 1) % count;
}


void drawClock(uint8_t hour, uint8_t minute, uint8_t second, bool isPM) {
  FastLED.clear();

  for (uint8_t bit = 0; bit < 5; bit++) {
    if (hour & (1 << bit)) {
      leds[HOUR_LED[bit]] = settings.hourColor;
    }
  }

  for (uint8_t bit = 0; bit < 6; bit++) {
    if (minute & (1 << bit)) {
      leds[MIN_LED[bit]] = settings.minuteColor;
    }
  }

  for (uint8_t bit = 0; bit < 6; bit++) {
    if (second & (1 << bit)) {
      leds[SEC_LED[bit]] = settings.secondColor;
    }
  }

  FastLED.show();
}
