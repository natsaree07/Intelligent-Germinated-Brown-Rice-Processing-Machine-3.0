#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>
#include "secrets.h"

// MAC address ของบอร์ดหลัก ดูได้จาก Serial Monitor ของบอร์ดหลัก
uint8_t MAIN_MAC[6] = {0xD0, 0xEF, 0x76, 0x44, 0xAD, 0x9C};

constexpr uint8_t SHT45_ADDRESS = 0x44;
constexpr uint32_t SENSOR_MAGIC = 0x53485445UL;
constexpr uint32_t SAMPLE_INTERVAL_MS = 2000UL;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 5000UL;

struct SensorPacket {
  uint32_t magic;
  uint32_t bootId;
  uint32_t sequence;
  float temperature;
  float humidity;
};

uint32_t bootId = 0;
uint32_t sequenceNumber = 0;
bool espNowInitialized = false;

uint8_t shtCRC(const uint8_t* data, size_t length) {
  uint8_t crc = 0xFF;

  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];

    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80)
                ? static_cast<uint8_t>((crc << 1) ^ 0x31)
                : static_cast<uint8_t>(crc << 1);
    }
  }

  return crc;
}

bool readSHT45(float& temperature, float& humidity) {
  // High repeatability measurement without clock stretching
  Wire.beginTransmission(SHT45_ADDRESS);
  Wire.write(0xFD);

  if (Wire.endTransmission() != 0) {
    return false;
  }

  delay(10);

  uint8_t received = Wire.requestFrom(SHT45_ADDRESS, static_cast<uint8_t>(6));

  if (received != 6 || Wire.available() < 6) {
    while (Wire.available()) {
      Wire.read();
    }

    return false;
  }

  uint8_t data[6];

  for (uint8_t i = 0; i < 6; i++) {
    data[i] = static_cast<uint8_t>(Wire.read());
  }

  if (shtCRC(data, 2) != data[2] ||
      shtCRC(data + 3, 2) != data[5]) {
    return false;
  }

  uint16_t rawTemperature =
      (static_cast<uint16_t>(data[0]) << 8) | data[1];
  uint16_t rawHumidity =
      (static_cast<uint16_t>(data[3]) << 8) | data[4];

  temperature =
      -45.0f + (175.0f * static_cast<float>(rawTemperature) / 65535.0f);
  humidity =
      -6.0f + (125.0f * static_cast<float>(rawHumidity) / 65535.0f);
  humidity = constrain(humidity, 0.0f, 100.0f);

  return isfinite(temperature) &&
         isfinite(humidity) &&
         temperature > -20.0f &&
         temperature < 90.0f;
}

bool ensureESPNowReady() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (!espNowInitialized) {
    esp_err_t initResult = esp_now_init();

    if (initResult != ESP_OK) {
      Serial.printf("ESP-NOW init failed: %d\n", initResult);
      return false;
    }

    espNowInitialized = true;
    Serial.println("ESP-NOW initialized");
  }

  if (!esp_now_is_peer_exist(MAIN_MAC)) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, MAIN_MAC, sizeof(MAIN_MAC));
    peer.channel = 0;  // ใช้ channel ปัจจุบันของ Wi-Fi
    peer.encrypt = false;

    esp_err_t peerResult = esp_now_add_peer(&peer);

    if (peerResult != ESP_OK) {
      Serial.printf("ESP-NOW peer add failed: %d\n", peerResult);
      return false;
    }

    Serial.println("ESP-NOW peer added");
  }

  return true;
}

void printWiFiInformation() {
  Serial.println();
  Serial.println("Wi-Fi connected");
  Serial.print("Sender IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("Sender MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.printf("Sender channel: %d\n", WiFi.channel());
  Serial.printf("Wi-Fi RSSI: %d dBm\n", WiFi.RSSI());
}

void setup() {
  Serial.begin(115200);
  delay(300);

  bootId = esp_random();

  Wire.begin(21, 22);
  Wire.setClock(100000);
  Wire.setTimeOut(50);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Joining 2.4 GHz Wi-Fi");

  uint32_t connectStarted = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - connectStarted < 20000UL) {
    delay(250);
    Serial.print('.');
  }

  if (WiFi.status() == WL_CONNECTED) {
    printWiFiInformation();
    ensureESPNowReady();
  } else {
    // ไม่ออกจากการทำงานถาวร: loop() จะลองเชื่อมต่อใหม่ต่อไป
    Serial.println();
    Serial.println("Wi-Fi unavailable; will retry in loop");
  }
}

void loop() {
  static uint32_t lastSample = 0;
  static uint32_t lastWiFiRetry = 0;
  static bool previouslyConnected = false;

  uint32_t now = millis();
  bool connected = WiFi.status() == WL_CONNECTED;

  if (!connected) {
    if (previouslyConnected) {
      Serial.println("Wi-Fi disconnected; sensor transmission paused");
      previouslyConnected = false;
    }

    if (now - lastWiFiRetry >= WIFI_RETRY_INTERVAL_MS) {
      lastWiFiRetry = now;
      Serial.println("Retrying Wi-Fi...");
      WiFi.reconnect();
    }

    delay(10);
    return;
  }

  if (!previouslyConnected) {
    previouslyConnected = true;
    printWiFiInformation();
  }

  // สำคัญ: หากตอนเปิดเครื่อง Wi-Fi ยังไม่ติด ESP-NOW จะถูกเริ่มตรงนี้
  if (!ensureESPNowReady()) {
    delay(100);
    return;
  }

  if (now - lastSample < SAMPLE_INTERVAL_MS) {
    delay(5);
    return;
  }

  lastSample = now;

  float temperature = NAN;
  float humidity = NAN;

  // อ่านซ้ำหนึ่งครั้งเพื่อทนต่อ CRC/noise ชั่วคราวบนสาย I2C
  if (!readSHT45(temperature, humidity)) {
    delay(20);

    if (!readSHT45(temperature, humidity)) {
      Serial.println("SHT45 CRC/read failure: packet NOT sent");
      return;
    }

    Serial.println("SHT45 recovered on retry");
  }

  SensorPacket packet = {
      SENSOR_MAGIC,
      bootId,
      ++sequenceNumber,
      temperature,
      humidity
  };

  esp_err_t sendResult = esp_now_send(
      MAIN_MAC,
      reinterpret_cast<const uint8_t*>(&packet),
      sizeof(packet));

  Serial.printf(
      "SHT45 %.2f C / %.2f %%RH, seq=%lu, send=%s\n",
      temperature,
      humidity,
      static_cast<unsigned long>(sequenceNumber),
      sendResult == ESP_OK ? "queued" : "FAILED");
}
