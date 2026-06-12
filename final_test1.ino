// ============================================================
// Project  : IoT-Based Smart Restaurant Ordering System
// Author   : Satheeswaran M
// Hardware : ESP32, PN532 RFID Module, L298N Motor Driver
// Function : Reads RFID tags to control conveyor belt motors
//            and fetches latest orders from Firebase Firestore
// ============================================================

#include <SPI.h>
#include <Adafruit_PN532.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ─────────────────────────────────────────────
// PN532 RFID Module - SPI Pin Configuration
// ─────────────────────────────────────────────
#define PN532_SCK  (18)   // SPI Clock
#define PN532_MOSI (23)   // SPI Master Out Slave In
#define PN532_SS   (5)    // SPI Chip Select (Slave Select)
#define PN532_MISO (19)   // SPI Master In Slave Out
Adafruit_PN532 nfc(PN532_SS);  // Initialize PN532 with SS pin

// ─────────────────────────────────────────────
// L298N Motor Driver Pin Configuration
// Motor A = Left motor, Motor B = Right motor
// ─────────────────────────────────────────────
#define IN1A 26   // Motor A direction pin 1
#define IN2A 27   // Motor A direction pin 2
#define ENA  33   // Motor A PWM speed control (Enable)
#define IN1B 14   // Motor B direction pin 1
#define IN2B 12   // Motor B direction pin 2
#define ENB  13   // Motor B PWM speed control (Enable)

// ─────────────────────────────────────────────
// Push Button for Manual Reset
// ─────────────────────────────────────────────
#define RESET_BUTTON_PIN 4  // Active LOW (uses internal pull-up)

// ─────────────────────────────────────────────
// WiFi Credentials
// ─────────────────────────────────────────────
#define WIFI_SSID     "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ─────────────────────────────────────────────
// Firebase Firestore - REST API Query Endpoint
// Queries the 'restaurant_orders' collection
// ─────────────────────────────────────────────
#define FIRESTORE_QUERY_URL "https://firestore.googleapis.com/v1/projects/foodapp231/databases/(default)/documents:runQuery"

// ─────────────────────────────────────────────
// NTP Time Server Settings (required for HTTPS)
// IST = UTC+5:30 = 19800 seconds offset
// ─────────────────────────────────────────────
const char* ntpServer       = "pool.ntp.org";
const long  gmtOffset_sec   = 19800;  // IST offset in seconds
const int   daylightOffset_sec = 0;   // No daylight saving in India

// ─────────────────────────────────────────────
// RFID Tag UIDs
// Each tag is 4 bytes (Mifare Classic 1K)
// ─────────────────────────────────────────────

// STOP tag: permanently stops the conveyor belt
byte stopUID[] = {0x05, 0xE0, 0xA1, 0x02};

// PAUSE tags: one per table — pauses belt for 30 seconds
// when the correct table's card is scanned during delivery
byte pauseUIDs[][4] = {
  {0x15, 0x87, 0x87, 0x02},  // Table 1
  {0xB5, 0x51, 0x46, 0x01},  // Table 2
  {0xB5, 0x22, 0x0C, 0x01},  // Table 3
  {0xB9, 0x2B, 0x57, 0x16},  // Table 4
  {0x85, 0x38, 0xFF, 0x03}   // Table 5
};

// ─────────────────────────────────────────────
// Global State Variables
// ─────────────────────────────────────────────
bool motorRunning       = true;   // Controls whether motors should run
bool permanentlyStopped = false;  // Set true when STOP tag is scanned
int  activeTableIndex   = -1;     // Index of the table currently being served (-1 = none)
String lastOrderId      = "";     // Tracks last fetched order to avoid duplicate processing

// ============================================================
// SETUP: Runs once on power-up or reset
// ============================================================
void setup() {
  Serial.begin(115200);

  // Configure motor driver pins as outputs
  pinMode(IN1A, OUTPUT); pinMode(IN2A, OUTPUT); pinMode(ENA, OUTPUT);
  pinMode(IN1B, OUTPUT); pinMode(IN2B, OUTPUT); pinMode(ENB, OUTPUT);

  // Configure reset button with internal pull-up (LOW = pressed)
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  stopMotors();  // Ensure motors are off at startup

  // Initialize PN532 RFID reader
  nfc.begin();
  if (!nfc.getFirmwareVersion()) {
    Serial.println("PN532 init failed!");
    while (1);  // Halt if RFID module not detected
  }
  nfc.SAMConfig();  // Configure Security Access Module (standard mode)

  // Connect to WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected");

  // Sync system time via NTP (needed for SSL/TLS certificate validation)
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.print("Syncing time");
  while (time(nullptr) < 1000000000) {  // Wait until epoch time is valid
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nTime synchronized");

  Serial.println("System Ready. Place tag...");
}

// ============================================================
// LOOP: Runs repeatedly after setup
// ============================================================
void loop() {
  // --- Check for manual reset button press ---
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    Serial.println("Reset button pressed! Restarting ESP32...");
    delay(200);       // Debounce delay
    ESP.restart();    // Software reset
  }

  // --- Fetch latest order from Firestore (if WiFi is connected) ---
  if (WiFi.status() == WL_CONNECTED) {
    fetchLatestTableNumber();
  }

  // --- Motor State Control ---
  if (permanentlyStopped || !motorRunning) {
    stopMotors();   // Keep motors off if stopped or paused
  } else {
    runMotors();    // Run conveyor belt normally
  }

  // --- RFID Tag Scanning ---
  uint8_t uid[7];        // Buffer to store scanned UID (up to 7 bytes)
  uint8_t uidLength;     // Actual length of scanned UID

  // Try to read a Mifare ISO14443A tag within 100ms timeout
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 100)) {
    Serial.print("Detected UID: ");
    for (uint8_t i = 0; i < uidLength; i++) {
      Serial.print(uid[i], HEX); Serial.print(" ");
    }
    Serial.println();

    if (uidLength == 4) {  // Only process standard 4-byte Mifare UIDs

      // Check if STOP tag is scanned — permanently halts the belt
      if (memcmp(uid, stopUID, 4) == 0) {
        Serial.println("STOP tag detected. Motors permanently stopped.");
        permanentlyStopped = true;
        motorRunning = false;
      }
      // Check if a PAUSE tag is scanned (only when a table is active and not stopped)
      else if (!permanentlyStopped && activeTableIndex >= 0) {
        if (memcmp(uid, pauseUIDs[activeTableIndex], 4) == 0) {
          // Correct table's card scanned — pause belt for food delivery
          Serial.print("Correct card for active Table detected (Table ");
          Serial.print(activeTableIndex + 1);
          Serial.println(")! Pausing motors...");
          motorRunning = false;
          stopMotors();
          delay(30000);  // Wait 30 seconds for food to be picked up

          // Resume belt after pause (unless permanently stopped)
          if (!permanentlyStopped) {
            Serial.println("Resuming motors...");
            motorRunning = true;
          }
        } else {
          // Wrong card scanned — ignore it
          Serial.println("Wrong card for current active Table. Ignoring...");
        }
      }
    }
    delay(10);  // Short debounce after tag read
  }

  delay(10);  // Small delay to prevent loop from running too fast
}

// ============================================================
// fetchLatestTableNumber()
// Queries Firestore for the most recent order and updates
// activeTableIndex so the correct PAUSE tag is expected
// ============================================================
void fetchLatestTableNumber() {
  HTTPClient http;
  http.begin(FIRESTORE_QUERY_URL);
  http.addHeader("Content-Type", "application/json");

  // Firestore structured query: fetch latest 1 order sorted by createdAt descending
  String queryPayload = R"(
  {
    "structuredQuery": {
      "from": [{ "collectionId": "restaurant_orders" }],
      "orderBy": [{ "field": { "fieldPath": "createdAt" }, "direction": "DESCENDING" }],
      "limit": 1
    }
  })";

  int httpCode = http.POST(queryPayload);

  if (httpCode == 200) {
    String response = http.getString();

    // Parse the JSON response
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, response);

    // Check if a document was returned with valid fields
    if (doc.size() > 0 && doc[0]["document"].containsKey("fields")) {
      String orderId      = doc[0]["document"]["fields"]["orderId"]["stringValue"];
      String customerName = doc[0]["document"]["fields"]["customerName"]["stringValue"];

      // Only process if this is a new order (avoid duplicate handling)
      if (orderId != lastOrderId) {
        lastOrderId = orderId;
        Serial.print("New Order fetched: ");
        Serial.println(customerName);

        // Extract table number from customerName (e.g., "Table 3" → index 2)
        if (customerName.startsWith("Table ")) {
          activeTableIndex = customerName.substring(6).toInt() - 1;
          Serial.print("Active Table Index set to: ");
          Serial.println(activeTableIndex);
        }
      }
    }
  } else {
    Serial.printf("HTTP Request failed. Code: %d\n", httpCode);
  }

  http.end();  // Free HTTP resources
}

// ============================================================
// runMotors()
// Drives both motors forward at ~50% speed (PWM = 127)
// ============================================================
void runMotors() {
  digitalWrite(IN1A, LOW);  digitalWrite(IN2A, HIGH); analogWrite(ENA, 127);
  digitalWrite(IN1B, LOW);  digitalWrite(IN2B, HIGH); analogWrite(ENB, 127);
}

// ============================================================
// stopMotors()
// Cuts power to both motors (coast stop)
// ============================================================
void stopMotors() {
  digitalWrite(IN1A, LOW); digitalWrite(IN2A, LOW); analogWrite(ENA, 0);
  digitalWrite(IN1B, LOW); digitalWrite(IN2B, LOW); analogWrite(ENB, 0);
}
