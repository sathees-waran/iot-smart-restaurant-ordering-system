// Satheeswaran
// WiFi + Firestore + RFID + Motor Control (Table-specific pause)

#include <SPI.h>
#include <Adafruit_PN532.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// --- PN532 SPI configuration ---
#define PN532_SCK  (18)
#define PN532_MOSI (23)
#define PN532_SS   (5)
#define PN532_MISO (19)
Adafruit_PN532 nfc(PN532_SS);

// --- Motor Driver Pins ---
#define IN1A 26
#define IN2A 27
#define ENA 33
#define IN1B 14
#define IN2B 12
#define ENB 13

// --- Push Button Reset ---
#define RESET_BUTTON_PIN 4

// --- WiFi credentials ---
#define WIFI_SSID "xyz"
#define WIFI_PASSWORD "****"

// --- Firestore Query URL ---
#define FIRESTORE_QUERY_URL "https://firestore.googleapis.com/v1/projects/foodapp231/databases/(default)/documents:runQuery"

// --- Time settings for HTTPS ---
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800; // IST
const int daylightOffset_sec = 0;

// --- RFID UIDs ---
byte stopUID[]     = {0x05, 0xE0, 0xA1, 0x02};
byte pauseUIDs[][4] = {
  {0x15, 0x87, 0x87, 0x02}, // Pause 1 - Table 1
  {0xB5, 0x51, 0x46, 0x01}, // Pause 2 - Table 2
  {0xB5, 0x22, 0x0C, 0x01}, // Pause 3 - Table 3
  {0xB9, 0x2B, 0x57, 0x16}, // Pause 4 - Table 4
  {0x85, 0x38, 0xFF, 0x03}  // Pause 5 - Table 5
};

bool motorRunning = true;
bool permanentlyStopped = false;
int activeTableIndex = -1; // No table active initially
String lastOrderId = "";

void setup() {
  Serial.begin(115200);

  pinMode(IN1A, OUTPUT); pinMode(IN2A, OUTPUT); pinMode(ENA, OUTPUT);
  pinMode(IN1B, OUTPUT); pinMode(IN2B, OUTPUT); pinMode(ENB, OUTPUT);
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  stopMotors();

  // Init PN532
  nfc.begin();
  if (!nfc.getFirmwareVersion()) {
    Serial.println("PN532 init failed!");
    while (1);
  }
  nfc.SAMConfig();

  // Connect WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected");

  // Sync Time
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.print("Syncing time");
  while (time(nullptr) < 1000000000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nTime synchronized");

  Serial.println("System Ready. Place tag...");
}

void loop() {
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    Serial.println("Reset button pressed! Restarting ESP32...");
    delay(200);
    ESP.restart();
  }

  if (WiFi.status() == WL_CONNECTED) {
    fetchLatestTableNumber();
  }

  // Motor control based on state
  if (permanentlyStopped || !motorRunning) {
    stopMotors();
  } else {
    runMotors();
  }

  // RFID scanning
  uint8_t uid[7];
  uint8_t uidLength;
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 100)) {
    Serial.print("Detected UID: ");
    for (uint8_t i = 0; i < uidLength; i++) {
      Serial.print(uid[i], HEX); Serial.print(" ");
    }
    Serial.println();

    if (uidLength == 4) {
      // Check STOP tag first
      if (memcmp(uid, stopUID, 4) == 0) {
        Serial.println("STOP tag detected. Motors permanently stopped.");
        permanentlyStopped = true;
        motorRunning = false;
      }
      else if (!permanentlyStopped && activeTableIndex >= 0) {
        // Check if correct pause card detected
        if (memcmp(uid, pauseUIDs[activeTableIndex], 4) == 0) {
          Serial.print("Correct card for active Table detected (Table ");
          Serial.print(activeTableIndex + 1);
          Serial.println(")! Pausing motors...");
          motorRunning = false;
          stopMotors();
          delay(30000); // Pause 30 seconds
          if (!permanentlyStopped) {
            Serial.println("Resuming motors...");
            motorRunning = true;
          }
        } else {
          Serial.println("Wrong card for current active Table. Ignoring...");
        }
      }
    }
    delay(10); // Debounce
  }

  delay(10); // Small loop delay
}

void fetchLatestTableNumber() {
  HTTPClient http;
  http.begin(FIRESTORE_QUERY_URL);
  http.addHeader("Content-Type", "application/json");

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

    DynamicJsonDocument doc(2048);
    deserializeJson(doc, response);

    if (doc.size() > 0 && doc[0]["document"].containsKey("fields")) {
      String orderId = doc[0]["document"]["fields"]["orderId"]["stringValue"];
      String customerName = doc[0]["document"]["fields"]["customerName"]["stringValue"];

      if (orderId != lastOrderId) {
        lastOrderId = orderId;
        Serial.print(" New Order fetched: ");
        Serial.println(customerName);

        // Update active table index based on customerName "Table 1" etc.
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

  http.end();
}

void runMotors() {
  digitalWrite(IN1A, LOW); digitalWrite(IN2A, HIGH); analogWrite(ENA,127);
  digitalWrite(IN1B, LOW); digitalWrite(IN2B, HIGH); analogWrite(ENB,127);
}

void stopMotors() {
  digitalWrite(IN1A, LOW); digitalWrite(IN2A, LOW); analogWrite(ENA, 0);
  digitalWrite(IN1B, LOW); digitalWrite(IN2B, LOW); analogWrite(ENB, 0);
}
