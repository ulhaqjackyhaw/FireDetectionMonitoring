#include <WiFi.h>
#include <Firebase_ESP_Client.h> // Pastikan library ini sudah terinstall
#include <DHT.h>                 // Untuk sensor DHT11/22
#include "time.h"                // Untuk sinkronisasi waktu NTP

// --- GANTI DENGAN KONFIGURASI ANDA JIKA BERBEDA ---
#define WIFI_SSID "xrp"
#define WIFI_PASSWORD "mstahulhaq"

// PENTING: Format DATABASE_URL sudah diperbaiki
#define API_KEY "AIzaSyCKerVVynvh2puu7Cqe0wZQ5HNChogCH6c"      // API Key Firebase Anda
#define DATABASE_URL "tubes-e7c2f-default-rtdb.firebaseio.com" // TANPA https:// dan TANPA / di akhir

// USER_EMAIL dan USER_PASSWORD tidak diperlukan jika aturan RTDB public dan menggunakan nullptr untuk auth
#define USER_EMAIL "ulhaq@gmail.com"  // Sebaiknya dikomentari jika menggunakan auth nullptr
#define USER_PASSWORD "ulhaq123" // Sebaiknya dikomentari jika menggunakan auth nullptr
// --- AKHIR KONFIGURASI ---

// --- PIN DEFINITIONS ---
#define MQ2_PIN       34  // Pin ADC untuk MQ-2
#define IR_PIN        26  // Pin digital untuk Flame Sensor (IR)
#define DHT_PIN       4  // Pin digital untuk DHT11/22
#define BUZZER_PIN    25  // Pin digital untuk Buzzer
#define WATERPUMP_PIN 23  // Pin digital untuk Water Pump (misalnya GPIO23) <--- TAMBAHKAN INI

#define DHTTYPE DHT11    // Tipe sensor DHT (DHT11 atau DHT22)
DHT dht(DHT_PIN, DHTTYPE);

// Objek Firebase
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// Variabel global untuk status fuzzy sebelumnya
bool previousAlarmActiveFuzzy = false;

// === BAGIAN FUZZY LOGIC (SAMA SEPERTI SEBELUMNYA, SUDAH CUKUP BAIK SEBAGAI DASAR) ===
struct FuzzyMembership {
  float rendah;
  float sedang;
  float tinggi;
};
float fuzzyDangerLevelOutput = 0.0;
String fuzzyStatusOutput = "AMAN";

float trimf(float x, float a, float b, float c) {
  float val1 = (x - a) / (b - a);
  float val2 = (c - x) / (c - b);
  return max(0.0f, min(val1, val2));
}

float trapmf(float x, float a, float b, float c, float d) {
  float val1 = (x - a) / (b - a);
  float val2 = (d - x) / (d - c);
  return max(0.0f, min(min(val1, 1.0f), val2));
}

void calculateFuzzyLogic(float temp, float hum, int gasRaw, bool irFireDetected) {
  FuzzyMembership tempMembership, humMembership, gasMembership;
  tempMembership.rendah = trapmf(temp, 0, 0, 20, 26);
  tempMembership.sedang = trimf(temp, 24, 30, 36);
  tempMembership.tinggi = trapmf(temp, 34, 40, 60, 60);
  humMembership.rendah = trapmf(hum, 0, 0, 30, 45);
  humMembership.sedang = trimf(hum, 40, 55, 70);
  humMembership.tinggi = trapmf(hum, 65, 80, 100, 100);
  gasMembership.rendah = trapmf(gasRaw, 0, 0, 300, 500);
  gasMembership.sedang = trimf(gasRaw, 450, 700, 950);
  gasMembership.tinggi = trapmf(gasRaw, 900, 1200, 4095, 4095);

  float amanStrength = 0.0, waspadaStrength = 0.0, bahayaStrength = 0.0;
  amanStrength = max(amanStrength, min(tempMembership.sedang, gasMembership.rendah));
  waspadaStrength = max(waspadaStrength, min(tempMembership.tinggi, gasMembership.sedang));
  bahayaStrength = max(bahayaStrength, min(tempMembership.tinggi, gasMembership.tinggi));
  bahayaStrength = max(bahayaStrength, gasMembership.tinggi);
  if (temp > 45.0) {
      bahayaStrength = max(bahayaStrength, tempMembership.tinggi * 0.8f);
  }
  waspadaStrength = max(waspadaStrength, min(humMembership.rendah, tempMembership.tinggi));
  if (irFireDetected) {
    bahayaStrength = 1.0f; waspadaStrength = 0.0f; amanStrength = 0.0f;
  }

  float amanLevel = 15.0, waspadaLevel = 50.0, bahayaLevel = 85.0;
  float numerator = (amanStrength * amanLevel) + (waspadaStrength * waspadaLevel) + (bahayaStrength * bahayaLevel);
  float denominator = amanStrength + waspadaStrength + bahayaStrength;

  fuzzyDangerLevelOutput = (denominator == 0) ? 0 : (numerator / denominator);

  if (fuzzyDangerLevelOutput <= 30) fuzzyStatusOutput = "AMAN";
  else if (fuzzyDangerLevelOutput <= 70) fuzzyStatusOutput = "WASPADA";
  else fuzzyStatusOutput = "BAHAYA";

  Serial.println("--- Fuzzy Logic Output ---");
  Serial.print("Aman Strength: "); Serial.print(amanStrength);
  Serial.print(" | Waspada Strength: "); Serial.print(waspadaStrength);
  Serial.print(" | Bahaya Strength: "); Serial.println(bahayaStrength);
  Serial.print("Fuzzy Danger Level: "); Serial.println(fuzzyDangerLevelOutput);
  Serial.print("Fuzzy Status: "); Serial.println(fuzzyStatusOutput);
  Serial.println("---------------------------");
}
// === AKHIR BAGIAN FUZZY LOGIC ===

void connectToWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  int wifi_retry_count = 0;
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
    wifi_retry_count++;
    if (wifi_retry_count > 20) {
        Serial.println("\nFailed to connect to WiFi. Restarting...");
        ESP.restart();
    }
  }
  Serial.println("\nConnected to Wi-Fi");
  Serial.print("IP Address: "); Serial.println(WiFi.localIP());
}

void initTime() {
  configTime(25200, 0, "pool.ntp.org", "time.nist.gov"); // UTC+7 (WIB)
  Serial.print("Waiting for NTP time sync: ");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    Serial.print(".");
    delay(1000);
  }
  Serial.println(" Synced!");
}

String getTimeNow() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 5000)) { // Tambahkan timeout 5000 ms
    Serial.println("Failed to obtain time");
    return "N/A";
  }
  char timeStr[25];
  strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeStr);
}

void setup() {
  Serial.begin(115200);
  while (!Serial); // Tunggu serial siap (untuk beberapa board seperti ESP32-S3)
  Serial.println("\nStarting Fire Detection System...");

  dht.begin();
  pinMode(MQ2_PIN, INPUT);
  pinMode(IR_PIN, INPUT_PULLUP); // Sensor IR biasanya LOW saat deteksi
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(WATERPUMP_PIN, OUTPUT); // <--- INISIALISASI PIN POMPA
  digitalWrite(BUZZER_PIN, LOW);    // Buzzer mati (asumsi aktif HIGH)
  digitalWrite(WATERPUMP_PIN, LOW); 

  connectToWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    initTime();

    config.api_key = API_KEY;
    config.database_url = DATABASE_URL;

    // PENTING: Karena aturan RTDB Anda .read:true, .write:true, maka gunakan nullptr untuk auth:
    Firebase.begin(&config, nullptr);
    Serial.println("Firebase initialized with public access (nullptr auth).");

    // // Baris di bawah ini (autentikasi email/pass) seharusnya TIDAK DIPAKAI jika sudah pakai nullptr di atas
    // // dan jika aturan RTDB adalah public. Jika aturan RTDB memerlukan autentikasi,
    // // maka uncomment ini dan comment Firebase.begin(&config, nullptr); di atas.
    // // Pastikan USER_EMAIL dan USER_PASSWORD sudah benar.
    auth.user.email = USER_EMAIL;
    auth.user.password = USER_PASSWORD;
    Firebase.begin(&config, &auth);
    if(Firebase.signUp(&config, &auth, USER_EMAIL, USER_PASSWORD)){
      Serial.println("Firebase signed up successfully.");
    } else {
      Serial.printf("Firebase sign up failed: %s\n", config.signer.signupError.message.c_str());
    }
    // // Setelah sign up, atau jika user sudah ada, gunakan signIn
    // // Firebase.signIn(&config, &auth, USER_EMAIL, USER_PASSWORD); // atau Firebase.signInWithCustomToken(...)

    Firebase.reconnectWiFi(true);
  } else {
    Serial.println("WiFi connection failed. Cannot initialize Firebase.");
  }
  Serial.println("🔥 Fire Detection with Fuzzy Logic & Firebase Initialized");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Attempting to reconnect...");
    connectToWiFi();
    if (WiFi.status() != WL_CONNECTED) {
        delay(5000); return; // Tunggu sebelum retry lagi jika gagal
    }
  }

  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  int mq2RawValue = analogRead(MQ2_PIN);
  bool irDetectedFire = (digitalRead(IR_PIN) == LOW); // IR sensor LOW saat api terdeteksi

  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("Failed to read from DHT sensor!");
    temperature = -1.0; humidity = -1.0; // Nilai error, bisa disesuaikan
  }

  calculateFuzzyLogic(temperature, humidity, mq2RawValue, irDetectedFire);

  bool activateAlarm = false;
  if (fuzzyStatusOutput == "BAHAYA") {
    activateAlarm = true;
  } else if (fuzzyStatusOutput == "WASPADA" && fuzzyDangerLevelOutput > 60.0) { // Lebih spesifik
    activateAlarm = true;
  }
  // Logika tambahan: jika IR mendeteksi api DAN suhu sangat tinggi, pasti alarm
  // Kondisi ini bisa jadi sudah tercakup oleh fuzzy "BAHAYA" jika IR langsung trigger bahaya=1.0
  if (irDetectedFire && temperature > 45.0) {
      activateAlarm = true;
  }


  if (activateAlarm) {
    digitalWrite(BUZZER_PIN, HIGH);    // Nyalakan buzzer
    digitalWrite(WATERPUMP_PIN, LOW); // Nyalakan pompa air <--- KONTROL POMPA
  } else {
    digitalWrite(BUZZER_PIN, LOW);     // Matikan buzzer
    digitalWrite(WATERPUMP_PIN, HIGH);  // Matikan pompa air <--- KONTROL POMPA
  }
  
  // Notifikasi perubahan status alarm (opsional, tapi bagus untuk debugging)
  if (activateAlarm != previousAlarmActiveFuzzy) {
    if (activateAlarm) Serial.println("🔥 FUZZY ALARM ACTIVATED! 🔥 (Buzzer & Pompa ON)");
    else Serial.println("✅ FUZZY ALARM DEACTIVATED. (Buzzer & Pompa OFF)");
    previousAlarmActiveFuzzy = activateAlarm;
  }

  String currentTime = getTimeNow();
  Serial.println("\n=== Sensor Data & Fuzzy ===");
  Serial.print("Timestamp: "); Serial.println(currentTime);
  Serial.print("Temperature: "); Serial.print(temperature, 1); Serial.println(" C");
  Serial.print("Humidity: "); Serial.print(humidity, 1); Serial.println(" %");
  Serial.print("Gas (MQ2 Raw): "); Serial.println(mq2RawValue);
  Serial.print("IR Sensor: "); Serial.println(irDetectedFire ? "API TERDETEKSI" : "Aman");
  Serial.print("Fuzzy Status: "); Serial.println(fuzzyStatusOutput);
  Serial.print("Fuzzy Danger Level: "); Serial.println(fuzzyDangerLevelOutput, 2);
  Serial.print("Alarm Active: "); Serial.println(activateAlarm ? "YA" : "TIDAK");
  Serial.print("Water Pump Active: "); Serial.println(digitalRead(WATERPUMP_PIN) == HIGH ? "TIDAK" : "YA"); // <--- STATUS POMPA
  Serial.println("===========================");

  if (Firebase.ready()) {
    String path = "/fire_detection"; // Mengirim ke path utama, atau bisa dibuat path unik per pembacaan
    // String path = "/fire_detection/log/" + String(millis()); // contoh path unik (kurang disarankan untuk data terbaru)
    
    FirebaseJson json;
    json.set("temperature", String(temperature, 1));
    json.set("humidity", String(humidity, 1));
    json.set("gas", mq2RawValue); // ganti "gas" menjadi "gas_raw" agar lebih jelas
    json.set("ir_detected_fire", irDetectedFire);
    json.set("fuzzy_status", fuzzyStatusOutput);
    json.set("fuzzy_danger_level", String(fuzzyDangerLevelOutput, 2));
    json.set("alarm_active", activateAlarm);
    // json.set("waterpump_active", digitalRead(WATERPUMP_PIN) == HIGH); // <--- STATUS POMPA KE FIREBASE
    json.set("timestamp", currentTime);

    // Menggunakan setJSON untuk mengirim data sebagai satu objek JSON
    Serial.print("Sending data to Firebase: "); Serial.println(path);
    if (Firebase.RTDB.setJSON(&fbdo, path.c_str(), &json)) { // Path harus const char*
      Serial.println("-> Data sent successfully!");
    } else {
      Serial.print("-> Firebase send FAILED: ");
      Serial.println(fbdo.errorReason());
    }
  } else {
    Serial.println("Firebase not ready. Skipping data send.");
  }

  delay(500); // Kurangi delay jika ingin respons lebih cepat, tapi perhatikan batasan Firebase. 10000ms (10 detik) adalah interval yang aman.
}