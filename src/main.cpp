#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <time.h>
#include <DHTesp.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

const int SOIL_POT_PIN = 34;        // Potentiometer simulating soil moisture on GPIO34
const int ULTRASONIC_TRIG_PIN = 25; // Ultrasonic trigger on GPIO25
const int ULTRASONIC_ECHO_PIN = 26; // Ultrasonic echo on GPIO26
const int RELAY_PIN = 19;           // Relay on GPIO19
const int LED_PIN = 18;             // LED on GPIO18
const int SOLAR_POT_PIN = 36;       // Potentiometer simulating solar on GPIO36
const int DHT_PIN = 27;             // DHT22 data pin on GPIO27

// Wi-Fi settings for Wokwi
const char *ssid = "Wokwi-GUEST";
const char *password = "";

// Firebase configuration
#define API_KEY "AIzaSyCmvVA9K1kkzhMq8bMuJXVnNHI_c92_DW8"
#define DATABASE_URL "water-harvesting-b4520-default-rtdb.firebaseio.com"

// Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;
unsigned long dataMillis = 0;
int count = 0;

// DHT sensor object
DHTesp dht;

void initializeSerialAndPins()
{
  Serial.begin(115200);
  delay(1000);
  pinMode(ULTRASONIC_TRIG_PIN, OUTPUT);
  pinMode(ULTRASONIC_ECHO_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  // Initialize DHT sensor
  dht.setup(DHT_PIN, DHTesp::DHT22);
}

void connectToWiFi()
{
  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");
  unsigned long wifiTimeout = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiTimeout < 30000)
  {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("\nWiFi connection failed! Restarting...");
    ESP.restart();
  }

  Serial.println();
  Serial.print("Connected with IP: ");
  Serial.println(WiFi.localIP());
}

void syncTime()
{
  // Configure time with better error handling
  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");

  Serial.print("Waiting for NTP time sync: ");
  time_t nowSecs = time(nullptr);
  int retryCount = 0;
  while (nowSecs < 8 * 3600 * 2 && retryCount < 40)
  {
    delay(500);
    Serial.print(".");
    yield();
    nowSecs = time(nullptr);
    retryCount++;
  }

  if (retryCount >= 40)
  {
    Serial.println("\nNTP sync timeout - using fallback time");
    // Set a reasonable time for SSL to work
    struct tm timeinfo;
    timeinfo.tm_year = 125; // 2025 - 1900
    timeinfo.tm_mon = 7;    // August (0-11)
    timeinfo.tm_mday = 11;
    timeinfo.tm_hour = 12;
    timeinfo.tm_min = 0;
    timeinfo.tm_sec = 0;
    time_t t = mktime(&timeinfo);
    struct timeval tv = {.tv_sec = t};
    settimeofday(&tv, NULL);
  }
  else
  {
    Serial.println("\nTime synchronized successfully");
    struct tm timeinfo;
    gmtime_r(&nowSecs, &timeinfo);
    Serial.printf("Current time: %s", asctime(&timeinfo));
  }
}

void initializeFirebase()
{
  // Firebase configuration
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  // IMPORTANT: Add these for Wokwi compatibility
  config.signer.test_mode = true; // Disable certificate validation for testing
  config.cert.data = nullptr;     // Skip certificate

  // Increase timeouts for slow simulation
  config.timeout.serverResponse = 15 * 1000;
  config.timeout.socketConnection = 15 * 1000;
  config.timeout.sslHandshake = 20 * 1000;

  // Configure token callback
  config.token_status_callback = tokenStatusCallback;

  // Increase buffer sizes
  fbdo.setBSSLBufferSize(4096, 1024);

  // Try anonymous auth instead of signup (simpler for testing)
  Serial.println("Attempting Firebase authentication...");

  // Option 1: Try signup first
  if (!Firebase.signUp(&config, &auth, "", ""))
  {
    Serial.println("Sign up failed, trying anonymous mode");
    // Option 2: Use anonymous mode as fallback
    auth.user.email = "";
    auth.user.password = "";
    signupOK = true; // Proceed anyway for testing
  }
  else
  {
    Serial.println("Firebase sign up successful!");
    signupOK = true;
  }

  // Initialize Firebase
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Set stream timeouts
  if (!fbdo.httpConnected())
  {
    fbdo.setResponseSize(1024);
  }

  Serial.println("System ready to send data");

  // Test Firebase connection
  Serial.println("Testing Firebase connection...");
  if (Firebase.RTDB.setInt(&fbdo, "/test/connection", 1))
  {
    Serial.println("Firebase connection test successful!");
  }
  else
  {
    Serial.println("Firebase connection test failed:");
    Serial.println(fbdo.errorReason());
  }
}

int readSoilMoisture()
{
  int soilRaw = analogRead(SOIL_POT_PIN);
  return map(soilRaw, 0, 4095, 0, 100); // Map to 0-100%
}

int readWaterLevel()
{
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(ULTRASONIC_TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);
  long duration = pulseIn(ULTRASONIC_ECHO_PIN, HIGH);
  long distance = duration * 0.034 / 2; // Distance in cm
  Serial.print("Distance: ");
  Serial.println(distance);
  int waterLevel = 0;
  if (distance <= 100)
  {                                // Tank is 100cm deep
    waterLevel = (100 - distance); // % full (simplified)
    if (waterLevel < 0)
      waterLevel = 0;
    if (waterLevel > 100)
      waterLevel = 100;
  }
  return waterLevel;
}

float readSolarVoltage()
{
  int solarRaw = analogRead(SOLAR_POT_PIN);
  return solarRaw / 4095.0 * 3.3; // 0 to 3.3V
}

void readTempAndHumidity(float &temperature, float &humidity)
{
  TempAndHumidity newValues = dht.getTempAndHumidity();
  if (dht.getStatus() == 0)
  {
    temperature = newValues.temperature; // In Celsius
    humidity = newValues.humidity;       // In %
  }
  else
  {
    Serial.println("Failed to read from DHT sensor!");
    temperature = NAN; // Not a Number, indicates a failed reading
    humidity = NAN;
  }
}

bool shouldRunPump(int soilMoisture, int waterLevel, float solarVoltage, float temperature, float humidity)
{
  // Adjust moisture threshold based on temp/humidity
  int moistureThreshold = 30; // Default
  if (temperature > 30 || humidity < 40)
  {
    moistureThreshold = 25; // Water sooner in hot/dry conditions
  }

  if (solarVoltage > 2.5)
  { // Enough power
    if (soilMoisture < moistureThreshold && waterLevel > 10)
    { // Soil "dry", water available
      return true;
    }
  }
  return false;
}

void controlActuators(bool pumpShouldRun)
{
  digitalWrite(RELAY_PIN, pumpShouldRun ? HIGH : LOW);
  digitalWrite(LED_PIN, pumpShouldRun ? HIGH : LOW);
}

void printSensorReadings(int soilMoisture, int waterLevel, float solarVoltage, float temperature, float humidity, bool pumpShouldRun)
{
  Serial.printf("Soil: %d%%, Water: %d%%, Solar: %.2fV, Temp: %.1fC, Humid: %.1f%%, Pump: %s\n",
                soilMoisture, waterLevel, solarVoltage, temperature, humidity, pumpShouldRun ? "ON" : "OFF");
}

void sendDataToFirebase(int soilMoisture, int waterLevel, float solarVoltage, float temperature, float humidity, bool pumpShouldRun)
{
  if (Firebase.ready() && signupOK && (millis() - dataMillis > 30000 || dataMillis == 0))
  {
    dataMillis = millis();
    Serial.println("Sending data to Firebase...");

    // Create a JSON object to send all data at once
    FirebaseJson json;
    json.set("soilMoisture", soilMoisture);
    json.set("waterLevel", waterLevel);
    json.set("solarVoltage", solarVoltage);
    json.set("pumpStatus", pumpShouldRun ? 1 : 0);
    json.set("temperature", temperature);
    json.set("humidity", humidity);
    json.set("timestamp", (int)(millis() / 1000));
    json.set("count", count++);

    // Try to send data with error handling
    if (Firebase.RTDB.setJSON(&fbdo, "/sensorData", &json))
    {
      Serial.println("Data sent successfully");
    }
    else
    {
      Serial.println("Failed to send data to Firebase");
      Serial.print("Reason: ");
      Serial.println(fbdo.errorReason());

      // If connection is lost, try to reconnect
      if (fbdo.errorReason().indexOf("connection") >= 0)
      {
        Serial.println("Attempting to reconnect...");
        Firebase.reconnectWiFi(true);
        delay(5000);
      }
    }
  }
  else if (!Firebase.ready())
  {
    Serial.println("Firebase not ready - checking connection...");
    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("WiFi disconnected, reconnecting...");
      WiFi.reconnect();
    }
  }
}

void setup()
{
  initializeSerialAndPins();
  connectToWiFi();
  syncTime();
  initializeFirebase();
}

void loop()
{
  int soilMoisture = readSoilMoisture();
  int waterLevel = readWaterLevel();
  float solarVoltage = readSolarVoltage();
  float temperature, humidity;
  readTempAndHumidity(temperature, humidity);

  bool pumpShouldRun = shouldRunPump(soilMoisture, waterLevel, solarVoltage, temperature, humidity);
  controlActuators(pumpShouldRun);
  printSensorReadings(soilMoisture, waterLevel, solarVoltage, temperature, humidity, pumpShouldRun);
  sendDataToFirebase(soilMoisture, waterLevel, solarVoltage, temperature, humidity, pumpShouldRun);
  delay(5000); // Reduced delay for testing, increase for production
}
