//ESP32 Diesel Generator Status Logger with RTC
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <Wire.h>
#include "RTClib.h"
RTC_DS3231 rtc;

bool lastWiFiState = false;
bool lastInternetState = false;

// Wi-Fi credentials
const char* ssid = "WIFI NAME";
const char* password = "PASSWORD";

// Google Apps Script Web App URL
const char* scriptURL = "https://script.google.com/macros/s/AKfycbxwqMuCwmhqQwhvNHRO_vP8ggXBffqqWTjGoXulzygCehw4ZTcSk8Tn5J3y1wcGsKQYuA/exec";

// NTP configuration
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;     // GMT+5:30
const int daylightOffset_sec = 0;

// DG input pin
const int dgStatusPin = 18;    // HIGH = OFF, LOW = ON

// LED pins
const int LED_INTERNET  = 4;    // Wi-Fi status LED
const int LED_DG_PIN   = 19;   // DG status LED
const int LED_BUF_PIN  = 23;   // Buffer-pending LED
const int LED_PWR_PIN  =  5;   // Power supply LED

// Ring buffer
const int BUFFER_SIZE = 100;
String buffer[BUFFER_SIZE];
int head = 0;
int tail = 0;


// Check if internet is available 
bool internetOK() {
  WiFiClient client;
  return client.connect("8.8.8.8", 53);
}

// DG state
bool lastDGState;
long lastTimeRtcUpdated = 0;

// LED control functions
void LedStatusINTERNET(bool status) {
  digitalWrite(LED_INTERNET, status);
}
void LedStatusDG(bool status) {
  digitalWrite(LED_DG_PIN, status);
}
void LedStatusRb(bool status) {
  digitalWrite(LED_BUF_PIN, status);
}

// Print buffer for debug
void printBuffer() {
  for (int i = 0; i < BUFFER_SIZE; i++) {
    Serial.print(buffer[i]);
    Serial.print(",");
  }
  Serial.println();
}

// Get timestamp from NTP
bool getTimestamp(struct tm& t) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("❌ Time not available");
    return false;
  }
  t = timeinfo;
  return true;
}

// Get formatted time from RTC
String GetTimeStampFromRTC() {
  DateTime now = rtc.now();
  char buf[20];
  snprintf(buf, sizeof(buf), "%02u/%02u/%04u %02u:%02u:%02u",
           now.day(),
           now.month(),
           now.year(),
           now.hour(),
           now.minute(),
           now.second());
  return String(buf);
}

// Add entry to ring buffer
void addToBuffer(String entry) {
  int nextHead = (head + 1) % BUFFER_SIZE;
  if (nextHead == tail) {
    Serial.println("⚠ Buffer full. Overwriting oldest.");
    tail = (tail + 1) % BUFFER_SIZE;
  }
  buffer[head] = entry;
  head = nextHead;
  Serial.println("📝 Buffered: " + entry);
  LedStatusRb(true);
}

// Upload one entry
bool uploadEntry(String entry) {
  if (!connectToWiFi()) return false;
  int sep = entry.indexOf(',');
  String timestamp = entry.substring(0, sep);
  String dgState = entry.substring(sep + 1);

  HTTPClient http;
  http.begin(scriptURL);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String data = "timestamp=" + timestamp + "&dg=" + dgState;
  int code = http.POST(data);
  Serial.printf("📤 Uploading: %s | Code: %d\n", data.c_str(), code);
  http.end();
  return true;                    // you can change to code == 200;
}

// Upload buffer gradually
void uploadBufferGradually() {
  if (connectToWiFi() || tail == head) return;
  while (tail != head) {
    if (uploadEntry(buffer[tail])) {
      tail = (tail + 1) % BUFFER_SIZE;
    } else {
      break;
    }
  }
  // If buffer is empty, turn off LED
  if (tail == head) {
    LedStatusRb(false);
  }
}

//Wifi & Internet Connection
 bool connectToWiFi()
{
  bool currentWiFiState = WiFi.status() == WL_CONNECTED;

  // Try to connect if not already connected
  if (!currentWiFiState) {
    Serial.print("🌐 Connecting to WiFi");
    WiFi.begin(ssid, password);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts++ < 20) {
      delay(500);
      Serial.print(".");
    }
    currentWiFiState = (WiFi.status() == WL_CONNECTED);
  }

  // Print WiFi state only on change
  if (currentWiFiState != lastWiFiState) {
    lastWiFiState = currentWiFiState;
    if (currentWiFiState) {
      Serial.println("\n✅ WiFi connected");
    } else {
      Serial.println("\n❌ WiFi not connected");
      if (lastInternetState != false) {
        lastInternetState = false;
        Serial.println("📡 No internet");
        LedStatusINTERNET(false);
      }
      return false;
    }
  }

  // Check internet: first lightweight TCP
  bool internetOK = false;
  WiFiClient tcpClient;
  if (tcpClient.connect("google.com", 80, 1000)) {
    internetOK = true;
    tcpClient.stop();
  } else {
    // Fallback: HTTP HEAD
    HTTPClient http;
    http.begin("http://clients3.google.com/generate_204");
    http.setTimeout(2000);
    int code = http.sendRequest("HEAD");
    http.end();
    internetOK = (code == 204);
  }

  // Print Internet state only on change
  if (internetOK != lastInternetState) {
    lastInternetState = internetOK;
    if (internetOK) {
      Serial.println("\n✅ Internet connected");
    } else {
      Serial.println("\n❌ Internet not connected");
    }
  }

  LedStatusINTERNET(internetOK);
  return internetOK;
}


// Sync RTC from NTP
void configRTC() {
  if (!connectToWiFi()) return;

  struct tm t;
  if (getTimestamp(t)) {
    DateTime dt(t.tm_year + 1900,
    t.tm_mon + 1, 
    t.tm_mday,
    t.tm_hour,
    t.tm_min,
    t.tm_sec);
    // Push the new time to the RTC
    rtc.adjust(dt);
    char buf[70];
    sprintf(buf, "RTC configured: %02d/%02d/%04d %02d:%02d:%02d",
            dt.day(),
            dt.month(),
            dt.year(),
            dt.hour(),
            dt.minute(),
            dt.second());
    Serial.println(buf);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(dgStatusPin, INPUT_PULLUP);
  pinMode(LED_PWR_PIN, OUTPUT); 
  pinMode(LED_INTERNET, OUTPUT);
  pinMode(LED_DG_PIN, OUTPUT);
  pinMode(LED_BUF_PIN, OUTPUT);
  digitalWrite(LED_PWR_PIN, HIGH);
  LedStatusINTERNET(false);
  LedStatusDG(false);
  LedStatusRb(false);
  Wire.begin();
  if (!rtc.begin()) {
    Serial.println("❌ RTC not found. Please check connections!");
    while (1);
  }
  if (rtc.lostPower()) {
    Serial.println("⚠ RTC lost power, setting time to compile time.");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  Serial.println("✅ RTC is running. Current time:");
  if (connectToWiFi()) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    configRTC();
  }
  lastDGState = digitalRead(dgStatusPin);
  lastTimeRtcUpdated = millis();
  LedStatusDG(lastDGState == LOW); // show DG LED status
}

void loop() {
  bool currentDGState = digitalRead(dgStatusPin);
  if (currentDGState != lastDGState) {
    Serial.println("DG state has changed");
    String dgStr = (currentDGState == LOW) ? "ON" : "OFF";
    String timestamp = GetTimeStampFromRTC();
    String entry = timestamp + "," + dgStr;

    long tt = millis();
    while (millis() - tt < 3000) {
      if (digitalRead(dgStatusPin) == lastDGState) {
        Serial.println("False trigger");
        return;
      }
    }

    lastDGState = currentDGState;    
    LedStatusDG(currentDGState == LOW);

    Serial.println("No false trigger");
    if (connectToWiFi()) {
      uploadEntry(entry);
    } else {
      addToBuffer(entry);
      printBuffer();
    }
  }

  uploadBufferGradually();

  // Update RTC every hour
  if (millis() - lastTimeRtcUpdated > 3600000L) {
    lastTimeRtcUpdated = millis();
    if (connectToWiFi()) {
      configRTC();
    }
  }

}
