//ESP32 Diesel Generator Status Logger with RTC

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <Wire.h>
#include "RTClib.h"
#include <WiFiClientSecure.h>

#define RING_SIZE 100  // 100
#define NODE_SIZE 64   // actual 25+1

RTC_DS3231 rtc;


bool lastDGState;
long lastTimeRtcUpdated = 0;
// Wi-Fi credentials
const char* ssid = "";
const char* password = "";

// Google Apps Script Web App URL
const char* scriptURL = "";

// NTP configuration
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;  // GMT+5:30
const int daylightOffset_sec = 0;

// DG input pin
const int dgStatusPin = 18;  // HIGH = OFF, LOW = ON


// LED pins
const int LED_INTERNET = 4;  // Internet status LED
const int LED_DG_PIN = 19;   // DG status LED
const int LED_BUF_PIN = 23;  // Buffer-pending LED
const int LED_PWR_PIN = 5;   // Power supply LED


typedef struct {
  char data[RING_SIZE][NODE_SIZE];
  int head;   // next write index
  int tail;   // next read index (oldest)
  int count;  // number of items in buffer
} RingBuffer;

RingBuffer rb;

char out[NODE_SIZE];

void initRing() {
  rb.head = 0;
  rb.tail = 0;
  rb.count = 0;
  for (int i = 0; i < RING_SIZE; ++i) rb.data[i][0] = '\0';
}

// Add(node): write at head, advance head. If full, advance tail too (overwrite).
void Add(const char* node) {
  // safe copy (truncate if necessary)
  strncpy(rb.data[rb.head], node, NODE_SIZE - 1);
  rb.data[rb.head][NODE_SIZE - 1] = '\0';

  // move head to next write slot
  rb.head = (rb.head + 1) % RING_SIZE;

  if (rb.count == RING_SIZE) {
    // buffer was full; we overwrote the oldest element, so move tail forward
    rb.tail = (rb.tail + 1) % RING_SIZE;
    //printf("[Add] Buffer full: overwrote oldest. head=%d tail=%d count=%d\n", rb.head, rb.tail, rb.count);
  } else {
    rb.count++;
    int writtenIndex = (rb.head + RING_SIZE - 1) % RING_SIZE;  // where we wrote
    //printf("[Add] Added \"%s\" at index %d (head now=%d, count=%d)\n", node, writtenIndex, rb.head, rb.count);
  }
}

// Remove(): takes from tail (oldest). Returns 1 on success and fills out buffer, 0 on empty.
int Remove(char* out) {
  if (rb.count == 0) {
    //printf("[Remove] Buffer empty. Nothing to remove.\n");
    return 0;
  }
  strncpy(out, rb.data[rb.tail], NODE_SIZE - 1);
  out[NODE_SIZE - 1] = '\0';
  //printf("[Remove] Removing \"%s\" from index %d (tail)\n", out, rb.tail);

  // optional: clear the slot so prints show (empty)
  rb.data[rb.tail][0] = '\0';

  rb.tail = (rb.tail + 1) % RING_SIZE;
  rb.count--;
  return 1;
}

// helper: check whether a raw slot index currently holds active data
int slotActive(int idx) {
  if (rb.count == 0) return 0;
  int i = rb.tail;
  for (int k = 0; k < rb.count; ++k) {
    if (i == idx) return 1;
    i = (i + 1) % RING_SIZE;
  }
  return 0;
}

// Print the full raw array (indices 0..RING_SIZE-1), show head/tail flags and logical order
void printBuffer() {
  printf("\n=== Ring Buffer STATE ===\n");
  printf("size=%d node_size=%d  head=%d  tail=%d  count=%d\n", RING_SIZE, NODE_SIZE, rb.head, rb.tail, rb.count);
  for (int i = 0; i < RING_SIZE; ++i) {
    int active = slotActive(i);
    const char* content = active ? rb.data[i] : "(empty)";
    char flags[16] = "";
    if (i == rb.head) strcat(flags, "H");
    if (i == rb.tail) {
      if (flags[0]) strcat(flags, "/T");
      else strcat(flags, "T");
    }
    printf("[%d] %-12s %s\n", i, content, flags);
  }

  // logical view from oldest (tail) -> newest
  printf("Logical (oldest -> newest): ");
  if (rb.count == 0) {
    printf("(empty)");
  } else {
    int i = rb.tail;
    for (int k = 0; k < rb.count; ++k) {
      printf("\"%s\"", rb.data[i]);
      if (k < rb.count - 1) printf(" -> ");
      i = (i + 1) % RING_SIZE;
    }
  }
  printf("\n=========================\n\n");
}


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

//Wifi & Internet Connection
bool connectToWiFi() {
  bool currentWiFiState = WiFi.status() == WL_CONNECTED;

  // Try to connect if not already connected
  if (!currentWiFiState) {
    Serial.print("🌐 Connecting to WiFi");
    WiFi.begin(ssid, password);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts++ < 5) {
      delay(500);
      Serial.print(".");
    }
    currentWiFiState = (WiFi.status() == WL_CONNECTED);
  }

  if (!currentWiFiState) {
    Serial.println("\n✅ WIFI not connected");
    return false;
  }

  WiFiClient tcpClient;
  if (tcpClient.connect("google.com", 80, 1000)) {
    tcpClient.stop();
    LedStatusINTERNET(true);
    //Serial.println("\n✅ Internet connected");
    return true;
  } else {
    // Fallback: HTTP HEAD
    HTTPClient http;
    http.begin("http://clients3.google.com/generate_204");
    http.setTimeout(2000);
    int code = http.sendRequest("HEAD");
    http.end();
    if (code == 204) {
      LedStatusINTERNET(true);
      //Serial.println("\n✅ Internet connected");
      return true;
    }
  }

  LedStatusINTERNET(false);
  Serial.println("\n❌ Internet not connected");
  return false;
}

// Sync RTC from NTP
void configRTC() {
  if (!connectToWiFi()) return;

  struct tm t;
  if (getTimestamp(t)) {
    // Create DateTime from struct tm (RTClib DateTime constructor expects year, month, day, hour, min, sec)
    DateTime dt(
      t.tm_year + 1900,
      t.tm_mon + 1,
      t.tm_mday,
      t.tm_hour,
      t.tm_min,
      t.tm_sec);

    // push the new time to the RTC
    rtc.adjust(dt);
    char buf[70];
    snprintf(buf, sizeof(buf), "RTC configured: %02d/%02d/%04d %02d:%02d:%02d",
            dt.day(), dt.month(), dt.year(), dt.hour(), dt.minute(), dt.second());
    Serial.println(buf);
  }
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
void GetTimeStampFromRTC(char* buf) {
  DateTime now = rtc.now();
  snprintf(buf, NODE_SIZE, "%02u/%02u/%04u %02u:%02u:%02u",
           now.day(),
           now.month(),
           now.year(),
           now.hour(),
           now.minute(),
           now.second());
  return;
}


int validateDate(const char* date) {
  // Expected format: DD/MM/YYYY
  if (strlen(date) != 10) return 0;
  for (int i = 0; i < 10; i++) {
    if (i == 2 || i == 5) {
      if (date[i] != '/') return 0;
    } else {
      if (!isdigit((unsigned char)date[i])) return 0;
    }
  }
  return 1;
}

int validateTime(const char* time) {
  // Expected format: HH:MM:SS
  if (strlen(time) != 8) return 0;
  for (int i = 0; i < 8; i++) {
    if (i == 2 || i == 5) {
      if (time[i] != ':') return 0;
    } else {
      if (!isdigit((unsigned char)time[i])) return 0;
    }
  }
  return 1;
}

int validateStatus(const char* status) {
  return (strcmp(status, "ON") == 0 || strcmp(status, "OFF") == 0);
}

// Upload one entry (REPLACE existing uploadEntry)
bool uploadEntry(char* buf) {
  if (!connectToWiFi()) {
    Serial.println("DEBUG uploadEntry(): no WiFi/internet");
    return false;
  }


  WiFiClientSecure client;
  client.setInsecure();  // pragmatic for Apps Script (insecure TLS verification)

  HTTPClient http;
  http.begin(client, scriptURL);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");



  char* date = strtok(buf, ", ");
  char* time = strtok(NULL, ", ");
  char* status = strtok(NULL, ", ");

  if (date && time && status) {
    printf("Date  : %s\n", date);
    printf("Time  : %s\n", time);
    printf("Status: %s\n", status);

    // Validate
    if (validateDate(date) && validateTime(time) && validateStatus(status)) {
      printf("✅ Format is valid.\n");

      char body[NODE_SIZE];
      snprintf(body, sizeof(body), "date=%s&time=%s&dg=%s", date, time, status);
      Serial.printf("DEBUG uploadEntry(): POST -> %s\n", body);

      int statusCode = http.POST(body);

      if (statusCode > 0) {
        printf("Upload success.\n");
      } else {
        printf("Upload failed.\n");
      }

      http.end();

      // Pragmatic success: treat 2xx and 3xx as success (handles redirects like HTTP 302)
      bool success = (statusCode >= 200 && statusCode < 400);
      if (success) printf("Upload success confirmed.\n");
      return success;

    } else {
      printf("❌ Format is invalid.\n");
      return false;  //TODO
    }
  } else {
    printf("❌ Failed to parse string.\n");
    return false;  //TODO
  }

  return false;
}



void uploadBufferGradually() {

 static bool empty = false; 

  if (!connectToWiFi() && rb.count != 0) {
    Serial.println("Buffer pending but no internet");
    return;
  }

  int result = Remove(out);

  if (result) {
    empty = false;
    uploadEntry(out);
    LedStatusRb(true);
    printf("Uploaded a node data from the ring buffer.\n");
  } else {
    
    LedStatusRb(false);
    if(!empty) printf("Ring buffer is empty.\n");
    empty = true;
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

  printf("starting RTC clock ...\n");
  if (!rtc.begin()) {
    Serial.println("❌ RTC not found. Please check connections!");
    while (1)
      ;
  }
  if (rtc.lostPower()) {
    Serial.println("⚠ RTC lost power, setting time to compile time.");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }



  printf("RTC is running.\n");

  if (connectToWiFi()) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    configRTC();
    printf("RTC clock synced with internet time.\n");
  }

  lastDGState = digitalRead(dgStatusPin);
  printf("Initial state of DG : %s \n", lastDGState == LOW ? "ON" : "OFF");
  LedStatusDG(lastDGState == LOW);  // show DG LED status
  lastTimeRtcUpdated = millis();

  delay(3000);
 // delay(10000);
}

void loop() {
  char entry[NODE_SIZE];
  bool currentDGState = digitalRead(dgStatusPin);

  if (currentDGState != lastDGState) {
    Serial.println("DG state has changed");
    char timestamp[NODE_SIZE];
    GetTimeStampFromRTC(timestamp);
    snprintf(entry, sizeof(entry), "%s, %s", timestamp, (currentDGState == LOW) ? "ON" : "OFF");

    long tt = millis();
    while (millis() - tt < 60000) {
      if (digitalRead(dgStatusPin) == lastDGState) {
        Serial.println("Detected a false trigger");
        printf("Current state of DG : %s\n", lastDGState == LOW ? "ON" : "OFF");
        return;
      }
      delay(50); 
    }

    lastDGState = currentDGState; 
    printf("Current state of DG : %s\n", lastDGState  == LOW? "ON" : "OFF");

    LedStatusDG(currentDGState == LOW);

    Serial.println("No false trigger");

    if (connectToWiFi()) {
      uploadEntry(entry);
    } else {
      Add(entry);
      //printBuffer();
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
