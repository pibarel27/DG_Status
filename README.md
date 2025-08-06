# 🚜 ESP32 Diesel Generator Status Logger with RTC

This project logs **Diesel Generator (DG)** ON/OFF timestamps using an **ESP32**, a **DS3231 RTC**, and **Google Sheets** as a cloud-based logbook via **Google Apps Script Web App**.

---
<div align="center">
  <img alt="Demo" src="/photo.jpg" />
</div

## 📦 Features

- ✅ DG ON/OFF detection via GPIO input (with debouncing).
- ✅ Timestamp logging using DS3231 Real-Time Clock.
- ✅ Internet check and status LED indication.
- ✅ Data upload to Google Sheets via HTTP.
- ✅ Offline logging using ring buffer (up to 100 entries).
- ✅ Reconnect Wi-Fi and upload pending logs automatically.
- ✅ LED indicators for:
  - Power (LED_PWR)
  - DG Status (LED_DG)
  - Internet Status (LED_INTERNET)
  - Buffer Pending (LED_BUF)

---

## 🔌 Hardware Components

| Component           | Description                              |
|--------------------|------------------------------------------|
| ESP32 Board        | Main microcontroller                     |
| DS3231 RTC Module  | Real-Time Clock for timestamping         |
| Relay/Input Signal | GPIO18 input (LOW = DG ON, HIGH = OFF)  |
| 4x LEDs + 220Ω     | Power, Internet, DG, Buffer LEDs         |
| Power Supply       | Powered via DG’s 12V-to-5V converter     |
| Wi-Fi              | Used for uploading data to Google Sheets |

---

## 📐 Pin Configuration

| Signal            | GPIO  | Description               |
|------------------|-------|---------------------------|
| DG Status Input  | 18    | Detects ON/OFF from Relay |
| Power LED        | 5     | Constant ON when powered  |
| Internet LED     | 4     | ON when internet is up    |
| DG LED           | 19    | ON when DG is ON          |
| Buffer LED       | 23    | ON when buffer not empty  |

---

## 🌐 Google Sheets Integration

1. Create a Google Sheet with columns:  
   `Date Time` | `DG Status`

3. Create an Apps Script (Extensions > Apps Script) with this code:

   ```javascript
   function doPost(e) {
    if (!e || !e.parameter) {
    return ContentService.createTextOutput("❌ No data received");
    }

    var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
    var date = e.parameter.date;
    var time = e.parameter.time;
    var dg = e.parameter.dg;

    if (date && time && dg) {
    sheet.appendRow([new Date(), date, time, dg]);
    return ContentService.createTextOutput("✅ Success");
    } else {
    return ContentService.createTextOutput("⚠️ Missing parameter");
    }
    }

