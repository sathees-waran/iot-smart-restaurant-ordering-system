// ======================================================================
// Project  : IoT-Based Smart Restaurant Ordering System
// Author   : Satheeswaran M
// Hardware : ESP32, PN532 RFID Module, L298N Motor Driver, li-on Battery
// Function : Reads RFID tags to control the automated food delivery cart's motors
//            and fetches latest orders from Firebase Firestore
//            and Deliver the food to the correct Table.
// ========================================================================

#include <SPI.h>
#include <Adafruit_PN532.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Preferences.h>   // NEW: saves last delivered order in flash (survives restart)

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
// NEW: Timing and Safety Settings
// ─────────────────────────────────────────────
const unsigned long POLL_INTERVAL_MS    = 3000;    // Ask Firestore every 3 s (only when idle)
const unsigned long WIFI_TIMEOUT_MS     = 20000;   // Max wait for WiFi in setup()
const unsigned long NTP_TIMEOUT_MS      = 20000;   // Max wait for NTP in setup()
const unsigned long WIFI_RETRY_MS       = 10000;   // Gap between WiFi reconnect attempts
const uint16_t      HTTP_TIMEOUT_MS     = 5000;    // Max wait for one HTTP request
const bool          CLEAR_SAVED_PROGRESS = false;  // Set true for ONE upload to forget saved progress

// ─────────────────────────────────────────────
// RFID Tag UIDs
// Each tag is 4 bytes (Mifare Classic 1K)
// ─────────────────────────────────────────────

// STOP tag: permanently stops the cart
byte stopUID[] = {0x05, 0xE0, 0xA1, 0x02};

// PAUSE tags: one per table — pauses cart for 30 seconds
// when the correct table's card is scanned during delivery
byte pauseUIDs[][4] = {
  {0x15, 0x87, 0x87, 0x02},  // Table 1
  {0xB5, 0x51, 0x46, 0x01},  // Table 2
  {0xB5, 0x22, 0x0C, 0x01},  // Table 3
  {0xB9, 0x2B, 0x57, 0x16},  // Table 4
  {0x85, 0x38, 0xFF, 0x03}   // Table 5
};
const int NUM_TABLES = sizeof(pauseUIDs) / sizeof(pauseUIDs[0]);  // NEW: = 5

// ─────────────────────────────────────────────
// Global State Variables
// ─────────────────────────────────────────────
bool motorRunning       = true;   // Controls whether motors should run
bool permanentlyStopped = false;  // Set true when STOP tag is scanned
int  activeTableIndex   = -1;     // Index of the table currently being served (-1 = none)

// NEW: replaces lastOrderId
Preferences prefs;                  // Flash storage handle
char lastDeliveredTime[40] = "";    // createdAt of the last delivered order (saved in flash)
char pendingCreatedAt[40]  = "";    // createdAt of the order currently being delivered
bool deliveredThisRun      = false; // One order per run: stop polling after delivery
unsigned long lastPollTime  = 0;    // millis() of last Firestore poll
unsigned long lastWifiRetry = 0;    // millis() of last WiFi reconnect attempt

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
    Serial.println("PN532 init failed! Restarting...");  // CHANGED: restart instead of freezing
    delay(3000);
    ESP.restart();
  }
  nfc.SAMConfig();  // Configure Security Access Module (standard mode)

  // Connect to WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  unsigned long wifiStart = millis();  // NEW: timeout timer
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - wifiStart > WIFI_TIMEOUT_MS) {  // NEW
      Serial.println("\nWiFi timeout! Restarting...");
      ESP.restart();
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected");

  // Sync system time via NTP (needed for SSL/TLS certificate validation)
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.print("Syncing time");
  unsigned long ntpStart = millis();  // NEW: timeout timer
  while (time(nullptr) < 1000000000) {  // Wait until epoch time is valid
    if (millis() - ntpStart > NTP_TIMEOUT_MS) {  // NEW
      Serial.println("\nNTP timeout! Restarting...");
      ESP.restart();
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nTime synchronized");

  // NEW: Load the last delivered order time from flash
  prefs.begin("cart", false);
  if (CLEAR_SAVED_PROGRESS) {
    prefs.clear();
  }
  lastDeliveredTime[0] = '\0';
  prefs.getString("lastTime", lastDeliveredTime, sizeof(lastDeliveredTime));
  if (strlen(lastDeliveredTime) == 0) {
    // First boot: start from "now" so old test orders are not delivered
    time_t now = time(nullptr);
    struct tm utc;
    gmtime_r(&now, &utc);
    strftime(lastDeliveredTime, sizeof(lastDeliveredTime), "%Y-%m-%dT%H:%M:%SZ", &utc);
    prefs.putString("lastTime", lastDeliveredTime);
    Serial.print("First boot. Starting from: ");
  } else {
    Serial.print("Last delivered order time: ");
  }
  Serial.println(lastDeliveredTime);

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

  // --- CHANGED: Fetch order only when idle, using a timer (no HTTP while delivering) ---
  if (WiFi.status() == WL_CONNECTED) {
    if (activeTableIndex == -1 && !deliveredThisRun && !permanentlyStopped &&
        millis() - lastPollTime >= POLL_INTERVAL_MS) {
      fetchLatestTableNumber();
      lastPollTime = millis();
    }
  }
  // --- NEW: WiFi reconnect (cart keeps running; only new orders wait) ---
  else if (millis() - lastWifiRetry >= WIFI_RETRY_MS) {
    lastWifiRetry = millis();
    Serial.println("WiFi lost. Reconnecting...");
    WiFi.reconnect();
  }

  // --- Motor State Control ---
  if (permanentlyStopped || !motorRunning) {
    stopMotors();   // Keep motors off if stopped or paused
  } else {
    runMotors();    // Run cart normally
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

      // Check if STOP tag is scanned — permanently halts the cart
      if (memcmp(uid, stopUID, 4) == 0) {
        Serial.println("STOP tag detected. Motors permanently stopped.");
        permanentlyStopped = true;
        motorRunning = false;
      }
      // Check if a PAUSE tag is scanned (only when a table is active and not stopped)
      else if (!permanentlyStopped && activeTableIndex >= 0) {
        if (memcmp(uid, pauseUIDs[activeTableIndex], 4) == 0) {
          // Correct table's card scanned — pause cart for food delivery
          Serial.print("Correct card for active Table detected (Table ");
          Serial.print(activeTableIndex + 1);
          Serial.println(")! Pausing motors...");
          motorRunning = false;
          stopMotors();
          delay(30000);  // Wait 30 seconds for food to be picked up

          // NEW: Order delivered. Save progress and clear the active table
          markOrderDone(pendingCreatedAt);
          activeTableIndex = -1;
          deliveredThisRun = true;
          Serial.println("Order delivered. Progress saved.");

          // Resume cart after pause (unless permanently stopped)
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
// CHANGED: Queries Firestore for the OLDEST order created after
// the last delivered one, and updates activeTableIndex so the
// correct PAUSE tag is expected
// ============================================================
void fetchLatestTableNumber() {
  HTTPClient http;
  http.begin(FIRESTORE_QUERY_URL);
  http.setTimeout(HTTP_TIMEOUT_MS);  // NEW: do not wait forever
  http.addHeader("Content-Type", "application/json");

  // Firestore structured query:
  // createdAt > lastDelivered, oldest first, only 1 order,
  // and only the 2 fields we need (keeps the response small)
  char queryPayload[600];
  snprintf(queryPayload, sizeof(queryPayload),
    "{\"structuredQuery\":{"
      "\"from\":[{\"collectionId\":\"restaurant_orders\"}],"
      "\"where\":{\"fieldFilter\":{\"field\":{\"fieldPath\":\"createdAt\"},"
        "\"op\":\"GREATER_THAN\",\"value\":{\"timestampValue\":\"%s\"}}},"
      "\"orderBy\":[{\"field\":{\"fieldPath\":\"createdAt\"},\"direction\":\"ASCENDING\"}],"
      "\"select\":{\"fields\":[{\"fieldPath\":\"customerName\"},{\"fieldPath\":\"createdAt\"}]},"
      "\"limit\":1}}",
    lastDeliveredTime);

  int httpCode = http.POST((uint8_t*)queryPayload, strlen(queryPayload));

  if (httpCode == 200) {
    String response = http.getString();

    // Parse the JSON response
    DynamicJsonDocument doc(3072);
    DeserializationError err = deserializeJson(doc, response);  // NEW: check for errors

    if (err) {
      Serial.print("JSON parse failed: ");
      Serial.println(err.c_str());
    } else {
      // When no new order exists, Firestore returns an entry without "document"
      JsonObject fields = doc[0]["document"]["fields"];

      if (!fields.isNull()) {
        const char* customerName = fields["customerName"]["stringValue"] | "";
        const char* createdAt    = fields["createdAt"]["timestampValue"] | "";

        // Extract table number from customerName (e.g., "Table 3" → index 2)
        if (strlen(createdAt) > 0 && strncmp(customerName, "Table ", 6) == 0) {
          int tableIndex = atoi(customerName + 6) - 1;

          if (tableIndex >= 0 && tableIndex < NUM_TABLES) {
            strncpy(pendingCreatedAt, createdAt, sizeof(pendingCreatedAt) - 1);
            pendingCreatedAt[sizeof(pendingCreatedAt) - 1] = '\0';
            activeTableIndex = tableIndex;

            Serial.print("New Order fetched: ");
            Serial.println(customerName);
            Serial.print("Active Table Index set to: ");
            Serial.println(activeTableIndex);
          } else {
            // Bad table number: skip this order so it cannot block newer orders
            Serial.println("Invalid table number. Skipping order.");
            markOrderDone(createdAt);
          }
        } else if (strlen(createdAt) > 0) {
          // Unexpected customerName format: skip this order
          Serial.println("Unexpected customerName. Skipping order.");
          markOrderDone(createdAt);
        }
      }
    }
  } else {
    Serial.printf("HTTP Request failed. Code: %d\n", httpCode);
    if (httpCode > 0) {
      Serial.println(http.getString());  // NEW: show Firestore's error message for debugging
    }
  }

  http.end();  // Free HTTP resources
}

// ============================================================
// markOrderDone()
// NEW: Remembers this order's createdAt as the last delivered
// one, in RAM and in flash (survives the button restart)
// ============================================================
void markOrderDone(const char* createdAt) {
  strncpy(lastDeliveredTime, createdAt, sizeof(lastDeliveredTime) - 1);
  lastDeliveredTime[sizeof(lastDeliveredTime) - 1] = '\0';
  prefs.putString("lastTime", lastDeliveredTime);
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
