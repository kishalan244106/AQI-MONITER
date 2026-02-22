#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <DHT.h>
#include <SensirionI2CSgp41.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <MHZ19.h>
#include <PMS.h>

// ---------------- WiFi ----------------
#define WIFI_SSID "Redmi 13C"
#define WIFI_PASSWORD "12345678"

// ---------------- Firebase ----------------
#define API_KEY "AIzaSyC6pyBexwauJDUu2WkmhvlTXQlV5SU__Ww"
#define DATABASE_URL "https://kisha-86b74-default-rtdb.asia-southeast1.firebasedatabase.app"

#define FB_EMAIL "arunasalamkishalan@gmail.com"
#define FB_PASS  "12qw12qw34"

FirebaseData fbData;
FirebaseConfig config;
FirebaseAuth auth;

// ---------------- Sensors ----------------
Adafruit_AHTX0 aht;

// DHT22
#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// Rain & UV
#define RAIN_PIN 34
#define UV_PIN 35

// SGP41
SensirionI2CSgp41 sgp41;

// ---------------- UART Pins ----------------
// MH-Z19C: MHZ TX -> ESP32 RX2
static const int MHZ_RX_PIN = 16;
// MH-Z19C: MHZ RX -> ESP32 TX2 (MUST connect)
static const int MHZ_TX_PIN = 17;

// PMS5003: PMS TX -> ESP32 RX1
static const int PMS_RX_PIN = 14;
// PMS RX optional (not needed for normal reading)
static const int PMS_TX_PIN = -1;

// MH-Z19C (UART2)
HardwareSerial mhzSerial(2);
MHZ19 mhz19;

// PMS5003 (UART1)
HardwareSerial pmsSerial(1);
PMS pms(pmsSerial);
PMS::DATA pmsData;

// ---------------- Time ----------------
WiFiUDP ntpUDP;
// Sri Lanka offset = +5:30 = 19800 seconds
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800);

String deviceId = "device01";

// Warmup time for MHZ19C (3 minutes)
static const unsigned long MHZ_WARMUP_MS = 180000UL;
unsigned long mhzStartMs = 0;

// Fail counters
uint32_t mhzNoDataCount = 0;
uint32_t pmsFailCount = 0;

// ---- helpers ----
static inline bool isValidFloat(float v) { return !isnan(v) && isfinite(v); }

// ---------- WiFi NON-BLOCK connect ----------
bool wifiConnectNonBlock(uint32_t timeoutMs = 8000) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(true, true);
  delay(300);

  Serial.printf("WiFi: connecting to %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    Serial.print(".");
    delay(300);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi Connected!");
    Serial.print("IP: "); Serial.println(WiFi.localIP());
    Serial.print("RSSI: "); Serial.println(WiFi.RSSI());
    return true;
  } else {
    Serial.println("\n❌ WiFi NOT connected (continue without WiFi)");
    Serial.print("WiFi status: "); Serial.println((int)WiFi.status());
    return false;
  }
}

void wifiReconnectOccasionally() {
  static unsigned long lastTry = 0;
  const unsigned long everyMs = 20000;
  if (WiFi.status() == WL_CONNECTED) return;

  if (millis() - lastTry >= everyMs) {
    lastTry = millis();
    Serial.println("🔁 WiFi reconnect attempt...");
    wifiConnectNonBlock(6000);
  }
}

// ---------- PMS NON-BLOCK read (no stuck) ----------
bool readPMSNonBlock(PMS::DATA &out) {
  // read bytes for max 1200ms; if frame comes -> decode
  unsigned long start = millis();
  while (millis() - start < 1200) {
    if (pmsSerial.available() >= 32) {       // PMS frame length ~32 bytes
      if (pms.read(out)) return true;        // parse from stream
      // if parse fail, continue a bit
    }
    delay(10);
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  Serial.println("\nBOOT OK ✅");
  Serial.println("Serial working...");

  // I2C
  Wire.begin(21, 22);
  Serial.println("I2C init OK");

  // WiFi optional
  bool wifiOK = wifiConnectNonBlock(8000);

  // NTP only if WiFi ok
  if (wifiOK) {
    timeClient.begin();
    if (timeClient.update()) Serial.println("NTP updated ✅");
    else Serial.println("NTP update failed (will retry in loop)");
  } else {
    Serial.println("No WiFi -> skipping NTP");
  }

  // DHT
  dht.begin();
  Serial.println("DHT22 init OK");

  // AHT10
  if (!aht.begin()) Serial.println("AHT10 NOT found!");
  else Serial.println("AHT10 OK");

  // SGP41
  sgp41.begin(Wire);
  Serial.println("SGP41 warming up 15s...");
  delay(15000);

  // MH-Z19C UART start
  mhzSerial.begin(9600, SERIAL_8N1, MHZ_RX_PIN, MHZ_TX_PIN);
  mhz19.begin(mhzSerial);
  mhz19.autoCalibration(false);
  mhz19.setRange(5000);

  mhzStartMs = millis();
  Serial.println("MH-Z19C preheating 3 minutes...");

  // PMS UART start
  pmsSerial.begin(9600, SERIAL_8N1, PMS_RX_PIN, PMS_TX_PIN);
  pmsSerial.setTimeout(1000);
  Serial.println("PMS UART init OK");

  // Firebase init
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = FB_EMAIL;
  auth.user.password = FB_PASS;
  config.token_status_callback = tokenStatusCallback;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.println("✅ Sensors Ready!");
  Serial.println("IMPORTANT: External 5V power used -> External GND must connect to ESP32 GND.");
}

void loop() {
  Serial.println("✅ LOOP ALIVE");

  // WiFi reconnect (non-block)
  wifiReconnectOccasionally();

  // timestamp
  long ts = 0;
  if (WiFi.status() == WL_CONNECTED) {
    timeClient.update();
    ts = (long)timeClient.getEpochTime() * 1000;
  } else {
    ts = (long)millis(); // fallback
  }

  // Analog
  int rainValue = analogRead(RAIN_PIN);
  int uvValue = analogRead(UV_PIN);

  // DHT22
  float tempDHT = dht.readTemperature();
  float humDHT = dht.readHumidity();
  if (!isValidFloat(tempDHT)) tempDHT = 0;
  if (!isValidFloat(humDHT)) humDHT = 0;

  // AHT10
  sensors_event_t humidity, temp;
  aht.getEvent(&temp, &humidity);
  float ahtTemp = isValidFloat(temp.temperature) ? temp.temperature : 0;
  float ahtHum  = isValidFloat(humidity.relative_humidity) ? humidity.relative_humidity : 0;

  // SGP41
  uint16_t vocRaw = 0, noxRaw = 0;
  uint16_t error = 0;
  {
    float useHum = (ahtHum > 0 ? ahtHum : 50.0f);
    float useTemp = (ahtTemp != 0 ? ahtTemp : 25.0f);
    uint16_t rhTicks = (uint16_t)((useHum * 65535) / 100);
    uint16_t tTicks  = (uint16_t)(((useTemp + 45) * 65535) / 175);
    error = sgp41.measureRawSignals(rhTicks, tTicks, vocRaw, noxRaw);
  }

  // PMS (non-block)
  bool pmsOk = readPMSNonBlock(pmsData);
  if (!pmsOk) {
    pmsFailCount++;
    Serial.printf("PMS no frame (count=%lu)\n", (unsigned long)pmsFailCount);
    pmsData.PM_AE_UG_1_0  = 0;
    pmsData.PM_AE_UG_2_5  = 0;
    pmsData.PM_AE_UG_10_0 = 0;
  }

  // MH-Z19C
  int co2 = mhz19.getCO2();
  float co2Temp = mhz19.getTemperature();
  bool mhzHasData = (co2 > 0) && isValidFloat(co2Temp) && (co2Temp > -100);

  Serial.printf("[RAW] MH-Z19C CO2=%d ppm, Temp=%.1f\n", co2, co2Temp);

  bool mhzReady = (millis() - mhzStartMs >= MHZ_WARMUP_MS);
  if (!mhzReady) {
    Serial.println("MH-Z19C preheating... (not uploading CO2 yet)");
    co2 = 0;
    co2Temp = 0;
  } else {
    if (!mhzHasData) {
      mhzNoDataCount++;
      Serial.printf("MH-Z19C NO DATA (count=%lu)\n", (unsigned long)mhzNoDataCount);
      co2 = 0;
      co2Temp = 0;
    } else {
      if (co2 < 350 || co2 > 5000) {
        Serial.println("MH-Z19C out of range -> set 0");
        co2 = 0;
      }
      if (!isValidFloat(co2Temp)) co2Temp = 0;
    }
  }

  // JSON
  FirebaseJson json;
  json.set("rain", rainValue);
  json.set("uv", uvValue);

  json.set("dht22/temperature", tempDHT);
  json.set("dht22/humidity", humDHT);

  json.set("aht10/temperature", ahtTemp);
  json.set("aht10/humidity", ahtHum);

  json.set("sgp41/voc_raw", vocRaw);
  json.set("sgp41/nox_raw", noxRaw);
  json.set("sgp41/error", (int)error);

  json.set("mhz19c/co2", co2);
  json.set("mhz19c/temperature", co2Temp);
  json.set("mhz19c/no_data_count", (int)mhzNoDataCount);

  json.set("pms/pm1_0", pmsData.PM_AE_UG_1_0);
  json.set("pms/pm2_5", pmsData.PM_AE_UG_2_5);
  json.set("pms/pm10", pmsData.PM_AE_UG_10_0);
  json.set("pms/fail_count", (int)pmsFailCount);

  json.set("timestamp", ts);

  // Firebase upload only when ready + wifi
  if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
    bool ok1 = Firebase.RTDB.setJSON(&fbData, ("/sensors/" + deviceId + "/latest").c_str(), &json);
    if (!ok1) {
      Serial.print("setJSON failed: ");
      Serial.println(fbData.errorReason());
    } else {
      Serial.println("✅ latest setJSON OK");
    }

    bool ok2 = Firebase.RTDB.pushJSON(&fbData, ("/sensors/" + deviceId + "/history").c_str(), &json);
    if (!ok2) {
      Serial.print("pushJSON failed: ");
      Serial.println(fbData.errorReason());
    } else {
      Serial.println("✅ history pushJSON OK");
    }
  } else {
    if (WiFi.status() != WL_CONNECTED) Serial.println("No WiFi -> skip Firebase upload");
    else Serial.println("Firebase not ready -> skip upload");
  }

  // Print always
  Serial.println("---- SENSOR DATA ----");
  Serial.printf("WiFi: %s\n", (WiFi.status() == WL_CONNECTED ? "CONNECTED" : "NOT CONNECTED"));
  Serial.printf("Rain: %d  UV: %d\n", rainValue, uvValue);
  Serial.printf("DHT22 T/H: %.1f / %.1f\n", tempDHT, humDHT);
  Serial.printf("AHT10 T/H: %.1f / %.1f\n", ahtTemp, ahtHum);

  if (error == 0) Serial.printf("SGP41 VOC/NOx: %d / %d\n", vocRaw, noxRaw);
  else Serial.printf("SGP41 Error: %d\n", (int)error);

  Serial.printf("MH-Z19C CO2: %d ppm, Temp: %.1f\n", co2, co2Temp);
  Serial.printf("PMS PM1/2.5/10: %d / %d / %d\n\n",
                pmsData.PM_AE_UG_1_0,
                pmsData.PM_AE_UG_2_5,
                pmsData.PM_AE_UG_10_0);

  delay(5000);
}