// #include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <time.h> //Provide the token generation process info.
#include "addons/TokenHelper.h"
// Provide the RTDB payload printing info and other helper functions.
#include "addons/RTDBHelper.h"      // Pin definitions
const int SOIL_POT_PIN = 34;        // Potentiometer simulating soil moisture on GPIO34
const int ULTRASONIC_TRIG_PIN = 25; // Ultrasonic trigger on GPIO25
const int ULTRASONIC_ECHO_PIN = 26; // Ultrasonic echo on GPIO26
const int RELAY_PIN = 19;           // Relay on GPIO19
const int LED_PIN = 18;             // LED on GPIO18
const int SOLAR_POT_PIN = 36;       // Potentiometer simulating solar on GPIO36// Wi-Fi settings for Wokwi
const char *ssid = "Wokwi-GUEST";
const char *password = ""; // Firebase configuration
#define API_KEY "AIzaSyCmvVA9K1kkzhMq8bMuJXVnNHI_c92_DW8"
#define DATABASE_URL "https://water-harvesting-b4520-default-rtdb.firebaseio.com/" // Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;
unsigned long dataMillis = 0;
int count = 0;
void setup()
{
  Serial.begin(115200);
  delay(1000); // Set up pins
  pinMode(ULTRASONIC_TRIG_PIN, OUTPUT);
  pinMode(ULTRASONIC_ECHO_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // Start with pump off
  digitalWrite(LED_PIN, LOW);   // Start with LED off  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected with IP: ");
  Serial.println(WiFi.localIP()); // Configure time (important for SSL certificates)
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Waiting for NTP time sync: ");
  time_t nowSecs = time(nullptr);
  while (nowSecs < 8 * 3600 * 2)
  {
    delay(500);
    Serial.print(".");
    yield();
    nowSecs = time(nullptr);
  }
  Serial.println();
  struct tm timeinfo;
  gmtime_r(&nowSecs, &timeinfo);
  Serial.printf("Current time: %s", asctime(&timeinfo)); /* Assign the api key (required) */
  config.api_key = API_KEY;                              /* Assign the RTDB URL (required) */
  config.database_url = DATABASE_URL;                    /* Sign up with retry */
  while (!Firebase.signUp(&config, &auth, "", ""))
  {
    Serial.println("Firebase sign up failed: ");
    Serial.println(String(config.signer.signupError.message.c_str()));
    Serial.println("Retrying in 2 seconds...");
    delay(2000);
  }
  Serial.println("Firebase sign up successful!");
  signupOK = true;                                    /* Assign the callback function for the long running token generation task */
  config.token_status_callback = tokenStatusCallback; // see addons/TokenHelper.h  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Serial.println("System ready to send data");
}
void loop()
{
  // Read potentiometer (simulating soil moisture, 0% dry, 100% wet)
  int soilRaw = analogRead(SOIL_POT_PIN);
  int soilMoisture = map(soilRaw, 0, 4095, 0, 100); // Map to 0-100%  // Read water level with ultrasonic sensor
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(ULTRASONIC_TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);
  long duration = pulseIn(ULTRASONIC_ECHO_PIN, HIGH);
  long distance = duration * 0.034 / 2; // Distance in cm  Serial.print("Distance: ");
  Serial.println(distance);
  int waterLevel = 0;
  if (distance <= 100)
  {                                // Tank is 100cm deep
    waterLevel = (100 - distance); // % full (simplified)
    if (waterLevel < 0)
      waterLevel = 0;
    if (waterLevel > 100)
      waterLevel = 100;
  } // Read solar voltage (simulated with potentiometer)
  int solarRaw = analogRead(SOLAR_POT_PIN);
  float solarVoltage = solarRaw / 4095.0 * 3.3; // 0 to 3.3V  // Decide if pump should run
  bool pumpShouldRun = false;
  if (solarVoltage > 2.5)
  { // Enough power
    if (soilMoisture < 30 && waterLevel > 10)
    { // Soil "dry", water available
      pumpShouldRun = true;
    }
  }
  digitalWrite(RELAY_PIN, pumpShouldRun ? HIGH : LOW);
  digitalWrite(LED_PIN, pumpShouldRun ? HIGH : LOW); // Print sensor readings
  Serial.printf("Soil Moisture: %d%%, Water Level: %d%%, Solar Voltage: %.2fV, Pump: %s\n",
                soilMoisture, waterLevel, solarVoltage, pumpShouldRun ? "ON" : "OFF"); // Send data to Firebase (with error handling and retry logic)
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
  } // Wait before next reading
  delay(5000); // Reduced delay for testing, increase for production
}
