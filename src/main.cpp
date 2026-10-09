#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_arduino_version.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LiquidCrystal_I2C.h>
#include "secrets.h"

struct SensorLogBuffer;  // needed by Arduino-generated function prototypes
// ============================================================
// WIFI
// ============================================================

// ============================================================
// MQTT
// ============================================================

#define MQTT_PORT 1883

// ============================================================
// MQTT TOPICS
// ============================================================

const char* TOPIC_STATUS = "ricedryer/status";
const char* TOPIC_STAGE = "ricedryer/stage";
const char* TOPIC_WATER_TEMP = "ricedryer/water_temp";
const char* TOPIC_TEMP = "ricedryer/temperature";
const char* TOPIC_HUMIDITY = "ricedryer/humidity";

const char* TOPIC_HEATER = "ricedryer/heater";
const char* TOPIC_MISTMAKER = "ricedryer/mistmaker";
const char* TOPIC_FAN = "ricedryer/fan";
const char* TOPIC_BLOWER = "ricedryer/blower";
const char* TOPIC_MOTOR = "ricedryer/motor";

const char* TOPIC_SOLENOID1 = "ricedryer/solenoid1";
const char* TOPIC_SOLENOID2 = "ricedryer/solenoid2";

const char* TOPIC_PROCESS = "ricedryer/process";
const char* TOPIC_COMMAND = "ricedryer/command";
const char* TOPIC_PAUSED = "ricedryer/paused";

const char* TOPIC_STAGE1_TIME = "ricedryer/stage1_time";
const char* TOPIC_STAGE2_TIME = "ricedryer/stage2_time";
const char* TOPIC_STAGE3_TIME = "ricedryer/stage3_time";
const char* TOPIC_TOTAL_TIME = "ricedryer/total_time";

// ============================================================
// HOME ASSISTANT MQTT DISCOVERY
// ============================================================

const char* HA_PREFIX = "homeassistant";

const char* DEVICE_NAME = "Rice Dryer";
const char* DEVICE_ID = "rice_dryer_esp32";

const char* MQTT_AVAILABILITY = "ricedryer/status";

// ============================================================
// LCD
// ============================================================

LiquidCrystal_I2C lcd(0x27, 16, 2);

// ============================================================
// OUTPUT DEVICES
// ============================================================

#define LED_RED 13
#define LED_YELLOW 14
#define LED_GREEN 32

#define SOLENOID_IN 27
#define SOLENOID_DRAIN 25

#define FAN_DC 26
#define BLOWER_RELAY 12
#define MISTMAKER 33
#define MOTOR 19
#define HEATER 18
#define BUZZER 4

#define BUTTON_PIN 16
#define ONE_WIRE_BUS 15
#define LCD_I2C_SDA_PIN 21
#define LCD_I2C_SCL_PIN 22

// ต้องกดปุ่มค้างตามเวลานี้จึงยอมรับคำสั่ง Start
// ช่วยป้องกันสัญญาณรบกวนและการกดเด้ง
const unsigned long BUTTON_HOLD_TIME = 1000UL;

// ขณะระบบทำงาน กดปุ่มเดิมค้าง 1.5 วินาทีเพื่อหยุดกระบวนการ
const unsigned long STOP_BUTTON_HOLD_TIME = 1500UL;

// ============================================================
// SENSORS
// ============================================================

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);

// ============================================================
// WIFI / MQTT
// ============================================================

WiFiClient espClient;
PubSubClient mqttClient(espClient);

unsigned long lastReconnectAttempt = 0;

const unsigned long RECONNECT_INTERVAL = 5000;

// ============================================================
// PROCESS PARAMETERS
// ============================================================

// เปลี่ยนเป็น 0 เมื่อต้องการกลับไปใช้ค่าการทำงานจริง
#define PROCESS_TEST_MODE 1

#if PROCESS_TEST_MODE

// ============================================================
// TEST MODE PARAMETERS
// แต่ละ Stage ใช้เวลาสั้นลงเพื่อทดสอบอุปกรณ์และ Home Assistant
// ============================================================

// Stage 1

const float WATER_TEMP_TARGET = 40.0;

// Stage 2

const float STAGE2_TEMP_TARGET = 40.0;
const float STAGE2_HUMID_TARGET = 85.0;

// Stage 3

const float STAGE3_TEMP_TARGET = 40.0;
const float STAGE3_RH_TARGET = 70.0;

// Safety

const float STAGE3_MAX_TEMP = 45.0;

// Time

const unsigned long STAGE1_DURATION = 2UL * 60UL * 1000UL;

const unsigned long STAGE2_DURATION = 2UL * 60UL * 1000UL;

const unsigned long STAGE3_DURATION = 2UL * 60UL * 1000UL;

const unsigned long FILL_WATER_TIME = 5000;
const unsigned long DRAIN_WATER_TIME = 5000;

#else

// ============================================================
// NORMAL MODE PARAMETERS
// ============================================================

// Stage 1

const float WATER_TEMP_TARGET = 30.0;

// Stage 2

const float STAGE2_TEMP_TARGET = 30.0;
const float STAGE2_HUMID_TARGET = 80.0;

// Stage 3

const float STAGE3_TEMP_TARGET = 50.0;
const float STAGE3_RH_TARGET = 14.0;

// Safety

const float STAGE3_MAX_TEMP = 55.0;

// ============================================================
// TIME
// ============================================================

const unsigned long STAGE1_DURATION = 10UL * 60UL * 1000UL;

const unsigned long STAGE2_DURATION = 10UL * 60UL * 1000UL;

// Stage 3 มีเวลาสูงสุดเพื่อป้องกันเครื่องทำงานไม่สิ้นสุด

const unsigned long STAGE3_DURATION = 8UL * 60UL * 60UL * 1000UL;

const unsigned long FILL_WATER_TIME = 15000;
const unsigned long DRAIN_WATER_TIME = 10000;

#endif

// ============================================================
// HYSTERESIS PARAMETERS (PATCH)
// กันอุปกรณ์สลับ ON/OFF ถี่เกินไปเวลาค่าเซนเซอร์แกว่งเบาๆ
// รอบๆ จุด target ปรับตัวเลขได้ตามความไวจริงของระบบ
// ============================================================

const float WATER_TEMP_HYSTERESIS = 0.5;    // °C
const float STAGE2_TEMP_HYSTERESIS = 0.5;   // °C
const float STAGE2_HUMID_HYSTERESIS = 2.0;  // %RH

// ============================================================
// BLOWER COOL-DOWN (PATCH)
// หลัง Heater ดับ ให้พัดลม Blower เดินต่ออีกระยะหนึ่ง เพื่อเป่าระบายความร้อน
// สะสมที่แท่ง Heater ไม่ให้ความร้อนย้อนเข้าตัวพัดลม
// - Heater กลับมาติดก่อนครบเวลา: ยกเลิกการนับ Blower เดินต่อตามปกติ
// - ครบเวลาโดย Heater ยังดับ: ปิด Blower
// ทำงานแบบ non-blocking (ไม่หยุดรอ) ลูปควบคุมยังอ่านเซนเซอร์ตามปกติ
// ============================================================

#if PROCESS_TEST_MODE
const unsigned long BLOWER_COOLDOWN_MS = 30000UL;  // โหมดทดสอบ 30 วินาที
#else
const unsigned long BLOWER_COOLDOWN_MS = 60000UL;  // ใช้งานจริง 60 วินาที (ปรับตามที่วัดได้)
#endif

// true  = ตอน Pause / Stop ให้ Heater ดับทันที แต่ Blower เดิน cool-down ต่อจนครบเวลา
// false = ตอน Pause / Stop ให้ Blower ดับทันทีพร้อม Heater (หยุดทั้งหมดสนิท)
const bool BLOWER_COOLDOWN_ON_PAUSE_STOP = true;

bool blowerCooling = false;
unsigned long blowerCooldownStart = 0;

// ระยะเวลาแสดงแต่ละหน้า LCD ก่อนสลับหน้า
// 6000 ms = 6 วินาที
const unsigned long LCD_PAGE_DURATION = 6000UL;

// เริ่มต้น LCD ใหม่เป็นระยะเพื่อกู้หน้าจอจากสัญญาณรบกวน I2C
const unsigned long LCD_RECOVERY_INTERVAL = 60000UL;

// เวลารอให้ไฟเลี้ยงและสัญญาณนิ่งหลังสับ Solenoid/Relay
// ก่อนอ่าน Sensor หรือเปิดอุปกรณ์กำลังตัวถัดไป
const unsigned long SOLENOID_SETTLE_TIME = 3000UL;

// ============================================================
// PROCESS TIMERS
// ============================================================

unsigned long processStartMillis = 0;
unsigned long stageStartMillis = 0;

unsigned long stage1ElapsedSeconds = 0;
unsigned long stage2ElapsedSeconds = 0;
unsigned long stage3ElapsedSeconds = 0;
unsigned long totalElapsedSeconds = 0;

unsigned long lastTimerPublishMillis = 0;
unsigned long lastLCDSwitchMillis = 0;
unsigned long lastIdleSensorPublishMillis = 0;
unsigned long lastLCDRecoveryMillis = 0;

String lastLCDLine1 = "";
String lastLCDLine2 = "";
String currentErrorMessage = "";

bool processTimerRunning = false;
bool stageTimerRunning = false;
bool lcdShowTimerPage = true;
bool processActive = false;
bool remoteStartRequested = false;
bool processStopRequested = false;
bool processAborted = false;

bool stopButtonHoldTracking = false;
bool stopButtonTriggered = false;
unsigned long stopButtonHoldStart = 0;

bool stage1_heaterState = false;
bool stage2_heaterState = false;
bool stage2_mistState = false;

// ==========================================================
// PAUSE / RESUME (สั่งงานผ่าน Home Assistant เท่านั้น)
// ==========================================================

bool processPaused = false;        // true ระหว่างที่หยุดพักชั่วคราว (หยุดอุปกรณ์ทุกตัวสนิท)
bool pauseFlagRequested = false;   // ตั้งจาก mqttCallback เมื่อได้รับคำสั่ง PAUSE
bool resumeFlagRequested = false;  // ตั้งจาก mqttCallback เมื่อได้รับคำสั่ง RESUME

unsigned long pauseStartMillis = 0;  // millis() ตอนเริ่ม Pause
unsigned long totalPausedMillis = 0;

void compensatePause(unsigned long& start, unsigned long& pauseBaseline) {
  if (totalPausedMillis != pauseBaseline) {
    start += (totalPausedMillis - pauseBaseline);

    pauseBaseline = totalPausedMillis;
  }
}

uint8_t currentStage = 0;

float latestWaterTemp = NAN;
float latestAirTemp = NAN;
float latestHumidity = NAN;

void updateProcessTimers();
void updateProcessLCD();
void recoverLCDIfNeeded();
void mqttCallback(char* topic, byte* payload, unsigned int length);
bool handleProcessStop();
void enterPause();
void exitPause();
bool handleProcessPause();
void updateBlowerCooldown();
void forceBlowerOff();

unsigned long lastWaterSensorOK = 0;
unsigned long lastAirSensorOK = 0;

const unsigned long SENSOR_TIMEOUT = 5000;

// SHT45 now lives on a separate ESP32; data arrives by ESP-NOW.
// ESP-NOW runs on the SAME 2.4 GHz channel as the main board's Wi-Fi router.
const unsigned long SHT45_FAULT_STOP_MS = 10000UL;
const unsigned long ESP_NOW_STALE_MS = 5000UL;
const uint32_t SENSOR_MAGIC = 0x53485445UL;
struct SensorPacket {
  uint32_t magic;
  uint32_t bootId;
  uint32_t sequence;
  float temperature;
  float humidity;
};
portMUX_TYPE sensorMux = portMUX_INITIALIZER_UNLOCKED;
SensorPacket pendingSensor = {};
volatile bool sensorPending = false;
bool sensorSeen = false;
uint32_t lastSensorBoot = 0, lastSensorSeq = 0;
unsigned long lastSensorPacketMs = 0;
bool ambientFaultActive = false;
unsigned long ambientFaultSince = 0;
bool ambientBufferResetNeeded = false;
void onSensorPacket(const uint8_t *data, int length) {
  if (length != sizeof(SensorPacket)) return;
  SensorPacket temp;
  memcpy(&temp, data, sizeof(temp));
  if (temp.magic != SENSOR_MAGIC || !isfinite(temp.temperature) ||
      !isfinite(temp.humidity) || temp.temperature <= -20.0f ||
      temp.temperature >= 90.0f || temp.humidity < 0.0f ||
      temp.humidity > 100.0f) return;
  portENTER_CRITICAL(&sensorMux);
  pendingSensor = temp;
  sensorPending = true;
  portEXIT_CRITICAL(&sensorMux);
}
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void sensorReceiveCallback(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  onSensorPacket(data, len);
}
#else
void sensorReceiveCallback(const uint8_t *mac, const uint8_t *data, int len) {
  onSensorPacket(data, len);
}
#endif
void serviceSensorESPNow() {
  SensorPacket p;
  bool pending = false;
  portENTER_CRITICAL(&sensorMux);
  if (sensorPending) {
    p = pendingSensor;
    sensorPending = false;
    pending = true;
  }
  portEXIT_CRITICAL(&sensorMux);
  if (pending) {
    if (!sensorSeen || p.bootId != lastSensorBoot ||
        (int32_t)(p.sequence - lastSensorSeq) > 0) {
      lastSensorBoot = p.bootId;
      lastSensorSeq = p.sequence;
      sensorSeen = true;
      lastSensorPacketMs = millis();
      if (ambientFaultActive) ambientBufferResetNeeded = true;
      ambientFaultActive = false;
      latestAirTemp = p.temperature;
      latestHumidity = p.humidity;
      lastAirSensorOK = lastSensorPacketMs;
    }
  }
  // Fail closed even during waits: stop critical outputs once the sample expires.
  if (processActive && !processPaused && (currentStage == 2 || currentStage == 3) &&
      (!sensorSeen || millis() - lastSensorPacketMs > ESP_NOW_STALE_MS)) {
    digitalWrite(HEATER, LOW);
    digitalWrite(MISTMAKER, LOW);
    if (!ambientFaultActive) {
      ambientFaultActive = true;
      ambientFaultSince = millis();
    }
    if (millis() - ambientFaultSince >= SHT45_FAULT_STOP_MS) {
      processStopRequested = true;
    }
  }
}
void setupESPNowReceiver() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW INIT ERROR: SHT45 unavailable");
    return;
  }
  esp_now_register_recv_cb(sensorReceiveCallback);
  Serial.print("ESP-NOW receiver MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.printf("ESP-NOW channel: %d\n", WiFi.channel());
}

bool waterSensorTimeout() {
  if (lastWaterSensorOK == 0) {
    return true;
  }

  return millis() - lastWaterSensorOK > SENSOR_TIMEOUT;
}

bool airSensorTimeout() {
  if (lastAirSensorOK == 0) {
    return true;
  }

  return millis() - lastAirSensorOK > SENSOR_TIMEOUT;
}

// เรียกทุกครั้งที่เริ่มกระบวนการใหม่ เพื่อไม่ให้ timeout ค้างจากรอบก่อน
void resetSensorTimeouts() {
  lastWaterSensorOK = millis();
  lastAirSensorOK = millis();
}

float readWaterTemp() {
  if (ds18b20.getDeviceCount() == 0) {
    return NAN;
  }

  const int MAX_ATTEMPTS = 2;

  for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
    ds18b20.requestTemperatures();
    float t = ds18b20.getTempCByIndex(0);

    if (t == DEVICE_DISCONNECTED_C) {
      continue;
    }

    // ค่า default ตอน power-on-reset ของ DS18B20 มักเกิดเวลามี noise
    if (fabs(t - 85.0) < 0.05) {
      continue;
    }

    // ช่วงอุณหภูมิที่เป็นไปได้จริงของถังน้ำในระบบนี้
    if (t < -20.0 || t > 100.0) {
      continue;
    }

    latestWaterTemp = t;

    // PATCH: sensor timeout - นับว่าอ่านสำเร็จรอบนี้
    lastWaterSensorOK = millis();

    return t;
  }

  Serial.println("DS18B20: reading rejected as noise/disconnected");

  return NAN;
}

bool readAmbient(float& temp, float& humid) {
  serviceSensorESPNow();
  bool valid = sensorSeen && (millis() - lastSensorPacketMs <= ESP_NOW_STALE_MS);
  if (valid) {
    temp = latestAirTemp;
    humid = latestHumidity;
    return true;
  }
  temp = humid = NAN;
  latestAirTemp = latestHumidity = NAN;
  ambientBufferResetNeeded = true;
  if (!ambientFaultActive) {
    ambientFaultActive = true;
    ambientFaultSince = millis();
  }
  return false;
}

const unsigned long LOG_INTERVAL_MS = 2000UL;  // บันทึกทุก 2 วินาที
const int LOG_BUFFER_SIZE = 5;                 // เก็บย้อนหลัง 5 ค่า = 10 วินาที

const float WATER_TEMP_MAX_JUMP = 3.0;  // °C ต่อรอบบันทึก
const float AIR_TEMP_MAX_JUMP = 3.0;    // °C ต่อรอบบันทึก
const float HUMID_MAX_JUMP = 8.0;       // %RH ต่อรอบบันทึก

struct SensorLogBuffer {
  float values[LOG_BUFFER_SIZE];
  int count = 0;
  int head = 0;
  float lastGood = NAN;
};

SensorLogBuffer waterTempLog;
SensorLogBuffer airTempLog;
SensorLogBuffer humidLog;

unsigned long lastLogMillis = 0;

void addSensorSample(SensorLogBuffer& buf, float newValue, float maxJump) {
  if (isnan(newValue)) {
    return;
  }

  if (!isnan(buf.lastGood) && fabs(newValue - buf.lastGood) > maxJump) {
    Serial.print("Sensor jump rejected: ");
    Serial.print(buf.lastGood);
    Serial.print(" -> ");
    Serial.println(newValue);

    return;
  }

  buf.values[buf.head] = newValue;
  buf.head = (buf.head + 1) % LOG_BUFFER_SIZE;

  if (buf.count < LOG_BUFFER_SIZE) {
    buf.count++;
  }

  buf.lastGood = newValue;
}

float getBufferAverage(const SensorLogBuffer& buf) {
  if (buf.count == 0) {
    return NAN;
  }

  float sum = 0;

  for (int i = 0; i < buf.count; i++) {
    sum += buf.values[i];
  }

  return sum / buf.count;
}

// เรียกฟังก์ชันนี้พร้อมค่าดิบที่อ่านได้ในรอบนั้น (ค่าที่ไม่เกี่ยวข้อง
// ใส่ NAN ได้เลย เช่น Stage 1 มีแค่ waterTemp ก็ใส่ airTemp/humid เป็น NAN)
// จะบันทึกจริงเฉพาะเมื่อครบ 2 วินาทีตามที่กำหนด ไม่ต้องกังวลเรื่องเรียกถี่
void maybeAddToLogBuffers(float waterTemp, float airTemp, float humid) {
  if (ambientBufferResetNeeded) {
    airTempLog = SensorLogBuffer();
    humidLog = SensorLogBuffer();
    ambientBufferResetNeeded = false;
  }

  unsigned long now = millis();

  if (now - lastLogMillis < LOG_INTERVAL_MS) {
    return;
  }

  lastLogMillis = now;

  addSensorSample(waterTempLog, waterTemp, WATER_TEMP_MAX_JUMP);
  addSensorSample(airTempLog, airTemp, AIR_TEMP_MAX_JUMP);
  addSensorSample(humidLog, humid, HUMID_MAX_JUMP);

  Serial.print("[LOG] Water=");
  Serial.print(getBufferAverage(waterTempLog));
  Serial.print("C  Air=");
  Serial.print(getBufferAverage(airTempLog));
  Serial.print("C  RH=");
  Serial.println(getBufferAverage(humidLog));
}

// เรียกทุกครั้งที่เริ่มกระบวนการใหม่ (ทำใน loop() ตอน startRequested)
void resetSensorLogBuffers() {
  waterTempLog = SensorLogBuffer();
  airTempLog = SensorLogBuffer();
  humidLog = SensorLogBuffer();

  lastLogMillis = 0;
}

// ============================================================
// BUZZER
// ============================================================

// true = เปิดเสียง, false = ปิดเสียงทั้งหมด
const bool BUZZER_ENABLED = true;

void beep2() {
  if (!BUZZER_ENABLED) {
    return;
  }

  // ใช้ความถี่สูงขึ้นและดังสองจังหวะเพื่อให้ได้ยินชัดกว่าเดิม
  // tone() เปลี่ยนความถี่และระยะเวลา แต่ไม่ได้เพิ่มแรงดัน GPIO
  for (int i = 0; i < 2; i++) {
    tone(BUZZER, 2500);
    delay(180);

    noTone(BUZZER);
    delay(100);
  }
}

void beepDone() {
  if (!BUZZER_ENABLED) {
    return;
  }

  // เสียงสามจังหวะเมื่อกระบวนการเสร็จ
  for (int i = 0; i < 3; i++) {
    tone(BUZZER, 2800);
    delay(250);

    noTone(BUZZER);
    delay(120);
  }
}

// ============================================================
// LCD
// ============================================================

void lcdShow(String line1, String line2) {
  // จำกัดข้อความไม่ให้เกิน 16 ตัวอักษร
  if (line1.length() > 16) {
    line1 = line1.substring(0, 16);
  }

  if (line2.length() > 16) {
    line2 = line2.substring(0, 16);
  }

  // เติมช่องว่างเพื่อเขียนทับข้อความเดิมโดยไม่ต้อง lcd.clear()
  while (line1.length() < 16) {
    line1 += " ";
  }

  while (line2.length() < 16) {
    line2 += " ";
  }

  lastLCDLine1 = line1;
  lastLCDLine2 = line2;

  lcd.setCursor(0, 0);
  lcd.print(line1);

  lcd.setCursor(0, 1);
  lcd.print(line2);
}

void recoverLCDIfNeeded() {
  unsigned long now = millis();

  if (now - lastLCDRecoveryMillis < LCD_RECOVERY_INTERVAL) {
    return;
  }

  lastLCDRecoveryMillis = now;

  // เริ่มต้นจอใหม่ แต่ไม่รีเซต ESP32 และไม่หยุดกระบวนการ
  lcd.init();
  lcd.backlight();
  Wire.setClock(40000);

  lcd.setCursor(0, 0);
  lcd.print(lastLCDLine1);

  lcd.setCursor(0, 1);
  lcd.print(lastLCDLine2);

  Serial.println("LCD watchdog refresh");
}

// ============================================================
// MQTT CLIENT ID
// ============================================================

String createMQTTClientID() {
  uint64_t chipid = ESP.getEfuseMac();

  char clientId[40];

  snprintf(clientId, sizeof(clientId), "RiceDryer_%04X%08X", (uint16_t)(chipid >> 32),
           (uint32_t)chipid);

  return String(clientId);
}

// ============================================================
// WIFI
// ============================================================

void connectWiFi() {
  Serial.println();
  Serial.println("================================");
  Serial.println("Connecting to WiFi...");
  Serial.print("SSID: ");
  Serial.println(WIFI_SSID);
  Serial.println("================================");

  lcdShow("Connecting WiFi", "Please wait...");

  WiFi.mode(WIFI_STA);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempt = 0;

  while (WiFi.status() != WL_CONNECTED && attempt < 30) {
    Serial.print(".");

    lcdShow("Connecting WiFi", "Attempt " + String(attempt + 1));

    delay(1000);

    attempt++;
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi CONNECTED!");

    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    Serial.print("RSSI: ");
    Serial.println(WiFi.RSSI());

    lcdShow("WiFi Connected", WiFi.localIP().toString());

    delay(1500);

  } else {
    Serial.println("WiFi CONNECTION FAILED!");

    lcdShow("WiFi Failed", "Offline Mode");

    delay(1500);
  }
}

// ============================================================
// HOME ASSISTANT DISCOVERY
// ============================================================

bool publishDiscovery(const char* component, const char* objectId, const char* name,
                      const char* stateTopic, const char* unit, const char* deviceClass,
                      const char* stateClass, const char* payloadOn, const char* payloadOff) {
  if (!mqttClient.connected()) {
    Serial.println("Discovery FAILED: MQTT not connected");

    return false;
  }

  // ----------------------------------------------------------
  // Discovery topic
  // ----------------------------------------------------------

  String topic = String(HA_PREFIX) + "/" + component + "/" + DEVICE_ID + "/" + objectId + "/config";

  // ----------------------------------------------------------
  // Payload
  // ----------------------------------------------------------

  String payload = "{";

  payload += "\"name\":\"";
  payload += name;
  payload += "\",";

  payload += "\"unique_id\":\"";
  payload += DEVICE_ID;
  payload += "_";
  payload += objectId;
  payload += "\",";

  payload += "\"state_topic\":\"";
  payload += stateTopic;
  payload += "\",";

  payload += "\"availability_topic\":\"";
  payload += MQTT_AVAILABILITY;
  payload += "\",";

  payload += "\"payload_available\":\"connected\",";
  payload += "\"payload_not_available\":\"offline\",";

  // Unit

  if (unit != nullptr && strlen(unit) > 0) {
    payload += "\"unit_of_measurement\":\"";
    payload += unit;
    payload += "\",";
  }

  // Device class

  if (deviceClass != nullptr && strlen(deviceClass) > 0) {
    payload += "\"device_class\":\"";
    payload += deviceClass;
    payload += "\",";
  }

  // State class

  if (stateClass != nullptr && strlen(stateClass) > 0) {
    payload += "\"state_class\":\"";
    payload += stateClass;
    payload += "\",";
  }

  // Binary Sensor ON/OFF

  if (payloadOn != nullptr && strlen(payloadOn) > 0) {
    payload += "\"payload_on\":\"";
    payload += payloadOn;
    payload += "\",";
  }

  if (payloadOff != nullptr && strlen(payloadOff) > 0) {
    payload += "\"payload_off\":\"";
    payload += payloadOff;
    payload += "\",";
  }

  // ----------------------------------------------------------
  // DEVICE
  // ----------------------------------------------------------

  payload += "\"device\":{";

  payload += "\"identifiers\":[\"";
  payload += DEVICE_ID;
  payload += "\"],";

  payload += "\"name\":\"";
  payload += DEVICE_NAME;
  payload += "\",";

  payload += "\"manufacturer\":\"ESP32\",";

  payload += "\"model\":\"Rice Growing + Drying System\"";

  payload += "}";

  payload += "}";

  // ----------------------------------------------------------
  // Serial Debug
  // ----------------------------------------------------------

  Serial.println();
  Serial.println("--------------------------------");

  Serial.print("HA Discovery: ");
  Serial.println(name);

  Serial.print("Topic: ");
  Serial.println(topic);

  Serial.print("Payload size: ");
  Serial.println(payload.length());

  Serial.print("Payload: ");
  Serial.println(payload);

  // ----------------------------------------------------------
  // Publish
  // ----------------------------------------------------------

  bool result = mqttClient.publish(topic.c_str(), payload.c_str(), true);

  if (result) {
    Serial.println("Result: SUCCESS");

  } else {
    Serial.println("Result: FAILED");

    Serial.print("MQTT state: ");

    Serial.println(mqttClient.state());
  }

  mqttClient.loop();

  // สำคัญมาก
  delay(250);

  return result;
}

// ============================================================
// PUBLISH HOME ASSISTANT DISCOVERY
// ============================================================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";

  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  message.trim();
  message.toUpperCase();

  Serial.print("MQTT command: ");
  Serial.print(topic);
  Serial.print(" = ");
  Serial.println(message);

  if (String(topic) == TOPIC_COMMAND && message == "START") {
    if (!processActive) {
      // ให้ loop() เป็นผู้เริ่มกระบวนการ
      // ห้ามเรียก Stage โดยตรงจาก MQTT callback
      remoteStartRequested = true;

      Serial.println("REMOTE START REQUEST ACCEPTED");

    } else {
      Serial.println("REMOTE START IGNORED - PROCESS RUNNING");
    }
  }

  if (String(topic) == TOPIC_COMMAND && message == "STOP") {
    if (processActive) {
      // ตั้งธงไว้ก่อน แล้วให้ลูปกระบวนการเป็นผู้ปิดอุปกรณ์
      processStopRequested = true;

      Serial.println("REMOTE STOP REQUEST ACCEPTED");

    } else {
      Serial.println("REMOTE STOP IGNORED - PROCESS NOT RUNNING");
    }
  }

  if (String(topic) == TOPIC_COMMAND && message == "PAUSE") {
    if (processActive && !processPaused) {
      // ให้ loop()/handleProcessPause() เป็นผู้สั่งปิดอุปกรณ์จริง
      pauseFlagRequested = true;

      Serial.println("REMOTE PAUSE REQUEST ACCEPTED");

    } else {
      Serial.println("REMOTE PAUSE IGNORED - NOT RUNNING OR ALREADY PAUSED");
    }
  }

  if (String(topic) == TOPIC_COMMAND && message == "RESUME") {
    if (processActive && processPaused) {
      resumeFlagRequested = true;

      Serial.println("REMOTE RESUME REQUEST ACCEPTED");

    } else {
      Serial.println("REMOTE RESUME IGNORED - NOT PAUSED");
    }
  }
}

void publishStartButtonDiscovery() {
  if (!mqttClient.connected()) {
    return;
  }

  String topic = String(HA_PREFIX) + "/button/" + DEVICE_ID + "/start_process/config";

  String payload = "{";

  payload += "\"name\":\"Start Process\",";
  payload += "\"unique_id\":\"";
  payload += DEVICE_ID;
  payload += "_start_process\",";
  payload += "\"command_topic\":\"";
  payload += TOPIC_COMMAND;
  payload += "\",";
  payload += "\"payload_press\":\"START\",";
  payload += "\"availability_topic\":\"";
  payload += MQTT_AVAILABILITY;
  payload += "\",";
  payload += "\"payload_available\":\"connected\",";
  payload += "\"payload_not_available\":\"offline\",";
  payload += "\"icon\":\"mdi:play-circle\",";
  payload += "\"device\":{";
  payload += "\"identifiers\":[\"";
  payload += DEVICE_ID;
  payload += "\"],";
  payload += "\"name\":\"";
  payload += DEVICE_NAME;
  payload += "\",";
  payload += "\"manufacturer\":\"ESP32\",";
  payload += "\"model\":\"Rice Growing + Drying System\"";
  payload += "}";
  payload += "}";

  bool result = mqttClient.publish(topic.c_str(), payload.c_str(), true);

  Serial.print("HA Start button discovery: ");
  Serial.println(result ? "SUCCESS" : "FAILED");

  mqttClient.loop();
  delay(250);
}

void publishStopButtonDiscovery() {
  if (!mqttClient.connected()) {
    return;
  }

  String topic = String(HA_PREFIX) + "/button/" + DEVICE_ID + "/stop_process/config";

  String payload = "{";

  payload += "\"name\":\"Stop Process\",";
  payload += "\"unique_id\":\"";
  payload += DEVICE_ID;
  payload += "_stop_process\",";
  payload += "\"command_topic\":\"";
  payload += TOPIC_COMMAND;
  payload += "\",";
  payload += "\"payload_press\":\"STOP\",";
  payload += "\"availability_topic\":\"";
  payload += MQTT_AVAILABILITY;
  payload += "\",";
  payload += "\"payload_available\":\"connected\",";
  payload += "\"payload_not_available\":\"offline\",";
  payload += "\"icon\":\"mdi:stop-circle\",";
  payload += "\"device\":{";
  payload += "\"identifiers\":[\"";
  payload += DEVICE_ID;
  payload += "\"],";
  payload += "\"name\":\"";
  payload += DEVICE_NAME;
  payload += "\",";
  payload += "\"manufacturer\":\"ESP32\",";
  payload += "\"model\":\"Rice Growing + Drying System\"";
  payload += "}";
  payload += "}";

  bool result = mqttClient.publish(topic.c_str(), payload.c_str(), true);

  Serial.print("HA Stop button discovery: ");
  Serial.println(result ? "SUCCESS" : "FAILED");

  mqttClient.loop();
  delay(250);
}

void publishPauseButtonDiscovery() {
  if (!mqttClient.connected()) {
    return;
  }

  String topic = String(HA_PREFIX) + "/button/" + DEVICE_ID + "/pause_process/config";

  String payload = "{";

  payload += "\"name\":\"Pause Process\",";
  payload += "\"unique_id\":\"";
  payload += DEVICE_ID;
  payload += "_pause_process\",";
  payload += "\"command_topic\":\"";
  payload += TOPIC_COMMAND;
  payload += "\",";
  payload += "\"payload_press\":\"PAUSE\",";
  payload += "\"availability_topic\":\"";
  payload += MQTT_AVAILABILITY;
  payload += "\",";
  payload += "\"payload_available\":\"connected\",";
  payload += "\"payload_not_available\":\"offline\",";
  payload += "\"icon\":\"mdi:pause-circle\",";
  payload += "\"device\":{";
  payload += "\"identifiers\":[\"";
  payload += DEVICE_ID;
  payload += "\"],";
  payload += "\"name\":\"";
  payload += DEVICE_NAME;
  payload += "\",";
  payload += "\"manufacturer\":\"ESP32\",";
  payload += "\"model\":\"Rice Growing + Drying System\"";
  payload += "}";
  payload += "}";

  bool result = mqttClient.publish(topic.c_str(), payload.c_str(), true);

  Serial.print("HA Pause button discovery: ");
  Serial.println(result ? "SUCCESS" : "FAILED");

  mqttClient.loop();
  delay(250);
}

void publishResumeButtonDiscovery() {
  if (!mqttClient.connected()) {
    return;
  }

  String topic = String(HA_PREFIX) + "/button/" + DEVICE_ID + "/resume_process/config";

  String payload = "{";

  payload += "\"name\":\"Resume Process\",";
  payload += "\"unique_id\":\"";
  payload += DEVICE_ID;
  payload += "_resume_process\",";
  payload += "\"command_topic\":\"";
  payload += TOPIC_COMMAND;
  payload += "\",";
  payload += "\"payload_press\":\"RESUME\",";
  payload += "\"availability_topic\":\"";
  payload += MQTT_AVAILABILITY;
  payload += "\",";
  payload += "\"payload_available\":\"connected\",";
  payload += "\"payload_not_available\":\"offline\",";
  payload += "\"icon\":\"mdi:play-circle-outline\",";
  payload += "\"device\":{";
  payload += "\"identifiers\":[\"";
  payload += DEVICE_ID;
  payload += "\"],";
  payload += "\"name\":\"";
  payload += DEVICE_NAME;
  payload += "\",";
  payload += "\"manufacturer\":\"ESP32\",";
  payload += "\"model\":\"Rice Growing + Drying System\"";
  payload += "}";
  payload += "}";

  bool result = mqttClient.publish(topic.c_str(), payload.c_str(), true);

  Serial.print("HA Resume button discovery: ");
  Serial.println(result ? "SUCCESS" : "FAILED");

  mqttClient.loop();
  delay(250);
}

void publishHomeAssistantDiscovery() {
  if (!mqttClient.connected()) {
    Serial.println("Cannot publish HA Discovery - MQTT disconnected");

    return;
  }

  Serial.println();
  Serial.println("================================");
  Serial.println(" HOME ASSISTANT MQTT DISCOVERY");
  Serial.println("================================");

  // ==========================================================
  // REMOTE START BUTTON
  // ==========================================================

  publishStartButtonDiscovery();
  publishStopButtonDiscovery();
  publishPauseButtonDiscovery();
  publishResumeButtonDiscovery();

  // ==========================================================
  // PAUSED STATUS
  // ==========================================================

  publishDiscovery("binary_sensor", "paused", "Process Paused", TOPIC_PAUSED, "", "", "", "ON",
                   "OFF");

  // ==========================================================
  // WATER TEMPERATURE
  // ==========================================================

  publishDiscovery("sensor", "water_temperature", "Water Temperature", TOPIC_WATER_TEMP, "°C",
                   "temperature", "measurement", "", "");

  // ==========================================================
  // AIR TEMPERATURE
  // ==========================================================

  publishDiscovery("sensor", "temperature", "Temperature", TOPIC_TEMP, "°C", "temperature",
                   "measurement", "", "");

  // ==========================================================
  // HUMIDITY
  // ==========================================================

  publishDiscovery("sensor", "humidity", "Humidity", TOPIC_HUMIDITY, "%", "humidity", "measurement",
                   "", "");

  // ==========================================================
  // PROCESS STAGE
  // ==========================================================

  publishDiscovery("sensor", "stage", "Process Stage", TOPIC_STAGE, "", "", "", "", "");

  // ==========================================================
  // PROCESS STATUS
  // ==========================================================

  publishDiscovery("sensor", "process", "Process Status", TOPIC_PROCESS, "", "", "", "", "");

  // ==========================================================
  // PROCESS TIMERS
  // ==========================================================

  publishDiscovery("sensor", "stage1_time", "Stage 1 Time", TOPIC_STAGE1_TIME, "s", "duration",
                   "measurement", "", "");

  publishDiscovery("sensor", "stage2_time", "Stage 2 Time", TOPIC_STAGE2_TIME, "s", "duration",
                   "measurement", "", "");

  publishDiscovery("sensor", "stage3_time", "Stage 3 Time", TOPIC_STAGE3_TIME, "s", "duration",
                   "measurement", "", "");

  publishDiscovery("sensor", "total_time", "Total Process Time", TOPIC_TOTAL_TIME, "s", "duration",
                   "measurement", "", "");

  // ==========================================================
  // HEATER
  // ==========================================================

  publishDiscovery("binary_sensor", "heater", "Heater", TOPIC_HEATER, "", "", "", "ON", "OFF");

  // ==========================================================
  // MIST MAKER
  // ==========================================================

  publishDiscovery("binary_sensor", "mistmaker", "Mist Maker", TOPIC_MISTMAKER, "", "", "", "ON",
                   "OFF");

  // ==========================================================
  // FAN
  // ==========================================================

  publishDiscovery("binary_sensor", "fan", "Drying Fan", TOPIC_FAN, "", "", "", "ON", "OFF");

  // ==========================================================
  // HEATER BLOWER FANS
  // ==========================================================

  publishDiscovery("binary_sensor", "blower", "Heater Blower Fans", TOPIC_BLOWER, "", "", "", "ON",
                   "OFF");

  // ==========================================================
  // MOTOR
  // ==========================================================

  publishDiscovery("binary_sensor", "motor", "Motor", TOPIC_MOTOR, "", "", "", "ON", "OFF");

  // ==========================================================
  // SOLENOID IN
  // ==========================================================

  publishDiscovery("binary_sensor", "solenoid1", "Water Inlet Solenoid", TOPIC_SOLENOID1, "", "",
                   "", "ON", "OFF");

  // ==========================================================
  // SOLENOID DRAIN
  // ==========================================================

  publishDiscovery("binary_sensor", "solenoid2", "Drain Solenoid", TOPIC_SOLENOID2, "", "", "",
                   "ON", "OFF");

  Serial.println();
  Serial.println("--------------------------------");
  Serial.println("Home Assistant Discovery DONE");
  Serial.println("--------------------------------");
}

// ============================================================
// MQTT CONNECT
// ============================================================

void connectMQTT() {
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("MQTT skipped - WiFi not connected");

    return;
  }

  int attempt = 0;

  while (!mqttClient.connected() && attempt < 10) {
    String clientId = createMQTTClientID();

    Serial.println();
    Serial.println("================================");
    Serial.println("MQTT CONNECTION");
    Serial.println("================================");

    Serial.print("Server: ");
    Serial.println(MQTT_SERVER);

    Serial.print("Port: ");
    Serial.println(MQTT_PORT);

    Serial.print("Username: ");
    Serial.println(MQTT_USER);

    Serial.print("Client ID: ");
    Serial.println(clientId);

    lcdShow("Connecting MQTT", "Attempt " + String(attempt + 1));

    // --------------------------------------------------------
    // CONNECT WITH LWT
    // --------------------------------------------------------

    if (mqttClient.connect(clientId.c_str(),

                           MQTT_USER, MQTT_PASSWORD,

                           TOPIC_STATUS, 0, true, "offline")) {
      Serial.println("MQTT CONNECTED!");

      // ------------------------------------------------------
      // Availability
      // ------------------------------------------------------

      bool statusResult = mqttClient.publish(TOPIC_STATUS, "connected", true);

      if (statusResult) {
        Serial.println("Status publish: SUCCESS");

      } else {
        Serial.println("Status publish: FAILED");
      }

      delay(300);

      // รับคำสั่งควบคุมจาก Home Assistant

      bool subscribeResult = mqttClient.subscribe(TOPIC_COMMAND);

      Serial.print("Command subscribe: ");

      Serial.println(subscribeResult ? "SUCCESS" : "FAILED");

      // ------------------------------------------------------
      // Home Assistant Discovery
      // ------------------------------------------------------

      publishHomeAssistantDiscovery();

      mqttClient.loop();

      lcdShow("MQTT Connected", "HA Discovery OK");

      delay(1000);

      return;

    } else {
      Serial.print("MQTT FAILED, rc=");

      Serial.println(mqttClient.state());

      delay(3000);

      attempt++;
    }
  }

  Serial.println("MQTT CONNECTION FAILED!");

  lcdShow("MQTT Failed", "Check Broker");

  delay(1000);
}

// ============================================================
// ENSURE CONNECTION
// ============================================================

void ensureConnected() {
  serviceSensorESPNow();
  unsigned long now = millis();

  // ==========================================================
  // WIFI
  // ==========================================================

  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastReconnectAttempt >= RECONNECT_INTERVAL) {
      lastReconnectAttempt = now;

      Serial.println("Retrying WiFi...");

      WiFi.disconnect();

      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }

    return;
  }

  // ==========================================================
  // MQTT
  // ==========================================================

  if (!mqttClient.connected()) {
    if (now - lastReconnectAttempt >= RECONNECT_INTERVAL) {
      lastReconnectAttempt = now;

      String clientId = createMQTTClientID();

      Serial.println();
      Serial.println("Trying MQTT reconnect...");

      Serial.print("Client ID: ");

      Serial.println(clientId);

      if (mqttClient.connect(clientId.c_str(),

                             MQTT_USER, MQTT_PASSWORD,

                             TOPIC_STATUS, 0, true, "offline")) {
        Serial.println("MQTT RECONNECTED!");

        // Availability

        mqttClient.publish(TOPIC_STATUS, "connected", true);

        delay(300);

        // รับคำสั่งควบคุมจาก Home Assistant อีกครั้งหลัง reconnect

        bool subscribeResult = mqttClient.subscribe(TOPIC_COMMAND);

        Serial.print("Command subscribe: ");

        Serial.println(subscribeResult ? "SUCCESS" : "FAILED");

        // Discovery

        publishHomeAssistantDiscovery();

      } else {
        Serial.print("MQTT reconnect failed, rc=");

        Serial.println(mqttClient.state());
      }
    }

    return;
  }

  // ==========================================================
  // MQTT LOOP
  // ==========================================================

  mqttClient.loop();
}

// ============================================================
// MQTT DELAY
// ============================================================

void mqttDelay(unsigned long duration) {
  unsigned long start = millis();

  // ยอด pause ที่ดีเลย์นี้ชดเชยไปแล้ว (เป็นของดีเลย์นี้เท่านั้น)
  unsigned long pauseBaseline = totalPausedMillis;

  while (millis() - start < duration) {
    // ตรวจปุ่มและคำสั่ง STOP ตลอดช่วงหน่วงเวลา
    if (handleProcessStop()) {
      break;
    }

    // ตรวจคำสั่ง PAUSE/RESUME จาก Home Assistant
    // ถ้ากำลัง pause ฟังก์ชันนี้จะค้างรอจนกว่าจะ resume หรือ stop
    handleProcessPause();

    if (processAborted) {
      break;
    }

    // ถ้ามีการ pause เกิดขึ้น ให้เลื่อนจุดเริ่มของดีเลย์นี้ออกไป
    // เท่ากับเวลาที่ pause ไป เพื่อไม่ให้ duration สั้นลง
    compensatePause(start, pauseBaseline);

    ensureConnected();
    serviceSensorESPNow();

    // PATCH: ปิด Blower เมื่อ cool-down ครบเวลา (non-blocking)
    updateBlowerCooldown();

    updateProcessTimers();
    updateProcessLCD();
    recoverLCDIfNeeded();

    delay(50);
  }
}

// ============================================================
// MQTT PUBLISH STAGE
// ============================================================

void publishStage(const char* stage) {
  ensureConnected();

  if (mqttClient.connected()) {
    mqttClient.publish(TOPIC_STAGE, stage, true);
  }
}

// ============================================================
// MQTT PUBLISH STATE
// ============================================================

void publishState(const char* topic, bool state) {
  ensureConnected();

  if (mqttClient.connected()) {
    mqttClient.publish(topic, state ? "ON" : "OFF", true);
  }
}

// ============================================================
// PROCESS TIMER FUNCTIONS
// ============================================================

void publishTimerValue(const char* topic, unsigned long seconds) {
  if (!mqttClient.connected()) {
    return;
  }

  char buffer[16];

  snprintf(buffer, sizeof(buffer), "%lu", seconds);

  mqttClient.publish(topic, buffer, true);
}

void publishAllTimerValues() {
  publishTimerValue(TOPIC_STAGE1_TIME, stage1ElapsedSeconds);

  publishTimerValue(TOPIC_STAGE2_TIME, stage2ElapsedSeconds);

  publishTimerValue(TOPIC_STAGE3_TIME, stage3ElapsedSeconds);

  publishTimerValue(TOPIC_TOTAL_TIME, totalElapsedSeconds);
}

void resetProcessTimers() {
  stage1ElapsedSeconds = 0;
  stage2ElapsedSeconds = 0;
  stage3ElapsedSeconds = 0;
  totalElapsedSeconds = 0;

  currentStage = 0;
  currentErrorMessage = "";

  stageTimerRunning = false;
  processTimerRunning = false;

  publishAllTimerValues();
}

void startProcessTimer() {
  resetProcessTimers();

  processStartMillis = millis();

  processTimerRunning = true;

  lastTimerPublishMillis = 0;
  lastLCDSwitchMillis = millis();
  lcdShowTimerPage = true;
}

void startStageTimer(uint8_t stage) {
  currentStage = stage;
  stageStartMillis = millis();
  stageTimerRunning = true;

  lastLCDSwitchMillis = millis();
  lcdShowTimerPage = true;

  updateProcessTimers();
  publishAllTimerValues();
}

void stopStageTimer() {
  updateProcessTimers();

  stageTimerRunning = false;

  publishAllTimerValues();
}

void stopProcessTimer() {
  updateProcessTimers();

  processTimerRunning = false;
  stageTimerRunning = false;

  publishAllTimerValues();
}

void updateProcessTimers() {
  unsigned long now = millis();

  if (processTimerRunning) {
    totalElapsedSeconds = (now - processStartMillis) / 1000UL;
  }

  if (stageTimerRunning) {
    unsigned long elapsed = (now - stageStartMillis) / 1000UL;

    if (currentStage == 1) {
      stage1ElapsedSeconds = elapsed;

    } else if (currentStage == 2) {
      stage2ElapsedSeconds = elapsed;

    } else if (currentStage == 3) {
      stage3ElapsedSeconds = elapsed;
    }
  }

  if (mqttClient.connected() && now - lastTimerPublishMillis >= 1000UL) {
    lastTimerPublishMillis = now;

    publishAllTimerValues();
  }
}

String formatElapsedTime(unsigned long seconds) {
  unsigned long hours = seconds / 3600UL;

  unsigned long minutes = (seconds % 3600UL) / 60UL;

  unsigned long secs = seconds % 60UL;

  char buffer[16];

  snprintf(buffer, sizeof(buffer), "%02lu:%02lu:%02lu", hours, minutes, secs);

  return String(buffer);
}

void updateProcessLCD() {
  if (!processTimerRunning) {
    return;
  }

  unsigned long now = millis();

  // Error message has priority over normal process pages.
  // Keep it visible until the sensor recovers or the process stops.
  static unsigned long lastErrorRefresh = 0;

  if (currentErrorMessage != "") {
    if (now - lastErrorRefresh >= 1000UL) {
      lastErrorRefresh = now;

      lcdShow(currentErrorMessage, "Check Sensor");
    }

    return;
  }

  if (now - lastLCDSwitchMillis >= LCD_PAGE_DURATION) {
    lastLCDSwitchMillis = now;
    lcdShowTimerPage = !lcdShowTimerPage;
  }

  static bool previousPage = false;
  static unsigned long lastRefresh = 0;

  if (lcdShowTimerPage == previousPage && now - lastRefresh < 1000UL) {
    return;
  }

  previousPage = lcdShowTimerPage;
  lastRefresh = now;

  if (lcdShowTimerPage) {
    unsigned long stageSeconds = 0;

    if (currentStage == 1) {
      stageSeconds = stage1ElapsedSeconds;

    } else if (currentStage == 2) {
      stageSeconds = stage2ElapsedSeconds;

    } else if (currentStage == 3) {
      stageSeconds = stage3ElapsedSeconds;
    }

    lcdShow("S" + String(currentStage) + "   " + formatElapsedTime(stageSeconds),

            "TOTAL " + formatElapsedTime(totalElapsedSeconds));

  } else {
    // แสดงเฉพาะค่าที่เกี่ยวข้องกับ Stage ปัจจุบัน
    // PATCH: ใช้ค่าที่ผ่าน smoothing (buffer average) แล้วเสมอ
    // เพื่อไม่ให้ LCD กระพริบตามค่าดิบที่ยังแกว่งอยู่บ้าง
    if (currentStage == 1) {
      String line1 = "WATER  ";

      float displayWater = getBufferAverage(waterTempLog);
      if (isnan(displayWater)) {
        displayWater = latestWaterTemp;
      }

      if (isnan(displayWater)) {
        line1 += "--.-C";

      } else {
        line1 += String(displayWater, 1) + "C";
      }

      String line2 = "TARGET " + String(WATER_TEMP_TARGET, 1) + "C";

      lcdShow(line1, line2);

    } else if (currentStage == 2) {
      float displayTemp = getBufferAverage(airTempLog);
      float displayHumid = getBufferAverage(humidLog);

      if (isnan(displayTemp)) displayTemp = latestAirTemp;
      if (isnan(displayHumid)) displayHumid = latestHumidity;

      String line1 = "T ";

      if (isnan(displayTemp)) {
        line1 += "--.-";

      } else {
        line1 += String(displayTemp, 1);
      }

      line1 += "C FAN ON";

      String line2 = "HUM  ";

      if (isnan(displayHumid)) {
        line2 += "--.-";

      } else {
        line2 += String(displayHumid, 1);
      }

      line2 += "/" + String(STAGE2_HUMID_TARGET, 1) + "%";

      lcdShow(line1, line2);

    } else if (currentStage == 3) {
      float displayTemp = getBufferAverage(airTempLog);
      float displayHumid = getBufferAverage(humidLog);

      if (isnan(displayTemp)) displayTemp = latestAirTemp;
      if (isnan(displayHumid)) displayHumid = latestHumidity;

      String line1 = "TEMP ";

      if (isnan(displayTemp)) {
        line1 += "--.-";

      } else {
        line1 += String(displayTemp, 1);
      }

      line1 += "/" + String(STAGE3_TEMP_TARGET, 0) + "C";

      String line2 = "HUM  ";

      if (isnan(displayHumid)) {
        line2 += "--.-";

      } else {
        line2 += String(displayHumid, 1);
      }

      line2 += "/" + String(STAGE3_RH_TARGET, 1) + "%";

      lcdShow(line1, line2);
    }
  }
}

// ============================================================
// HEATER + BLOWER FAN CONTROL (พร้อม BLOWER COOL-DOWN)
// GPIO18 = Heater SSR
// BLOWER_RELAY = Relay 1 ช่อง สำหรับพัดลมหอยโข่ง 2 ตัว
//
// state = true  : เปิด Blower ก่อน แล้วเปิด Heater (ยกเลิก cool-down ถ้ากำลังนับอยู่)
// state = false : ปิด Heater ทันที แต่ Blower เดินต่อจนครบ BLOWER_COOLDOWN_MS
//                 (ปิดผ่าน updateBlowerCooldown() ซึ่งถูกเรียกใน mqttDelay(),
//                  ลูป Pause และ loop())
// ============================================================

void setHeaterAndBlower(bool state) {
  if (state) {
    // เปิดพัดลมก่อน Heater และยกเลิกการนับ cool-down
    blowerCooling = false;

    digitalWrite(BLOWER_RELAY, HIGH);

    digitalWrite(HEATER, HIGH);

    publishState(TOPIC_HEATER, true);

    publishState(TOPIC_BLOWER, true);

  } else {
    // ปิด Heater ก่อนเสมอ
    digitalWrite(HEATER, LOW);

    publishState(TOPIC_HEATER, false);

    if (digitalRead(BLOWER_RELAY) == HIGH) {
      // Blower ยังเปิดอยู่ -> เริ่มนับ cool-down (เริ่มครั้งเดียวต่อรอบ)
      if (!blowerCooling) {
        blowerCooling = true;
        blowerCooldownStart = millis();

        Serial.println("Heater OFF - blower cool-down started");
      }

    } else {
      // Blower ปิดอยู่แล้ว
      publishState(TOPIC_BLOWER, false);
    }
  }
}

// ปิด Blower ทันที (ใช้เมื่อ cool-down ครบเวลา หรือเมื่อสั่งหยุดแบบไม่ cool-down)
void forceBlowerOff() {
  blowerCooling = false;

  digitalWrite(BLOWER_RELAY, LOW);

  publishState(TOPIC_BLOWER, false);
}

// เรียกบ่อยๆ จากทุกลูปที่รออยู่ - ปิด Blower เมื่อครบเวลา cool-down
void updateBlowerCooldown() {
  if (!blowerCooling) {
    return;
  }

  if (millis() - blowerCooldownStart >= BLOWER_COOLDOWN_MS) {
    forceBlowerOff();

    Serial.println("Blower cool-down finished - blower OFF");
  }
}

// ============================================================
// HYSTERESIS CONTROL (PATCH)
// ตัดสินใจ ON/OFF แบบมี dead-band กันสลับถี่ๆ เวลาค่าแกว่งเบาๆ
// value        = ค่าที่ใช้ตัดสินใจ (แนะนำใช้ buffer average)
// targetLow    = ค่าเป้าหมาย (เดิมใช้เทียบว่า value < target ให้เปิด)
// hysteresis   = ระยะกันชน
// currentState = สถานะอุปกรณ์ปัจจุบัน
// ============================================================

bool hysteresisControl(float value, float targetLow, float hysteresis, bool currentState) {
  if (value < targetLow - hysteresis) {
    return true;
  }

  if (value > targetLow + hysteresis) {
    return false;
  }

  return currentState;
}

// ============================================================
// PUBLISH SENSORS
// ============================================================

// PATCH: ฟังก์ชันนี้ไม่อ่านเซนเซอร์เองอีกต่อไป (เดิมเรียก readWaterTemp()
// และ readAmbient() ซ้ำทุกครั้งที่เรียก ทำให้ในหนึ่งรอบควบคุมของแต่ละ
// Stage เกิดการอ่าน DS18B20/SHT45 ซ้ำสองรอบ - รอบแรกตอน Stage อ่านเพื่อ
// ใช้ตัดสินใจควบคุม แล้วรอบสองตอนเรียกฟังก์ชันนี้เพื่อส่ง MQTT)
// ตอนนี้ใช้ค่าล่าสุดที่ถูก cache ไว้แล้ว (latestWaterTemp/latestAirTemp/
// latestHumidity ซึ่งถูกอัปเดตอยู่แล้วทุกครั้งที่ readWaterTemp()/
// readAmbient() ถูกเรียกจากที่อื่น) ผู้เรียกต้องอ่านเซนเซอร์เองก่อน
// เรียกฟังก์ชันนี้ ถ้าต้องการค่าสดใหม่จริงๆ
void publishAllSensors() {
  ensureConnected();

  float waterTemp = latestWaterTemp;
  float temp = latestAirTemp;
  float humid = latestHumidity;

  Serial.println();
  Serial.println("---------- SENSOR ----------");

  // Water temperature

  if (!isnan(waterTemp)) {
    Serial.print("Water Temp: ");

    Serial.print(waterTemp);

    Serial.println(" C");

  } else {
    Serial.println("Water Temp: unavailable");
  }

  // SHT45

  if (!isnan(temp) && !isnan(humid)) {
    Serial.print("SHT45 Temp: ");

    Serial.print(temp);

    Serial.println(" C");

    Serial.print("Humidity: ");

    Serial.print(humid);

    Serial.println(" % RH");

  } else {
    Serial.println("SHT45: unavailable");
  }

  Serial.println("----------------------------");

  if (!mqttClient.connected()) {
    return;
  }

  // Original HA behavior: publish only valid readings; retain last broker value.
  // A retained value is NOT proof that the sensor is currently healthy.

  char buf[16];

  // Water temperature

  if (!isnan(waterTemp)) {
    dtostrf(waterTemp, 4, 1, buf);

    mqttClient.publish(TOPIC_WATER_TEMP, buf, true);
  }

  // Air temperature

  if (!isnan(temp)) {
    dtostrf(temp, 4, 1, buf);

    mqttClient.publish(TOPIC_TEMP, buf, true);
  }

  // Humidity

  if (!isnan(humid)) {
    dtostrf(humid, 4, 1, buf);

    mqttClient.publish(TOPIC_HUMIDITY, buf, true);
  }
}

// ============================================================
// STAGE LED CONTROL
// เปิดไฟของ Stage ปัจจุบันค้างไว้ และดับไฟ Stage อื่น
// stage = 1 แดง, stage = 2 เหลือง, stage = 3 เขียว
// stage = 0 ดับไฟทั้งหมด
// ============================================================

void setStageLED(uint8_t stage) {
  digitalWrite(LED_RED, stage == 1 ? HIGH : LOW);

  digitalWrite(LED_YELLOW, stage == 2 ? HIGH : LOW);

  digitalWrite(LED_GREEN, stage == 3 ? HIGH : LOW);
}

// ============================================================
// ALL OUTPUT OFF
// PATCH: Heater ดับทันที แต่ Blower ไม่ถูกตัดตรงนี้ - ปล่อยให้
// setHeaterAndBlower(false) เริ่ม cool-down (ปิดเองเมื่อครบเวลา)
// ============================================================

void allOutputsOff() {
  setStageLED(0);

  digitalWrite(SOLENOID_IN, LOW);

  digitalWrite(SOLENOID_DRAIN, LOW);

  digitalWrite(FAN_DC, LOW);

  digitalWrite(MISTMAKER, LOW);

  digitalWrite(MOTOR, LOW);

  setHeaterAndBlower(false);

  publishState(TOPIC_HEATER, false);

  publishState(TOPIC_MISTMAKER, false);

  publishState(TOPIC_FAN, false);

  publishState(TOPIC_MOTOR, false);

  publishState(TOPIC_SOLENOID1, false);

  publishState(TOPIC_SOLENOID2, false);
}

// ============================================================
// PROCESS STOP
// ============================================================

bool checkStopButtonHeld() {
  if (!processActive) {
    stopButtonHoldTracking = false;
    stopButtonTriggered = false;
    stopButtonHoldStart = 0;
    return false;
  }

  if (digitalRead(BUTTON_PIN) == LOW) {
    if (!stopButtonHoldTracking) {
      stopButtonHoldTracking = true;
      stopButtonTriggered = false;
      stopButtonHoldStart = millis();

      Serial.println("BUTTON DETECTED - HOLD TO STOP");
    }

    if (!stopButtonTriggered && millis() - stopButtonHoldStart >= STOP_BUTTON_HOLD_TIME) {
      stopButtonTriggered = true;

      Serial.println("STOP BUTTON HOLD CONFIRMED");

      return true;
    }

  } else {
    stopButtonHoldTracking = false;
    stopButtonTriggered = false;
    stopButtonHoldStart = 0;
  }

  return false;
}

bool handleProcessStop() {
  if (checkStopButtonHeld()) {
    processStopRequested = true;

    Serial.println("STOP SOURCE: PHYSICAL BUTTON");
  }

  if (!processActive || !processStopRequested || processAborted) {
    return processAborted;
  }

  processStopRequested = false;
  remoteStartRequested = false;
  processAborted = true;

  // เคลียร์สถานะ Pause ทั้งหมด เพราะกระบวนการถูกยกเลิกไปแล้ว
  // (ไม่ต้องล้าง totalPausedMillis ตรงนี้ เพราะจะถูกรีเซตตอนเริ่มรอบใหม่)
  processPaused = false;
  pauseFlagRequested = false;
  resumeFlagRequested = false;
  currentErrorMessage = "";
  digitalWrite(HEATER, LOW);
  digitalWrite(MISTMAKER, LOW);
  digitalWrite(SOLENOID_IN, LOW);
  digitalWrite(SOLENOID_DRAIN, LOW);
  digitalWrite(MOTOR, LOW);
  digitalWrite(FAN_DC, LOW);
  publishState(TOPIC_PAUSED, false);

  // ปิดอุปกรณ์ทั้งหมดทันทีและหยุดตัวจับเวลา
  // (Heater ดับทันที, Blower เดิน cool-down ต่อ ถ้าเปิดใช้งานไว้)
  allOutputsOff();

  if (!BLOWER_COOLDOWN_ON_PAUSE_STOP) {
    forceBlowerOff();
  }

  stopProcessTimer();
  currentStage = 0;

  publishStage("emergency_stopped");

  if (mqttClient.connected()) {
    mqttClient.publish(TOPIC_PROCESS, "stopped", true);
  }

  lcdShow("PROCESS STOPPED", "Check Machine");

  Serial.println();
  Serial.println("================================");
  Serial.println("PROCESS STOPPED - ALL OUTPUTS OFF");
  Serial.println("================================");

  return true;
}

// ============================================================
// PROCESS PAUSE / RESUME
// สั่งงานผ่าน Home Assistant (ปุ่ม Pause / Resume หรือคำสั่ง MQTT)
// ระหว่าง Pause: ปิดอุปกรณ์กำลัง "ทั้งหมด" ทันที รวมถึง Solenoid
// เติม/ระบายน้ำ แต่จำสถานะก่อน pause ไว้ทุกตัว
// เพื่อคืนกลับให้ตรงเดิมเป๊ะตอน Resume แล้วทำงานต่อจากจุดเดิม
// (ไม่รีเซต Stage ไม่รีเซตตัวจับเวลา ไม่กลับไป Idle)
// PATCH: Heater ดับทันที แต่ Blower เดิน cool-down ต่อจนครบเวลา
// (ตั้ง BLOWER_COOLDOWN_ON_PAUSE_STOP = false ถ้าต้องการให้ดับทันที)
// ============================================================

// สถานะอุปกรณ์ ณ วินาทีก่อนกด Pause (ใช้คืนค่าตอน Resume)
bool pausedState_SolenoidIn = false;
bool pausedState_SolenoidDrain = false;
bool pausedState_Motor = false;
bool pausedState_FanDC = false;
bool pausedState_Heater = false;
bool pausedState_MistMaker = false;

void enterPause() {
  if (processPaused) {
    return;
  }

  processPaused = true;
  pauseStartMillis = millis();

  // ----------------------------------------------------------
  // จำสถานะปัจจุบันของอุปกรณ์ทุกตัวไว้ก่อน แล้วค่อยปิด
  // ----------------------------------------------------------

  pausedState_SolenoidIn = (digitalRead(SOLENOID_IN) == HIGH);
  pausedState_SolenoidDrain = (digitalRead(SOLENOID_DRAIN) == HIGH);
  pausedState_Motor = (digitalRead(MOTOR) == HIGH);
  pausedState_FanDC = (digitalRead(FAN_DC) == HIGH);
  pausedState_Heater = (digitalRead(HEATER) == HIGH);
  pausedState_MistMaker = (digitalRead(MISTMAKER) == HIGH);

  // ----------------------------------------------------------
  // ปิดอุปกรณ์กำลังไฟฟ้าทั้งหมดทันที
  // Heater ดับทันที / Blower เดิน cool-down ต่อ (ถ้าเปิดใช้งานไว้)
  // ----------------------------------------------------------

  setHeaterAndBlower(false);

  if (!BLOWER_COOLDOWN_ON_PAUSE_STOP) {
    forceBlowerOff();
  }

  digitalWrite(MOTOR, LOW);
  digitalWrite(MISTMAKER, LOW);
  digitalWrite(FAN_DC, LOW);
  digitalWrite(SOLENOID_IN, LOW);
  digitalWrite(SOLENOID_DRAIN, LOW);

  publishState(TOPIC_HEATER, false);
  publishState(TOPIC_MOTOR, false);
  publishState(TOPIC_MISTMAKER, false);
  publishState(TOPIC_FAN, false);
  publishState(TOPIC_SOLENOID1, false);
  publishState(TOPIC_SOLENOID2, false);

  publishState(TOPIC_PAUSED, true);

  if (mqttClient.connected()) {
    mqttClient.publish(TOPIC_PROCESS, "paused", true);
  }

  lcdShow("PROCESS PAUSED", "Wait for Resume");

  beep2();

  Serial.println();
  Serial.println("================================");
  Serial.println("PROCESS PAUSED - HEATER OFF");
  Serial.println("(via Home Assistant)");
  Serial.println("================================");
}

void exitPause() {
  if (!processPaused) {
    return;
  }

  // ระยะเวลาของการ pause ครั้งนี้
  unsigned long pausedDuration = millis() - pauseStartMillis;

  totalPausedMillis += pausedDuration;
  if (ambientFaultActive) ambientFaultSince += pausedDuration;

  processStartMillis += pausedDuration;
  stageStartMillis += pausedDuration;

  if (lastWaterSensorOK != 0) {
    lastWaterSensorOK += pausedDuration;
  }

  // ESP-NOW packet age is wall-clock based; never extend on pause.

  // ----------------------------------------------------------
  // คืนสถานะอุปกรณ์ทุกตัวกลับไปเหมือนก่อน pause เป๊ะๆ
  // เพื่อให้ "ทำงานต่อจากเดิม" จริง ไม่ใช่เริ่มใหม่
  // (Heater/Mist Maker จะถูกคำนวณซ้ำอีกครั้งในลูปถัดไปตามเซนเซอร์
  //  อยู่แล้ว แต่ Solenoid/Motor/Fan ต้องคืนค่าตรงนี้เพราะไม่ได้ถูก
  //  สั่งซ้ำทุกรอบใน while-loop ของแต่ละ Stage)
  // ----------------------------------------------------------

  digitalWrite(SOLENOID_IN, pausedState_SolenoidIn ? HIGH : LOW);
  digitalWrite(SOLENOID_DRAIN, pausedState_SolenoidDrain ? HIGH : LOW);
  digitalWrite(MOTOR, pausedState_Motor ? HIGH : LOW);
  digitalWrite(FAN_DC, pausedState_FanDC ? HIGH : LOW);
  if (currentStage == 2 || currentStage == 3) {
    serviceSensorESPNow();
    pausedState_MistMaker = false;
    pausedState_Heater = false;  // wait for a fresh SHT45 sample in stage loop
  }
  digitalWrite(MISTMAKER, pausedState_MistMaker ? HIGH : LOW);
  setHeaterAndBlower(pausedState_Heater);

  publishState(TOPIC_SOLENOID1, pausedState_SolenoidIn);
  publishState(TOPIC_SOLENOID2, pausedState_SolenoidDrain);
  publishState(TOPIC_MOTOR, pausedState_Motor);
  publishState(TOPIC_FAN, pausedState_FanDC);
  publishState(TOPIC_MISTMAKER, pausedState_MistMaker);

  processPaused = false;

  publishState(TOPIC_PAUSED, false);

  if (mqttClient.connected()) {
    mqttClient.publish(TOPIC_PROCESS, "running", true);
  }

  lcdShow("RESUMING", "Please Wait...");

  beep2();

  Serial.println();
  Serial.println("================================");
  Serial.println("PROCESS RESUMED - OUTPUTS RESTORED");
  Serial.print("Paused for (ms): ");
  Serial.println(pausedDuration);
  Serial.print("Total paused (ms): ");
  Serial.println(totalPausedMillis);
  Serial.println("(via Home Assistant)");
  Serial.println("================================");
}

// เรียกฟังก์ชันนี้บ่อยๆ ในทุกจุดที่เดิมเรียก handleProcessStop()
// คืนค่า true = ขณะนี้ยัง pause ค้างอยู่ (ผู้เรียกไม่ควรทำงานต่อ)
// คืนค่า false = ไม่ได้ pause แล้ว (ทำงานต่อได้ตามปกติ)
bool handleProcessPause() {
  if (!processActive || processAborted) {
    pauseFlagRequested = false;
    resumeFlagRequested = false;
    return false;
  }

  if (pauseFlagRequested && !processPaused) {
    pauseFlagRequested = false;
    enterPause();
  }

  if (resumeFlagRequested && processPaused) {
    resumeFlagRequested = false;
    exitPause();
  }

  // ค้างอยู่ตรงนี้ตลอดช่วง pause โดยยังคง
  // ตรวจ STOP, ต่อ WiFi/MQTT, และกู้ LCD ตามปกติ
  while (processPaused && processActive && !processAborted) {
    if (handleProcessStop()) {
      return false;
    }

    ensureConnected();
    serviceSensorESPNow();

    // PATCH: ให้ Blower cool-down ทำงานต่อระหว่าง Pause
    updateBlowerCooldown();

    recoverLCDIfNeeded();

    serviceSensorESPNow();
    if (resumeFlagRequested) {
      resumeFlagRequested = false;
      exitPause();
      break;
    }

    delay(100);
  }

  return processPaused;
}

// ============================================================
// BUTTON
// ============================================================

bool checkButtonPressed() {
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);

    if (digitalRead(BUTTON_PIN) == LOW) {
      unsigned long pressStart = millis();

      Serial.println("BUTTON DETECTED - HOLD TO START");

      // ต้องกดค้างต่อเนื่องจนครบ BUTTON_HOLD_TIME
      while (digitalRead(BUTTON_PIN) == LOW) {
        ensureConnected();

        if (millis() - pressStart >= BUTTON_HOLD_TIME) {
          Serial.println("BUTTON HOLD CONFIRMED");

          // รอให้ปล่อยปุ่มก่อนเริ่มกระบวนการ
          while (digitalRead(BUTTON_PIN) == LOW) {
            ensureConnected();
            delay(10);
          }

          // Debounce ตอนปล่อยปุ่ม
          delay(100);

          Serial.println("BUTTON RELEASED - START ACCEPTED");

          return true;
        }

        delay(10);
      }

      Serial.println("BUTTON RELEASED TOO SOON - START CANCELLED");

      return false;
    }
  }

  return false;
}

// ============================================================
// STAGE 1
// ============================================================

void stage1_FillAndHeatWater() {
  startStageTimer(1);

  Serial.println();
  Serial.println("================================");

  Serial.println("STAGE 1: FILL + HEAT WATER");

  Serial.println("================================");

  publishStage("stage1_fill_heat");

  // ไฟสีแดงติดค้างตลอด Stage 1
  setStageLED(1);

  // แจ้งเตือนหนึ่งครั้งเมื่อเข้าสู่ Stage 1
  beep2();

  lcdShow("STAGE 1", "Filling Water");

  digitalWrite(SOLENOID_IN, HIGH);

  publishState(TOPIC_SOLENOID1, true);

  mqttDelay(FILL_WATER_TIME);

  if (processAborted) {
    return;
  }

  digitalWrite(SOLENOID_IN, LOW);

  publishState(TOPIC_SOLENOID1, false);

  Serial.println("Water filling complete");

  // รอให้แรงดันและสัญญาณรบกวนจาก Solenoid/Relay สงบ
  // ก่อนอ่าน DS18B20 และสั่ง Heater/Blower
  mqttDelay(SOLENOID_SETTLE_TIME);

  if (processAborted) {
    return;
  }

  unsigned long start = millis();

  // ยอด pause ที่ลูปของ Stage นี้ชดเชยไปแล้ว
  unsigned long pauseBaseline = totalPausedMillis;

  // สำคัญ: ต้อง compensatePause() ก่อนเช็คเงื่อนไขเวลาเสมอ
  // ถ้าเช็คเงื่อนไขก่อน (แบบเดิม) แล้ว pause ไปเกิดขึ้นตอนอยู่ใน
  // mqttDelay() ที่ซ้อนอยู่ข้างใน พอกลับออกมาที่นี่ start ยังไม่ถูก
  // ชดเชย เงื่อนไขจะเจอเวลาที่ "กระโดด" ไปเลยและออกจากลูปทันที
  // จึงใช้ while(true) แล้วเช็คเวลาไว้หลัง compensatePause() แทน
  while (!processAborted) {
    if (handleProcessStop()) {
      return;
    }

    handleProcessPause();

    if (processAborted) {
      return;
    }

    // ชดเชยเวลาที่ pause ไป ไม่ให้ถูกนับเป็นเวลาทำงานของ Stage
    compensatePause(start, pauseBaseline);

    // เช็คเงื่อนไขเวลาหลังชดเชยเสมอ
    if (millis() - start >= STAGE1_DURATION) {
      break;
    }

    ensureConnected();

    // ค่าดิบของรอบนี้ (single-shot, ไม่อ่านซ้ำ) - ใช้ตรงๆ ถ้าอ่านไม่ได้
    float waterTemp = readWaterTemp();

    // บันทึกเข้า log buffer (จะบันทึกจริงทุก 2 วิเท่านั้น)
    maybeAddToLogBuffers(waterTemp, NAN, NAN);

    // PATCH: Sensor Error -> Heater OFF ต้องตรวจทั้งค่า NAN และ
    // การ timeout ของเซนเซอร์ (เผื่อเซนเซอร์ค้างค่าเดิมแต่ไม่อัปเดตจริง)
    if (isnan(waterTemp) || waterSensorTimeout()) {
      Serial.println("DS18B20 unavailable/timeout - Heater OFF");

      stage1_heaterState = false;

      setHeaterAndBlower(false);

      currentErrorMessage = "WATER SENSOR ERROR";

      publishState(TOPIC_HEATER, false);

    } else {
      currentErrorMessage = "";

      // ใช้ค่าเฉลี่ยจาก buffer ถ้ามีข้อมูลพอแล้ว (ราบรื่นกว่า)
      // ถ้า buffer ยังว่าง (เพิ่งเริ่ม Stage) ใช้ค่าดิบไปพลางก่อน
      float smoothed = getBufferAverage(waterTempLog);

      if (isnan(smoothed)) {
        smoothed = waterTemp;
      }

      stage1_heaterState =
          hysteresisControl(smoothed, WATER_TEMP_TARGET, WATER_TEMP_HYSTERESIS, stage1_heaterState);

      setHeaterAndBlower(stage1_heaterState);

      publishState(TOPIC_HEATER, stage1_heaterState);
    }

    publishAllSensors();

    mqttDelay(2000);
  }

  if (processAborted) {
    return;
  }

  setHeaterAndBlower(false);

  publishState(TOPIC_HEATER, false);

  stopStageTimer();

  publishStage("stage1_complete");

  lcdShow("Stage 1 Done", "Water Heated");

  mqttDelay(1000);
}

// ============================================================
// STAGE 2
// ============================================================

// Called only by stages which depend on SHT45, after critical outputs are OFF.
bool stopForAmbientFault(bool deadlineReached = false) {
  if (!ambientFaultActive) return false;
  if (!deadlineReached && millis() - ambientFaultSince < SHT45_FAULT_STOP_MS) return false;
  Serial.println("[SAFETY] SHT45 fault: cancelling process, manual restart required");
  processStopRequested = true;
  handleProcessStop();  // keep existing HA emergency_stopped / stopped states
  lcdShow("SHT45 ERROR", "Process stopped");
  return true;
}

void stage2_DrainAndHumidify() {
  ambientFaultActive = false;
  unsigned long sensorHoldStart = 0;
  bool sensorHoldActive = false;

  startStageTimer(2);

  Serial.println();
  Serial.println("================================");

  Serial.println("STAGE 2: DRAIN + HUMIDIFY");

  Serial.println("================================");

  publishStage("stage2_drain_humidify");

  // ดับไฟ Stage 1 และเปิดไฟสีเหลืองค้างตลอด Stage 2
  setStageLED(2);

  // Stage 2 configuration:
  // - PTC/Heater (GPIO18) must remain OFF for the whole stage.
  // - Ventilation fan FAN_DC (GPIO26) runs for the whole stage.
  // The heater blower may finish its existing Stage 1 cool-down, then stops
  // normally; it is not turned back on by Stage 2.
  stage2_heaterState = false;
  digitalWrite(HEATER, LOW);
  publishState(TOPIC_HEATER, false);

  digitalWrite(FAN_DC, HIGH);
  publishState(TOPIC_FAN, true);

  // แจ้งเตือนหนึ่งครั้งเมื่อเข้าสู่ Stage 2
  beep2();

  lcdShow("STAGE 2", "Draining Water");

  digitalWrite(SOLENOID_DRAIN, HIGH);

  publishState(TOPIC_SOLENOID2, true);

  mqttDelay(DRAIN_WATER_TIME);

  if (processAborted) {
    return;
  }

  digitalWrite(SOLENOID_DRAIN, LOW);

  publishState(TOPIC_SOLENOID2, false);

  // รอให้แรงดันและสัญญาณรบกวนจาก Solenoid/Relay สงบ
  // ก่อนอ่าน SHT45 และสั่ง Mist Maker/Heater
  mqttDelay(SOLENOID_SETTLE_TIME);

  if (processAborted) {
    return;
  }

  unsigned long start = millis();

  // ยอด pause ที่ลูปของ Stage นี้ชดเชยไปแล้ว
  unsigned long pauseBaseline = totalPausedMillis;

  // สำคัญ: ต้อง compensatePause() ก่อนเช็คเงื่อนไขเวลาเสมอ (ดูเหตุผลที่ Stage 1)
  while (!processAborted) {
    if (handleProcessStop()) {
      return;
    }

    handleProcessPause();

    if (processAborted) {
      return;
    }

    // ชดเชยเวลาที่ pause ไป ไม่ให้ถูกนับเป็นเวลาทำงานของ Stage
    compensatePause(start, pauseBaseline);

    // Read a fresh sensor sample before allowing normal completion.
    ensureConnected();

    float temp;
    float humid;

    bool ambientOK = readAmbient(temp, humid);

    // PATCH: Sensor Error -> Heater/Mist OFF ต้องตรวจทั้งการอ่านล้มเหลว
    // และการ timeout ของเซนเซอร์
    if (!ambientOK || airSensorTimeout()) {
      Serial.println("SHT45 unavailable/timeout - Heater/Mist OFF");

      if (!sensorHoldActive) {
        sensorHoldActive = true;
        sensorHoldStart = millis() - totalPausedMillis;
      }
      digitalWrite(HEATER, LOW);
      digitalWrite(MISTMAKER, LOW);
      if (stopForAmbientFault()) return;

      stage2_heaterState = false;
      stage2_mistState = false;

      // PATCH: ใช้ setHeaterAndBlower(false) แทนการสั่งขา HEATER ตรงๆ
      // เพื่อให้ Blower เข้าสู่ cool-down (เดิมปิดแค่ Heater ทำให้ Blower
      // ค้างเปิดโดยไม่มีตัวจับเวลาปิด)
      setHeaterAndBlower(false);

      digitalWrite(MISTMAKER, LOW);

      currentErrorMessage = "SHT45 ERROR";

      publishState(TOPIC_HEATER, false);

      publishState(TOPIC_MISTMAKER, false);

    } else {
      if (sensorHoldActive) {
        // Do not count sensor-recovery time toward Stage 2 conditioning duration.
        start += (millis() - totalPausedMillis) - sensorHoldStart;
        sensorHoldActive = false;
      }
      if (millis() - start >= STAGE2_DURATION) break;
      currentErrorMessage = "";

      // บันทึกเข้า log buffer (จะบันทึกจริงทุก 2 วิเท่านั้น)
      maybeAddToLogBuffers(NAN, temp, humid);

      float smoothedHumid = getBufferAverage(humidLog);
      if (isnan(smoothedHumid)) smoothedHumid = humid;

      // Mist Maker

      stage2_mistState = hysteresisControl(smoothedHumid, STAGE2_HUMID_TARGET,
                                           STAGE2_HUMID_HYSTERESIS, stage2_mistState);

      digitalWrite(MISTMAKER, stage2_mistState ? HIGH : LOW);

      publishState(TOPIC_MISTMAKER, stage2_mistState);

      // PTC is deliberately disabled in Stage 2. Reassert both outputs every
      // control cycle so a resume or transient cannot turn the heater on.
      stage2_heaterState = false;
      digitalWrite(HEATER, LOW);
      publishState(TOPIC_HEATER, false);

      digitalWrite(FAN_DC, HIGH);
      publishState(TOPIC_FAN, true);
    }

    publishAllSensors();

    mqttDelay(2000);
  }

  if (processAborted) {
    return;
  }

  digitalWrite(MISTMAKER, LOW);

  digitalWrite(FAN_DC, LOW);

  setHeaterAndBlower(false);

  publishState(TOPIC_MISTMAKER, false);

  publishState(TOPIC_FAN, false);

  publishState(TOPIC_HEATER, false);

  stopStageTimer();

  publishStage("stage2_complete");

  lcdShow("Stage 2 Done", "Humidify OK");

  mqttDelay(1000);
}

// ============================================================
// STAGE 3
// ============================================================

void stage3_Drying() {
  ambientFaultActive = false;

  startStageTimer(3);

  Serial.println();
  Serial.println("================================");

  Serial.println("STAGE 3: DRYING");

  Serial.println("================================");

  publishStage("stage3_drying");

  // ดับไฟ Stage 2 และเปิดไฟสีเขียวค้างตลอด Stage 3
  setStageLED(3);

  // แจ้งเตือนหนึ่งครั้งเมื่อเข้าสู่ Stage 3
  beep2();

  // Fan

  digitalWrite(FAN_DC, HIGH);

  publishState(TOPIC_FAN, true);

  // Motor

  digitalWrite(MOTOR, HIGH);

  publishState(TOPIC_MOTOR, true);

  lcdShow("STAGE 3", "Drying...");

  unsigned long start = millis();

  // ยอด pause ที่ลูปของ Stage นี้ชดเชยไปแล้ว
  unsigned long pauseBaseline = totalPausedMillis;

  // PATCH: ต้องถึงเป้าหมายต่อเนื่องกี่มิลลิวินาที ก่อนถือว่า Stage 3 จบ
  // กัน Stage 3 จบทันทีจากค่าที่แกว่งผ่านเป้าเพียงชั่วครู่เดียว
  const unsigned long STAGE3_TARGET_STABLE_MS = 30000UL;
  unsigned long targetStableStart = 0;

  // สำคัญ: ต้อง compensatePause() ก่อนเช็คเงื่อนไขเวลาเสมอ (ดูเหตุผลที่ Stage 1)
  while (!processAborted) {
    if (handleProcessStop()) {
      return;
    }

    handleProcessPause();

    if (processAborted) {
      return;
    }

    // ชดเชยเวลาที่ pause ไป ไม่ให้ถูกนับเป็นเวลาทำงานของ Stage
    compensatePause(start, pauseBaseline);

    // Read a fresh sensor sample before allowing normal completion.
    ensureConnected();

    float temp;
    float humid;

    bool ambientOK = readAmbient(temp, humid);

    // PATCH: Sensor Error -> Heater OFF ต้องตรวจทั้งการอ่านล้มเหลว
    // และการ timeout ของเซนเซอร์
    if (!ambientOK || airSensorTimeout()) {
      digitalWrite(HEATER, LOW);
      digitalWrite(MISTMAKER, LOW);
      if (stopForAmbientFault(millis() - start >= STAGE3_DURATION)) return;
      publishAllSensors();

      Serial.println("SHT45 unavailable/timeout - Heater OFF");

      setHeaterAndBlower(false);

      currentErrorMessage = "SHT45 ERROR";

      publishState(TOPIC_HEATER, false);

      // ค่ายังไม่ถึงเป้า (ไม่รู้ค่าจริง) จึงรีเซตตัวจับเวลาเสถียรภาพ
      targetStableStart = 0;

      mqttDelay(2000);

      continue;
    }

    if (millis() - start >= STAGE3_DURATION) break;
    currentErrorMessage = "";

    // บันทึกเข้า log buffer (จะบันทึกจริงทุก 2 วิเท่านั้น) - ใช้เพื่อ
    // smoothing การตัดสินใจ ON/OFF ปกติเท่านั้น "ไม่ใช่" สำหรับ safety
    maybeAddToLogBuffers(NAN, temp, humid);

    Serial.print("Drying Temp: ");

    Serial.print(temp);

    Serial.print(" C | RH: ");

    Serial.print(humid);

    Serial.println(" %");

    // ==========================================================
    // SAFETY - ใช้ค่าดิบ (temp) ที่ผ่าน median filter มาโดยตรง
    // ห้ามใช้ buffer average เด็ดขาด เพราะ average มี lag
    // ถ้าอุณหภูมิพุ่งจริง ต้องตัดฮีตเตอร์ทันที ไม่ใช่รอ average ขยับตาม
    // ==========================================================

    if (temp >= STAGE3_MAX_TEMP) {
      Serial.println("OVER TEMPERATURE!");

      Serial.println("HEATER OFF FOR SAFETY");

      setHeaterAndBlower(false);

      publishState(TOPIC_HEATER, false);

      // เกินเพดานความปลอดภัย ไม่นับเป็นสถานะเสถียรที่เป้าหมาย
      targetStableStart = 0;

      mqttDelay(2000);

      continue;
    }

    // ==========================================================
    // ควบคุมปกติ - ใช้ค่าเฉลี่ยจาก buffer (ราบรื่นกว่า) ถ้ามีข้อมูลพอ
    // ==========================================================

    float smoothedTemp = getBufferAverage(airTempLog);
    if (isnan(smoothedTemp)) smoothedTemp = temp;

    float smoothedHumid = getBufferAverage(humidLog);
    if (isnan(smoothedHumid)) smoothedHumid = humid;

    // Heater

    if (smoothedTemp < STAGE3_TEMP_TARGET) {
      Serial.println("Temperature < target -> HEATER ON");

      setHeaterAndBlower(true);

      publishState(TOPIC_HEATER, true);

      // ยังไม่ถึงเป้า -> รีเซตตัวจับเวลาเสถียรภาพ
      targetStableStart = 0;

    }

    else if (smoothedHumid > STAGE3_RH_TARGET) {
      Serial.println("Temp OK but RH > target -> HEATER ON");

      setHeaterAndBlower(true);

      publishState(TOPIC_HEATER, true);

      // ยังไม่ถึงเป้า -> รีเซตตัวจับเวลาเสถียรภาพ
      targetStableStart = 0;

    }

    else {
      // PATCH: ถึงเป้าหมายแล้ว แต่ยังไม่จบทันที ต้องคงอยู่ที่เป้าหมาย
      // ต่อเนื่องอย่างน้อย STAGE3_TARGET_STABLE_MS ก่อนถือว่า Stage 3
      // เสร็จจริง กันค่ากระเพื่อมผ่านเป้าเพียงแวบเดียวแล้วจบก่อนเวลา

      setHeaterAndBlower(false);

      publishState(TOPIC_HEATER, false);

      if (targetStableStart == 0) {
        targetStableStart = millis();

        Serial.println("TARGET REACHED - confirming stability...");

      } else if (millis() - targetStableStart >= STAGE3_TARGET_STABLE_MS) {
        Serial.println("TARGET AIR RH REACHED AND STABLE!");

        Serial.println("STAGE 3 COMPLETE");

        break;
      }
    }

    publishAllSensors();

    mqttDelay(2000);
  }

  if (processAborted) {
    return;
  }

  if (millis() - start >= STAGE3_DURATION) {
    Serial.println("STAGE 3 TIME LIMIT REACHED");

    Serial.println("Stopping Stage 3 safely");
  }

  // ปิดอุปกรณ์
  // Heater ดับทันที, Blower เดิน cool-down ต่อ (ปิดเองเมื่อครบเวลา)

  setHeaterAndBlower(false);

  digitalWrite(FAN_DC, LOW);

  digitalWrite(MOTOR, LOW);

  publishState(TOPIC_HEATER, false);

  publishState(TOPIC_FAN, false);

  publishState(TOPIC_MOTOR, false);

  stopStageTimer();

  publishStage("stage3_complete");

  lcdShow("Drying Done!", "Process Complete");
}

// ============================================================
// COMPLETE PROCESS
// ============================================================

void completeProcess() {
  stopProcessTimer();

  allOutputsOff();

  publishStage("all_complete");

  if (mqttClient.connected()) {
    mqttClient.publish(TOPIC_PROCESS, "complete", true);
  }

  lcdShow("Process Done!", "ALL " + formatElapsedTime(totalElapsedSeconds));

  Serial.println();
  Serial.println("================================");

  Serial.println("ALL PROCESS COMPLETE");

  Serial.println("================================");

  beepDone();
}

// ============================================================
// CONNECT ALL
// ============================================================

void connectAll() {
  // WiFi

  connectWiFi();

  // MQTT

  connectMQTT();

  if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
    Serial.println("WiFi + MQTT CONNECTED");

    lcdShow("Connected!", "WiFi + MQTT OK");

  } else {
    Serial.println("Offline Mode");

    lcdShow("Offline Mode", "Process Continue");
  }

  mqttDelay(1000);
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);

  randomSeed(micros());

  delay(1000);

  Serial.println();
  Serial.println("================================");

  Serial.println(" RICE GROWING + DRYING SYSTEM");

  Serial.println("================================");

  // ==========================================================
  // I2C
  // ==========================================================

  // LCD ใช้ I2C bus หลัก (Wire)
  Wire.begin(LCD_I2C_SDA_PIN, LCD_I2C_SCL_PIN);


  // ป้องกัน I2C ค้างรอนานเมื่อ LCD หรือสายสัญญาณถูกรบกวน
  Wire.setTimeOut(50);

  // ==========================================================
  // LCD
  // ==========================================================

  lcd.init();

  lcd.backlight();

  // ลดความเร็ว I2C ของ LCD เพื่อลดปัญหาสัญญาณรบกวน
  // LCD ใช้ Wire แยกจาก SHT45
  Wire.setClock(40000);

  lcdShow("System Start", "Loading...");

  delay(1500);

  // ==========================================================
  // BUTTON
  // ==========================================================

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // ==========================================================
  // LED
  // ==========================================================

  pinMode(LED_RED, OUTPUT);

  pinMode(LED_YELLOW, OUTPUT);

  pinMode(LED_GREEN, OUTPUT);

  // ==========================================================
  // OUTPUTS
  // ==========================================================

  pinMode(SOLENOID_IN, OUTPUT);

  pinMode(SOLENOID_DRAIN, OUTPUT);

  pinMode(FAN_DC, OUTPUT);

  pinMode(BLOWER_RELAY, OUTPUT);

  pinMode(MISTMAKER, OUTPUT);

  pinMode(MOTOR, OUTPUT);

  pinMode(HEATER, OUTPUT);

  pinMode(BUZZER, OUTPUT);

  // ปิดทั้งหมด

  digitalWrite(LED_RED, LOW);

  digitalWrite(LED_YELLOW, LOW);

  digitalWrite(LED_GREEN, LOW);

  digitalWrite(SOLENOID_IN, LOW);

  digitalWrite(SOLENOID_DRAIN, LOW);

  digitalWrite(FAN_DC, LOW);

  digitalWrite(BLOWER_RELAY, LOW);

  digitalWrite(MISTMAKER, LOW);

  digitalWrite(MOTOR, LOW);

  digitalWrite(HEATER, LOW);

  // ยังไม่ได้เรียก tone() จึงใช้ LOW แทน noTone()
  // เพื่อไม่ให้เกิดข้อความ LEDC is not initialized ตอนเปิดเครื่อง
  digitalWrite(BUZZER, LOW);

  // ==========================================================
  // DS18B20
  // ==========================================================

  Serial.println("Initializing DS18B20...");

  ds18b20.begin();

  Serial.print("DS18B20 sensors found: ");

  Serial.println(ds18b20.getDeviceCount());

  // ESP-NOW receiver starts after Wi-Fi initialization below.

  // ==========================================================
  // WIFI
  // ==========================================================

  WiFi.persistent(true);

  WiFi.setAutoReconnect(true);

  // ==========================================================
  // MQTT
  // ==========================================================

  mqttClient.setKeepAlive(60);

  // ลดเวลาที่ MQTT รอเครือข่าย เพื่อไม่ให้ LCD เหมือนค้างนาน
  mqttClient.setSocketTimeout(1);

  mqttClient.setBufferSize(1024);

  mqttClient.setCallback(mqttCallback);

  // ==========================================================
  // CONNECT
  // ==========================================================

  connectAll();
  setupESPNowReceiver();

  // ส่งสถานะเริ่มต้น OFF ของอุปกรณ์ทั้งหมดไปยัง Home Assistant

  allOutputsOff();

  publishState(TOPIC_PAUSED, false);

  // ==========================================================
  // INITIAL TIMER STATES
  // ==========================================================

  resetProcessTimers();
  resetSensorLogBuffers();
  resetSensorTimeouts();

  // ==========================================================
  // INITIAL PROCESS STATUS
  // ==========================================================

  if (mqttClient.connected()) {
    mqttClient.publish(TOPIC_PROCESS, "idle", true);
  }

  // ==========================================================
  // SENSOR TEST
  // ==========================================================

  lcdShow("Reading Sensors", "Please wait...");

  // PATCH: publishAllSensors() ไม่อ่านเซนเซอร์เองแล้ว ต้องอ่านก่อนที่นี่
  readWaterTemp();

  float setupAmbientTemp;
  float setupAmbientHumid;

  readAmbient(setupAmbientTemp, setupAmbientHumid);

  publishAllSensors();

  mqttDelay(1000);

  // ==========================================================
  // READY
  // ==========================================================

  lcdShow("System Ready", "Press Start");

  beep2();

  Serial.println();
  Serial.println("================================");

  Serial.println("SYSTEM READY");

  Serial.println("PRESS START BUTTON");

  Serial.println("================================");
}

// ============================================================
// LOOP
// ============================================================

void loop() {
  serviceSensorESPNow();
  ensureConnected();
  recoverLCDIfNeeded();

  // PATCH: ปิด Blower เมื่อ cool-down ครบเวลา (ทำงานต่อได้แม้จบกระบวนการแล้ว)
  updateBlowerCooldown();

  // อ่าน Sensor ขณะรอ Start

  if (millis() - lastIdleSensorPublishMillis >= 2000UL) {
    lastIdleSensorPublishMillis = millis();

    // PATCH: publishAllSensors() ไม่อ่านเซนเซอร์เองแล้ว ต้องอ่านก่อนที่นี่
    readWaterTemp();

    float idleAmbientTemp;
    float idleAmbientHumid;

    readAmbient(idleAmbientTemp, idleAmbientHumid);

    publishAllSensors();
  }

  // ตรวจคำสั่ง Start จากปุ่มจริงหรือ Home Assistant

  bool startRequested = false;

  if (!processActive) {
    if (remoteStartRequested) {
      remoteStartRequested = false;
      startRequested = true;

      Serial.println("START SOURCE: HOME ASSISTANT");

    } else if (checkButtonPressed()) {
      startRequested = true;

      Serial.println("START SOURCE: PHYSICAL BUTTON");
    }
  }

  if (startRequested) {
    // ล็อกไม่ให้รับคำสั่ง Start ซ้ำระหว่างกระบวนการทำงาน
    processActive = true;
    processAborted = false;
    processStopRequested = false;

    stopButtonHoldTracking = false;
    stopButtonTriggered = false;
    stopButtonHoldStart = 0;

    processPaused = false;
    pauseFlagRequested = false;
    resumeFlagRequested = false;

    // รีเซตยอด pause สะสมทุกครั้งที่เริ่มกระบวนการใหม่
    // (สำคัญ ไม่งั้นรอบถัดไปจะถูกชดเชยเวลาเกินจริง)
    totalPausedMillis = 0;

    // รีเซตสถานะ hysteresis และ log buffer ของทุก Stage
    // (สำคัญ ไม่งั้นรอบใหม่จะเริ่มด้วยค่าที่ค้างจากรอบก่อน)
    stage1_heaterState = false;
    stage2_heaterState = false;
    stage2_mistState = false;
    resetSensorLogBuffers();
    resetSensorTimeouts();

    publishState(TOPIC_PAUSED, false);

    Serial.println();
    Serial.println("================================");

    Serial.println("START PROCESS");

    Serial.println("================================");

    startProcessTimer();

    if (mqttClient.connected()) {
      mqttClient.publish(TOPIC_PROCESS, "running", true);
    }

    lcdShow("START PROCESS", "Please Wait...");

    mqttDelay(1000);

    // ========================================================
    // STAGE 1
    // ========================================================

    if (!processAborted) {
      stage1_FillAndHeatWater();
    }

    // ========================================================
    // STAGE 2
    // ========================================================

    if (!processAborted) {
      stage2_DrainAndHumidify();
    }

    // ========================================================
    // STAGE 3
    // ========================================================

    if (!processAborted) {
      stage3_Drying();
    }

    // ========================================================
    // COMPLETE
    // ========================================================

    if (!processAborted) {
      completeProcess();

    } else {
      Serial.println("PROCESS ABORTED - WAITING FOR NEXT START");
    }

    // รอปล่อยปุ่มก่อน เพื่อไม่ให้การกด STOP กลายเป็น START รอบใหม่

    while (digitalRead(BUTTON_PIN) == LOW) {
      ensureConnected();
      updateBlowerCooldown();
      delay(10);
    }

    delay(200);

    stopButtonHoldTracking = false;
    stopButtonTriggered = false;
    stopButtonHoldStart = 0;
    processStopRequested = false;

    // ปลดล็อกหลังจากปล่อยปุ่มเรียบร้อยแล้ว
    processActive = false;

    Serial.println("WAITING FOR NEXT START...");
  }

  // ตรวจปุ่มได้รวดเร็ว โดยไม่ส่ง Sensor ถี่เกินไป
  delay(50);
}




