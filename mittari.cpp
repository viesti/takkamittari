#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>

// Hardware pins for XIAO ESP32C3
const int sdaPin = 6;
const int sclPin = 7;
const int oneWireBus = 2;

OneWire oneWire(oneWireBus);
DallasTemperature sensors(&oneWire);
LiquidCrystal_I2C lcd(0x27, 16, 2);

WiFiClient espClient;
PubSubClient mqtt(espClient);
Preferences prefs;

// Persistent buffer for PubSubClient so hostname pointer never invalidates
char mqttHostBuf[64];

// Runtime Configuration Struct
struct Config {
  String ssid;
  String wifiPass;
  String mqttHost;
  uint16_t mqttPort;
  String mqttUser;
  String mqttPass;
  String mqttTopic;
  uint32_t mqttIntervalSec;
} cfg;

// Timing and state variables
unsigned long lastTempRequest = 0;
unsigned long lastMqttPublish = 0;
unsigned long lastMqttCheck = 0;

const unsigned long tempUpdateIntervalMs = 2000;
const unsigned long conversionTimeMs = 750; // 12-bit DS18B20 conversion time
bool waitingForConversion = false;
float currentTempC = DEVICE_DISCONNECTED_C;

String serialBuffer = "";

// --- NVS Configuration Management ---

void loadConfig() {
  prefs.begin("iot-cfg", true); // Read-only mode
  cfg.ssid            = prefs.getString("ssid", "");
  cfg.wifiPass        = prefs.getString("wpass", "");
  cfg.mqttHost        = prefs.getString("mhost", "");
  cfg.mqttPort        = prefs.getUShort("mport", 1883);
  cfg.mqttUser        = prefs.getString("muser", "");
  cfg.mqttPass        = prefs.getString("mpass", "");
  cfg.mqttTopic       = prefs.getString("mtopic", "home/xiao_c3/temperature");
  cfg.mqttIntervalSec = prefs.getUInt("mint", 300); // Default 300s (5 min)
  prefs.end();

  cfg.ssid.trim();
  cfg.wifiPass.trim();
  cfg.mqttHost.trim();
  cfg.mqttUser.trim();
  cfg.mqttPass.trim();
  cfg.mqttTopic.trim();
}

void saveStringPref(const char* key, const String& val) {
  prefs.begin("iot-cfg", false);
  prefs.putString(key, val);
  prefs.end();
}

void saveUIntPref(const char* key, uint32_t val) {
  prefs.begin("iot-cfg", false);
  prefs.putUInt(key, val);
  prefs.end();
}

void saveUShortPref(const char* key, uint16_t val) {
  prefs.begin("iot-cfg", false);
  prefs.putUShort(key, val);
  prefs.end();
}

// --- Network & MQTT Management ---

void updateStatusRow() {
  lcd.setCursor(0, 0);
  String status = "W:";
  status += (WiFi.status() == WL_CONNECTED) ? "OK " : "-- ";
  status += "M:";
  status += (mqtt.connected()) ? "OK       " : "--       ";
  lcd.print(status.substring(0, 16));
}

// Diagnostic event handler for ESP32 Wi-Fi stack
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[WiFi Event] Associated with AP, waiting for DHCP IP...");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[WiFi Event] Connected! IP: %s (RSSI: %d dBm)\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[WiFi Event] Disconnected. Reason code: %u\n",
                    info.wifi_sta_disconnected.reason);
      break;
    default:
      break;
  }
}

void connectWiFi() {
  if (cfg.ssid.isEmpty()) {
    Serial.println("[WiFi] No SSID configured. Use 'ssid <name>' to set.");
    return;
  }

  Serial.printf("[WiFi] Connecting to '%s'...\n", cfg.ssid.c_str());
  WiFi.disconnect(false); // Drop active connection without wiping driver state
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // Disable modem sleep for reliable ping & MQTT
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid.c_str(), cfg.wifiPass.c_str());
}

void scanWiFi() {
  Serial.println("[WiFi] Scanning 2.4GHz networks...");
  int n = WiFi.scanNetworks();
  if (n == 0) {
    Serial.println("[WiFi] No networks found! Check u.FL antenna connection.");
  } else {
    Serial.printf("[WiFi] Found %d networks:\n", n);
    for (int i = 0; i < n; ++i) {
      Serial.printf("  %d: %s (%d dBm) Ch:%d\n", 
                    i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
    }
  }
  WiFi.scanDelete();
}

void publishHADiscovery() {
  String discTopic = "homeassistant/sensor/xiao_esp32c3_temp/config";
  String payload = String("{") +
    "\"name\":\"XIAO Temperature\"," +
    "\"stat_t\":\"" + cfg.mqttTopic + "\"," +
    "\"unit_of_meas\":\"°C\"," +
    "\"dev_cla\":\"temperature\"," +
    "\"stat_cla\":\"measurement\"," +
    "\"uniq_id\":\"xiao_esp32c3_ds18b20_01\"," +
    "\"dev\":{\"ids\":[\"xiao_esp32c3_01\"],\"name\":\"XIAO ESP32-C3 Sensor\",\"mdl\":\"XIAO ESP32-C3\",\"mf\":\"Seeed Studio\"}" +
    "}";

  mqtt.setBufferSize(512);
  if (mqtt.publish(discTopic.c_str(), payload.c_str(), true)) {
    Serial.println("[MQTT] Published Home Assistant auto-discovery configuration.");
  }
}

void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED || cfg.mqttHost.isEmpty()) return;

  // Safely handle IP address vs hostname parsing for PubSubClient
  IPAddress brokerIP;
  if (brokerIP.fromString(cfg.mqttHost)) {
    mqtt.setServer(brokerIP, cfg.mqttPort); // Passed by value
  } else {
    cfg.mqttHost.toCharArray(mqttHostBuf, sizeof(mqttHostBuf));
    mqtt.setServer(mqttHostBuf, cfg.mqttPort); // Points to static buffer
  }

  mqtt.setSocketTimeout(5);
  mqtt.setBufferSize(512);

  String clientId = "xiao-c3-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  Serial.printf("[MQTT] Connecting to %s:%u as %s...\n", 
                cfg.mqttHost.c_str(), cfg.mqttPort, clientId.c_str());

  bool connected = false;
  if (cfg.mqttUser.isEmpty()) {
    connected = mqtt.connect(clientId.c_str());
  } else {
    connected = mqtt.connect(clientId.c_str(), cfg.mqttUser.c_str(), cfg.mqttPass.c_str());
  }

  if (connected) {
    Serial.println("[MQTT] Connected.");
    publishHADiscovery();
    lastMqttPublish = millis() - (cfg.mqttIntervalSec * 1000UL);
  } else {
    Serial.printf("[MQTT] Failed, rc=%d (Local IP: %s)\n", 
                  mqtt.state(), WiFi.localIP().toString().c_str());
  }
  updateStatusRow();
}

void publishTemperature() {
  if (!mqtt.connected() || currentTempC == DEVICE_DISCONNECTED_C) return;

  char payload[16];
  snprintf(payload, sizeof(payload), "%.2f", currentTempC);

  if (mqtt.publish(cfg.mqttTopic.c_str(), payload, true)) {
    Serial.printf("[MQTT] Published %s C to %s\n", payload, cfg.mqttTopic.c_str());
  } else {
    Serial.println("[MQTT] Publish failed!");
  }
}

// --- Non-Blocking Serial Configurator ---

void printHelp() {
  Serial.println("\n--- XIAO ESP32-C3 Serial Configurator ---");
  Serial.println("Commands:");
  Serial.println("  show                   : Display current configuration & status");
  Serial.println("  scan                   : Scan visible 2.4GHz WiFi networks & RSSI");
  Serial.println("  reconnect              : Force WiFi reconnect");
  Serial.println("  ssid <name>            : Set WiFi ESSID");
  Serial.println("  wpass <secret>         : Set WiFi WPA2 password");
  Serial.println("  mhost <ip_or_host>     : Set MQTT broker host/IP");
  Serial.println("  mport <port>           : Set MQTT broker port (default 1883)");
  Serial.println("  muser <username>       : Set MQTT username (leave empty for none)");
  Serial.println("  mpass <password>       : Set MQTT password");
  Serial.println("  mtopic <topic>         : Set MQTT state topic");
  Serial.println("  interval <seconds>     : Set MQTT publish interval in seconds");
  Serial.println("  pub                    : Force immediate MQTT publish");
  Serial.println("  reboot                 : Restart microcontroller");
  Serial.println("-----------------------------------------");
}

void printConfig() {
  Serial.println("\n--- Current Configuration ---");
  Serial.printf("WiFi SSID     : '%s'\n", cfg.ssid.c_str());
  Serial.printf("WiFi Pass Len : %u chars\n", cfg.wifiPass.length());
  Serial.printf("WiFi Status   : %s (status=%d, IP=%s, RSSI=%d dBm)\n", 
                (WiFi.status() == WL_CONNECTED) ? "Connected" : "Disconnected",
                WiFi.status(),
                WiFi.localIP().toString().c_str(),
                WiFi.RSSI());
  Serial.printf("MQTT Broker   : '%s':%u\n", cfg.mqttHost.c_str(), cfg.mqttPort);
  Serial.printf("MQTT User     : '%s' (Pass len: %u)\n", cfg.mqttUser.c_str(), cfg.mqttPass.length());
  Serial.printf("MQTT Topic    : '%s'\n", cfg.mqttTopic.c_str());
  Serial.printf("MQTT Status   : %s (state=%d)\n", mqtt.connected() ? "Connected" : "Disconnected", mqtt.state());
  Serial.printf("Send Interval : %u seconds\n", cfg.mqttIntervalSec);
  Serial.printf("Current Temp  : %.2f C\n", currentTempC);
  Serial.println("-----------------------------");
}

void processCommand(String line) {
  line.trim();
  if (line.isEmpty()) return;

  int spaceIdx = line.indexOf(' ');
  String cmd = (spaceIdx == -1) ? line : line.substring(0, spaceIdx);
  String arg = (spaceIdx == -1) ? "" : line.substring(spaceIdx + 1);
  arg.trim();
  cmd.toLowerCase();

  if (cmd == "help" || cmd == "?") {
    printHelp();
  } else if (cmd == "show" || cmd == "status") {
    printConfig();
  } else if (cmd == "scan") {
    scanWiFi();
  } else if (cmd == "reconnect") {
    connectWiFi();
  } else if (cmd == "ssid") {
    cfg.ssid = arg;
    saveStringPref("ssid", arg);
    Serial.printf("Saved SSID: '%s'. Reconnecting...\n", arg.c_str());
    connectWiFi();
  } else if (cmd == "wpass") {
    cfg.wifiPass = arg;
    saveStringPref("wpass", arg);
    Serial.printf("Saved WiFi password (%u chars). Reconnecting...\n", arg.length());
    connectWiFi();
  } else if (cmd == "mhost") {
    cfg.mqttHost = arg;
    saveStringPref("mhost", arg);
    Serial.printf("Saved MQTT host: '%s'\n", arg.c_str());
    mqtt.disconnect();
  } else if (cmd == "mport") {
    uint16_t port = (uint16_t)arg.toInt();
    if (port > 0) {
      cfg.mqttPort = port;
      saveUShortPref("mport", port);
      Serial.printf("Saved MQTT port: %u\n", port);
      mqtt.disconnect();
    }
  } else if (cmd == "muser") {
    cfg.mqttUser = arg;
    saveStringPref("muser", arg);
    Serial.printf("Saved MQTT user: '%s'\n", arg.c_str());
    mqtt.disconnect();
  } else if (cmd == "mpass") {
    cfg.mqttPass = arg;
    saveStringPref("mpass", arg);
    Serial.printf("Saved MQTT password (%u chars).\n", arg.length());
    mqtt.disconnect();
  } else if (cmd == "mtopic") {
    cfg.mqttTopic = arg;
    saveStringPref("mtopic", arg);
    Serial.printf("Saved MQTT topic: '%s'\n", arg.c_str());
  } else if (cmd == "interval") {
    uint32_t sec = (uint32_t)arg.toInt();
    if (sec >= 5) {
      cfg.mqttIntervalSec = sec;
      saveUIntPref("mint", sec);
      Serial.printf("Saved publish interval: %u seconds\n", sec);
    } else {
      Serial.println("Error: Minimum interval is 5 seconds.");
    }
  } else if (cmd == "pub") {
    publishTemperature();
  } else if (cmd == "reboot") {
    Serial.println("Rebooting...");
    delay(200);
    ESP.restart();
  } else {
    Serial.printf("Unknown command: '%s'. Type 'help' for commands.\n", cmd.c_str());
  }
}

void handleSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (!serialBuffer.isEmpty()) {
        processCommand(serialBuffer);
        serialBuffer = "";
      }
    } else {
      serialBuffer += c;
    }
  }
}

// --- Main Setup & Loop ---

void setup() {
  Serial.begin(115200);
  Wire.begin(sdaPin, sclPin);

  sensors.begin();
  sensors.setWaitForConversion(false); // Non-blocking sensor read

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("W:-- M:--       ");
  lcd.setCursor(0, 1);
  lcd.print("Temp:       C");

  WiFi.onEvent(onWiFiEvent);

  loadConfig();
  connectWiFi();
}

void loop() {
  unsigned long now = millis();

  // 1. Process serial configurator input immediately
  handleSerial();

  // 2. Non-blocking DS18B20 temperature state machine
  if (!waitingForConversion && (now - lastTempRequest >= tempUpdateIntervalMs)) {
    sensors.requestTemperatures();
    lastTempRequest = now;
    waitingForConversion = true;
  }

  if (waitingForConversion && (now - lastTempRequest >= conversionTimeMs)) {
    waitingForConversion = false;
    currentTempC = sensors.getTempCByIndex(0);

    lcd.setCursor(6, 1);
    if (currentTempC == DEVICE_DISCONNECTED_C) {
      lcd.print("Virhe!  ");
      Serial.println("Lämpötila: Virhe (Anturi irti)");
    } else {
      lcd.print(currentTempC, 1);
      lcd.print(" C  ");
      Serial.printf("Lämpötila: %.2f C\n", currentTempC);
    }
    updateStatusRow();
  }

  // 3. Maintain MQTT connection (Wi-Fi auto-reconnect is handled natively by ESP-IDF)
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) {
      if (now - lastMqttCheck >= 5000) {
        lastMqttCheck = now;
        connectMQTT();
      }
    } else {
      mqtt.loop();
    }
  }

  // 4. Periodic telemetry publish
  if (mqtt.connected() && (now - lastMqttPublish >= (cfg.mqttIntervalSec * 1000UL))) {
    lastMqttPublish = now;
    publishTemperature();
  }
}
