#include <Arduino.h>
#include <ESP32Servo.h>

// ---- Pin ----
#define PIN_TRIG_TANGAN 5
#define PIN_ECHO_TANGAN 18
#define PIN_TRIG_ISI    19
#define PIN_ECHO_ISI    21
#define PIN_SERVO       13
#define PIN_LED_PENUH   2
#define PIN_BUZZER      15

// ---- Parameter ----
const float JARAK_TANGAN_CM        = 20.0;   // tangan dekat -> tutup terbuka
const float TINGGI_TONG_CM         = 30.0;   // jarak sensor ke dasar tong (kosong)
const float AMBANG_PENUH_PERSEN    = 80.0;   // >= 80% dianggap penuh
const unsigned long INTERVAL_TANGAN_MS    = 200;
const unsigned long INTERVAL_TELEMETRI_MS = 2000;
const unsigned long TUTUP_SETELAH_MS      = 3000;
const unsigned long INTERVAL_BEEP_MS      = 500;

Servo tutup;
unsigned long waktuTanganTerakhirCek = 0;
unsigned long waktuTanganTerlihat    = 0;
unsigned long waktuTelemetriTerakhir = 0;
unsigned long waktuBeepTerakhir      = 0;
unsigned long nomorPaket = 0;
bool terbuka  = false;
bool penuh    = false;
bool beepOn   = false;
float persenIsi = 0;

float bacaJarakCm(int trig, int echo) {
  digitalWrite(trig, LOW);
  delayMicroseconds(2);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);
  long durasi = pulseIn(echo, HIGH, 30000);  // timeout 30 ms
  if (durasi == 0) return -1;
  return durasi * 0.0343 / 2;
}

void kirimTelemetri(float jarakIsi) {
  Serial.printf(
    "{\"device_id\":\"cleanbin-01\",\"seq\":%lu,\"uptime_ms\":%lu,"
    "\"fill_cm\":%.1f,\"fill_pct\":%.1f,\"lid\":\"%s\",\"full\":%s}\n",
    ++nomorPaket, millis(), jarakIsi, persenIsi,
    terbuka ? "open" : "closed",
    penuh ? "true" : "false"
  );
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG_TANGAN, OUTPUT);
  pinMode(PIN_ECHO_TANGAN, INPUT);
  pinMode(PIN_TRIG_ISI, OUTPUT);
  pinMode(PIN_ECHO_ISI, INPUT);
  pinMode(PIN_LED_PENUH, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  tutup.attach(PIN_SERVO);
  tutup.write(0);
  Serial.println("{\"event\":\"boot\",\"msg\":\"CleanBin aktif\"}");
}

void loop() {
  unsigned long sekarang = millis();

  // 1. Sensor tangan -> buka/tutup tutup (non-blocking)
  if (sekarang - waktuTanganTerakhirCek >= INTERVAL_TANGAN_MS) {
    waktuTanganTerakhirCek = sekarang;
    float jarakTangan = bacaJarakCm(PIN_TRIG_TANGAN, PIN_ECHO_TANGAN);
    if (jarakTangan > 0 && jarakTangan < JARAK_TANGAN_CM) {
      waktuTanganTerlihat = sekarang;
      if (!terbuka) {
        tutup.write(90);
        terbuka = true;
        Serial.println("{\"event\":\"lid\",\"state\":\"open\"}");
      }
    }
  }
  if (terbuka && sekarang - waktuTanganTerlihat >= TUTUP_SETELAH_MS) {
    tutup.write(0);
    terbuka = false;
    Serial.println("{\"event\":\"lid\",\"state\":\"closed\"}");
  }

  // 2. Sensor isi -> hitung persen, kirim telemetri berkala
  if (sekarang - waktuTelemetriTerakhir >= INTERVAL_TELEMETRI_MS) {
    waktuTelemetriTerakhir = sekarang;
    float jarakIsi = bacaJarakCm(PIN_TRIG_ISI, PIN_ECHO_ISI);
    if (jarakIsi < 0) {
      Serial.println("{\"error\":\"Gagal membaca sensor isi\"}");
    } else {
      persenIsi = constrain((TINGGI_TONG_CM - jarakIsi) / TINGGI_TONG_CM * 100.0, 0, 100);
      bool penuhBaru = (persenIsi >= AMBANG_PENUH_PERSEN);
      if (penuhBaru != penuh) {
        penuh = penuhBaru;
        Serial.printf("{\"event\":\"full_status\",\"full\":%s}\n", penuh ? "true" : "false");
      }
      digitalWrite(PIN_LED_PENUH, penuh ? HIGH : LOW);
      kirimTelemetri(jarakIsi);
    }
  }

  // 3. Buzzer berbunyi putus-putus saat penuh (non-blocking)
  if (penuh) {
    if (sekarang - waktuBeepTerakhir >= INTERVAL_BEEP_MS) {
      waktuBeepTerakhir = sekarang;
      beepOn = !beepOn;
      if (beepOn) tone(PIN_BUZZER, 2000);
      else noTone(PIN_BUZZER);
    }
  } else if (beepOn) {
    noTone(PIN_BUZZER);
    beepOn = false;
  }
}