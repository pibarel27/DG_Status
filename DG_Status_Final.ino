//ESP32 Diesel Generator Status Logger with RTC
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <Wire.h>
#include "RTClib.h"
#include <WiFiClientSecure.h> 
RTC_DS3231 rtc;


unsigned long dgChangeStartTime = 0;
bool dgChangePending = false;
bool currentDGState = HIGH; 

// Wi-Fi credentials
const char* ssid = "WIFI_NAME";
const char* password = "PASSWORD";

// Google Apps Script Web App URL
const char* scriptURL = "Web App URL";

// NTP configuration
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;     // GMT+5:30
const int daylightOffset_sec = 0;

// DG input pin
const int dgStatusPin = 18;    // HIGH = OFF, LOW = ON

bool lastWiFiState = false;
bool lastInternetState = false;
// LED pins
const int LED_INTERNET  = 4;    // Wi-Fi status LED
const int LED_DG_PIN   = 19;   // DG status LED
const int LED_BUF_PIN  = 23;   // Buffer-pending LED
const int LED_PWR_PIN  =  5;   // Power supply LED

// Ring buffer
const int BUFFER_SIZE = 300;
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

  int commaPos = entry.indexOf(',');
  String ts       = entry.substring(0, commaPos);
  String dgState  = entry.substring(commaPos + 1);
  int spacePos    = ts.indexOf(' ');
  String dateStr  = ts.substring(0, spacePos);       
  String timeStr  = ts.substring(spacePos + 1);    

  WiFiClientSecure client;
  client.setInsecure();   

  HTTPClient http;
  http.begin(client, scriptURL);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "date=" + dateStr
              + "&time=" + timeStr
              + "&dg="   + dgState;

  int statusCode = http.POST(body);
  Serial.printf("📤 POST [%s] → HTTP %d\n", body.c_str(), statusCode);

  http.end();
  return (statusCode == 200);   // or `return (statusCode == 200);
}

// Upload buffer gradually
void uploadBufferGradually() {
  if (!connectToWiFi() || tail == head) return;
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

  if (currentWiFiState != lastWiFiState) {
    lastWiFiState = currentWiFiState;
    if (currentWiFiState) {
      Serial.println("\n✅ WiFi connected");
    } else {
      Serial.println("\n❌ WiFi not connected");
      // If WiFi isn't connected, internet can't be either
      if (lastInternetState != false) {
        lastInternetState = false;
        Serial.println("📡 No internet");
        LedStatusINTERNET(false);
      }
      return false;
    }
  }

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
    // Create DateTime from struct tm (RTClib DateTime constructor expects year, month, day, hour, min, sec)
    DateTime dt(
      t.tm_year + 1900,
      t.tm_mon  + 1,
      t.tm_mday,
      t.tm_hour,
      t.tm_min,
      t.tm_sec
    );

    // push the new time to the RTC
    rtc.adjust(dt);
    char buf[70];
    sprintf(buf, "RTC configured: %02d/%02d/%04d %02d:%02d:%02d",
            dt.day(), dt.month(), dt.year(), dt.hour(), dt.minute(), dt.second());
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
currentDGState = digitalRead(dgStatusPin);
lastDGState = currentDGState;
lastTimeRtcUpdated = millis();
LedStatusDG(currentDGState == LOW); // show DG LED status

}

void loop() {
  bool newDGState = digitalRead(dgStatusPin);

  if (newDGState != currentDGState && !dgChangePending) {
    dgChangeStartTime = millis();
    dgChangePending = true;
    Serial.println("🕒 DG state change detected, validating...");
  }

  if (dgChangePending && (millis() - dgChangeStartTime >= 180000)) {
    bool confirmedState = digitalRead(dgStatusPin);

    if (confirmedState != currentDGState) {
      currentDGState = confirmedState;
      dgChangePending = false;

      Serial.println("✅ DG state change confirmed");
      String dgStr = (currentDGState == LOW) ? "ON" : "OFF";
      String timestamp = GetTimeStampFromRTC();
      String entry = timestamp + "," + dgStr;

      LedStatusDG(currentDGState == LOW);

      if (connectToWiFi()) {
        uploadEntry(entry);
      } else {
        addToBuffer(entry);
        printBuffer();
      }
    } else {
      Serial.println("❌ False DG trigger — change reverted");
      dgChangePending = false;
    }
  }

  uploadBufferGradually();

  // RTC update hourly
  if (millis() - lastTimeRtcUpdated > 3600000L) {
    lastTimeRtcUpdated = millis();
    if (connectToWiFi()) {
      configRTC();
    }
  }
}
