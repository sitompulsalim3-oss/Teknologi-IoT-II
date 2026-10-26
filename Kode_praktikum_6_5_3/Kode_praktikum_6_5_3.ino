#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// Kredensial Jaringan Wi-Fi (Gunakan Hotspot Pribadi Anda)
const char* ssid = "WIFI MAKSUS 4G";
const char* password = "ahdkvavfah8128@$@";

// Konfigurasi Broker MQTT Publik
const char* mqttServer = "broker.hivemq.com";
const int mqttPort = 1883;
const char* clientId = "ESP8266_LabIoT_25782053_ClosedLoop";

// Definisi Topik Mandiri (Ganti 'nim_anda' dengan NIM Anda, misal: 'mhs123456')
const char* topicTelemetry = "polinela/labiot/25782053/telemetri";
const char* topicAlert     = "polinela/labiot/25782053/alert";
const char* topicStatus    = "polinela/labiot/25782053/status";
const char* topicCommand   = "polinela/labiot/25782053/kendali";

// Alokasi Pin Perangkat Keras
const byte dhtPin = 2; // Pin D4 (GPIO 2)
const byte ledPin = 5; // Pin D1 (GPIO 5)
const float thresholdSuhu = 30.0; // Ambang batas suhu darurat (°C)

#define DHTTYPE DHT11
DHT dht(dhtPin, DHTTYPE);

WiFiClient espClient;
PubSubClient client(espClient);

unsigned long lastTelemetryTime = 0;
const unsigned long telemetryInterval = 4000; // Siklus kirim telemetri setiap 4 detik

// Variabel State Latch untuk pencegahan spam pesan alert
bool alertSent = false;

// Deklarasi Fungsi
void reconnect();
void callback(char* topic, byte* payload, unsigned int length);

void setup() {
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW); // Status awal LED padam[cite: 1]

  dht.begin();

  // Koneksi Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Menghubungkan ke Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi Terkoneksi!");
  Serial.print("Alamat IP: ");
  Serial.println(WiFi.localIP());

  client.setServer(mqttServer, mqttPort);
  client.setCallback(callback);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop(); // Memelihara sesi TCP, keep-alive ping, dan antrean pesan masuk[cite: 1]

  unsigned long currentMillis = millis();
  if (currentMillis - lastTelemetryTime >= telemetryInterval) {
    lastTelemetryTime = currentMillis;

    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (isnan(t) || isnan(h)) {
      Serial.println("Peringatan: Gagal membaca sensor DHT!");
      return;
    }

    // Penentuan Kondisi Suhu Berdasarkan Rule Engine
    String kondisiSuhu = (t > thresholdSuhu) ? "PANAS" : "NORMAL";

    // 1. Publikasi Telemetri Rutin Berformat JSON
    StaticJsonDocument<256> docTelemetry;
    docTelemetry["node_id"] = clientId;
    docTelemetry["suhu"] = serialized(String(t, 2));
    docTelemetry["kelembapan"] = serialized(String(h, 2));
    docTelemetry["kondisi_suhu"] = kondisiSuhu;
    docTelemetry["status_led"] = digitalRead(ledPin);

    char bufferTelemetry[256];
    serializeJson(docTelemetry, bufferTelemetry);
    
    bool pubSuccess = client.publish(topicTelemetry, bufferTelemetry);
    if (pubSuccess) {
      Serial.printf("[TELEMETRI] Terkirim: %s\n", bufferTelemetry);
    } else {
      Serial.println("[ERROR] Gagal mengirim telemetri!");
    }

    // 2. Logika Rule Engine & Dedicated Alert Topic
    if (t > thresholdSuhu) {
      // Nyalakan LED otomatis sebagai indikator bahaya
      digitalWrite(ledPin, HIGH);

      // Kirim alert hanya saat pertama kali transisi melewati ambang batas (State Latch)
      if (!alertSent) {
        StaticJsonDocument<256> docAlert;
        docAlert["status"] = "BAHAYA";
        docAlert["parameter"] = "OVERHEAT";
        docAlert["nilai_suhu"] = serialized(String(t, 2));

        char bufferAlert[256];
        serializeJson(docAlert, bufferAlert);

        if (client.publish(topicAlert, bufferAlert, true)) {
          Serial.println("--> [ALERT BAHAYA] Peringatan Overheat terkirim ke broker!");
          alertSent = true; // Mengunci agar tidak terjadi spamming
        }
      }
    } else {
      // Reset state alert jika suhu kembali normal
      if (alertSent) {
        Serial.println("--> [INFO] Suhu kembali normal. Status bahaya dipulihkan.");
        alertSent = false;
      }
    }
  }
}

// Fungsi Callback untuk Menangani Perintah Kendali Masuk (Manual Override)
void callback(char* topic, byte* payload, unsigned int length) {
  char incoming[length + 1];
  memcpy(incoming, payload, length);
  incoming[length] = '\0';

  Serial.println("----------------------------------------");
  Serial.printf("[KONTROL DITERIMA] Topik: %s | Payload: %s\n", topic, incoming);

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, incoming);
  if (err) {
    Serial.println("Gagal parsing JSON kendali!");
    return;
  }

  // Kendali manual hanya diizinkan jika sistem tidak dalam kondisi darurat overheat aktif
  float currentTemp = dht.readTemperature();
  if (currentTemp > thresholdSuhu) {
    Serial.println("--> [ABAIKAN KENDALI] Sistem terkunci dalam mode BAHAYA (Overheat)!");
    Serial.println("----------------------------------------");
    return;
  }

  if (doc.containsKey("led")) {
    int val = doc["led"];
    digitalWrite(ledPin, val == 1 ? HIGH : LOW);
    Serial.printf("--> [MANUAL OVERRIDE] LED diubah menjadi: %s\n", val == 1 ? "MENYALA" : "PADAM");
  }
  Serial.println("----------------------------------------");
}

// Fungsi Rekoneksi Dilengkapi Last Will and Testament (LWT)
void reconnect() {
  while (!client.connected()) {
    Serial.print("Menghubungkan ke MQTT Broker...");

    // Konfigurasi Payload Last Will and Testament (LWT)
    StaticJsonDocument<128> lwtDoc;
    lwtDoc["status"] = "OFFLINE";
    lwtDoc["keterangan"] = "Perangkat Terputus Tak Terduga";
    char lwtBuffer[128];
    serializeJson(lwtDoc, lwtBuffer);

    // Hubungkan dengan parameter LWT (Topic, QoS, Retain, Message)[cite: 1]
    if (client.connect(clientId, topicStatus, 1, true, lwtBuffer)) {
      Serial.println(" Sukses Terhubung!");

      // Publikasikan status ONLINE sesaat setelah berhasil terhubung[cite: 1]
      StaticJsonDocument<128> onlineDoc;
      onlineDoc["status"] = "ONLINE";
      onlineDoc["keterangan"] = "Perangkat Beroperasi Normal";
      char onlineBuffer[128];
      serializeJson(onlineDoc, onlineBuffer);
      client.publish(topicStatus, onlineBuffer, true);

      // Daftar subskripsi topik kendali perintah[cite: 1]
      client.subscribe(topicCommand);
      Serial.printf("Subskripsi Topik Kendali: %s\n", topicCommand);
    } else {
      Serial.printf(" Gagal (rc=%d). Coba lagi dalam 4 detik...\n", client.state());
      delay(4000);
    }
  }
}