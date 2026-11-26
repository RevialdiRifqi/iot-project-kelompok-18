#include <Arduino.h>
#include <DHT.h>
#include <ArduinoJson.h>

// --- Pin aman ESP32 ---
#define PIN_DHT 4
#define TIPE_DHT DHT22
#define PIN_SOIL_ADC1 34
#define PIN_TOMBOL_MODE 15
#define PIN_RELAY_POMPA 26
#define PIN_PWM_KIPAS 18
#define PIN_LED_FAULT 2          // [C6] LED merah peringatan

// 0 = ambang tunggal (chattering), 1 = histeresis
#define GUNAKAN_HISTERESIS 1

// Kalibrasi dua titik sensor tanah (mV)
const int MV_TANAH_KERING = 2650;
const int MV_TANAH_BASAH = 1100;

const float AMBANG_POMPA_ON = 35.0f;
const float AMBANG_POMPA_OFF = 60.0f;
const float AMBANG_TUNGGAL = 45.0f;

// PWM LEDC
const int PWM_FREQ_HZ = 5000;
const int PWM_RES_BIT = 8;
const int PWM_KANAL = 0;

// Moving average N = 5
const int UKURAN_FILTER = 5;
uint32_t bufferMv[UKURAN_FILTER] = {0};
int indeksFilter = 0;
bool filterPenuh = false;

DHT dht(PIN_DHT, TIPE_DHT);
unsigned long waktuTelemetriTerakhir = 0;
unsigned long waktuDebounceTombol = 0;
const unsigned long INTERVAL_BACA_MS = 1000;

bool statusPompa = false;
bool modeOverridePaksa = false;
bool tombolSebelumnya = HIGH;
unsigned long seqPaket = 0;
unsigned long hitungTransisiRelai = 0;

// [C6] Pengaman waktu jalan maksimum pompa
const unsigned long BATAS_JALAN_POMPA_MS = 15000;  // 15 detik
const float KENAIKAN_MIN_PERSEN = 2.0f;            // dianggap "naik" jika >= 2 poin
bool faultDryRun = false;
unsigned long waktuMulaiPompa = 0;
float soilAwalPompa = 0;
unsigned long waktuKedip = 0;
bool ledFaultOn = false;

void setupPwmKipas() {
#if defined(ESP_ARDUINO_VERSION) && (ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0))
  ledcAttach(PIN_PWM_KIPAS, PWM_FREQ_HZ, PWM_RES_BIT);
#else
  ledcSetup(PWM_KANAL, PWM_FREQ_HZ, PWM_RES_BIT);
  ledcAttachPin(PIN_PWM_KIPAS, PWM_KANAL);
#endif
}

void tulisPwmKipas(uint8_t duty) {
#if defined(ESP_ARDUINO_VERSION) && (ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0))
  ledcWrite(PIN_PWM_KIPAS, duty);
#else
  ledcWrite(PWM_KANAL, duty);
#endif
}

uint32_t bacaSoilMilliVoltsTerfilter() {
  uint32_t mvMentah = analogReadMilliVolts(PIN_SOIL_ADC1);
  bufferMv[indeksFilter] = mvMentah;
  indeksFilter = (indeksFilter + 1) % UKURAN_FILTER;
  if (indeksFilter == 0) filterPenuh = true;

  int jumlahSampel = filterPenuh ? UKURAN_FILTER : indeksFilter;
  uint64_t total = 0;
  for (int i = 0; i < jumlahSampel; i++) total += bufferMv[i];
  return (uint32_t)(total / jumlahSampel);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_SOIL_ADC1, INPUT);
  pinMode(PIN_TOMBOL_MODE, INPUT_PULLUP);
  pinMode(PIN_RELAY_POMPA, OUTPUT);
  pinMode(PIN_LED_FAULT, OUTPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_SOIL_ADC1, ADC_11db);

  setupPwmKipas();
  dht.begin();
  Serial.println("{\"event\":\"boot\",\"lab\":\"Lab-01 Greenhouse Kopi UNIB\"}");
}

void loop() {
  unsigned long sekarang = millis();

  // 1. Tombol override manual (non-blocking, debounce 60 ms)
  bool bacaTombol = digitalRead(PIN_TOMBOL_MODE);
  if (bacaTombol == LOW && tombolSebelumnya == HIGH && (sekarang - waktuDebounceTombol > 60)) {
    waktuDebounceTombol = sekarang;
    modeOverridePaksa = !modeOverridePaksa;
    Serial.printf("{\"event\":\"mode_change\",\"manual_override\":%s}\n",
                  modeOverridePaksa ? "true" : "false");
  }
  tombolSebelumnya = bacaTombol;

  // [C6] Kedipan LED peringatan saat fault (non-blocking, 250 ms)
  if (faultDryRun) {
    if (sekarang - waktuKedip >= 250) {
      waktuKedip = sekarang;
      ledFaultOn = !ledFaultOn;
      digitalWrite(PIN_LED_FAULT, ledFaultOn ? HIGH : LOW);
    }
  } else if (ledFaultOn) {
    ledFaultOn = false;
    digitalWrite(PIN_LED_FAULT, LOW);
  }

  // 2. Akuisisi sensor + kendali aktuator (1000 ms)
  if (sekarang - waktuTelemetriTerakhir >= INTERVAL_BACA_MS) {
    waktuTelemetriTerakhir = sekarang;

    int adcRaw = analogRead(PIN_SOIL_ADC1);
    uint32_t soilMv = bacaSoilMilliVoltsTerfilter();
    float soilPct = (float)(MV_TANAH_KERING - (int)soilMv) * 100.0f
                    / (float)(MV_TANAH_KERING - MV_TANAH_BASAH);
    soilPct = constrain(soilPct, 0.0f, 100.0f);

    float suhuUdara = dht.readTemperature();
    float lembapUdara = dht.readHumidity();
    if (isnan(suhuUdara) || isnan(lembapUdara)) {
      Serial.println("{\"error\":\"Pembacaan DHT22 gagal\"}");
      return;
    }

    // Kendali pompa: fault > override > histeresis/ambang tunggal
    bool pompaSebelum = statusPompa;
    if (faultDryRun) {
      statusPompa = false;                       // [C6] fault mengunci pompa MATI
      if (soilPct >= AMBANG_POMPA_OFF) {         // [C6] pulih setelah tanah basah lagi
        faultDryRun = false;
        Serial.println("{\"event\":\"fault_cleared\",\"code\":\"DRY_RUN_PROTECTION\"}");
      }
    } else if (modeOverridePaksa) {
      statusPompa = true;
    } else {
#if GUNAKAN_HISTERESIS == 1
      if (soilPct < AMBANG_POMPA_ON) statusPompa = true;
      else if (soilPct >= AMBANG_POMPA_OFF) statusPompa = false;
#else
      statusPompa = (soilPct < AMBANG_TUNGGAL);
#endif
    }

    // [C6] Catat awal pompa menyala, lalu cek tiap jendela 15 detik
    if (statusPompa && !pompaSebelum) {
      waktuMulaiPompa = sekarang;
      soilAwalPompa = soilPct;
    }
    if (statusPompa && (sekarang - waktuMulaiPompa >= BATAS_JALAN_POMPA_MS)) {
      if (soilPct - soilAwalPompa < KENAIKAN_MIN_PERSEN) {
        faultDryRun = true;
        statusPompa = false;
        Serial.println("{\"event\":\"fault\",\"code\":\"DRY_RUN_PROTECTION\"}");
      } else {
        waktuMulaiPompa = sekarang;              // lengas naik, mulai jendela baru
        soilAwalPompa = soilPct;
      }
    }

    if (statusPompa != pompaSebelum) hitungTransisiRelai++;
    digitalWrite(PIN_RELAY_POMPA, statusPompa ? HIGH : LOW);

    // PWM kipas proporsional suhu (28 C -> 0%, 38 C -> 100%)
    int nilaiDuty = map((long)(suhuUdara * 10.0f), 280, 380, 0, 255);
    nilaiDuty = constrain(nilaiDuty, 0, 255);
    tulisPwmKipas((uint8_t)nilaiDuty);
    float fanPct = (nilaiDuty * 100.0f) / 255.0f;

    // Telemetri JSON
    JsonDocument doc;
    doc["device_id"] = "esp32-kopi-gh01";
    doc["seq"] = ++seqPaket;
    doc["adc_raw"] = adcRaw;
    doc["soil_mv_avg"] = soilMv;
    doc["soil_pct"] = serialized(String(soilPct, 1));
    doc["temp_c"] = serialized(String(suhuUdara, 1));
    doc["air_hum_pct"] = serialized(String(lembapUdara, 1));
    doc["pump_on"] = statusPompa;
    doc["relay_switches"] = hitungTransisiRelai;
    doc["fan_pwm_pct"] = serialized(String(fanPct, 1));
    doc["ctrl_mode"] = modeOverridePaksa ? "MANUAL" : (GUNAKAN_HISTERESIS ? "HYSTERESIS" : "SINGLE_THR");
    doc["fault"] = faultDryRun ? "DRY_RUN_PROTECTION" : "NONE";   // [C6]

    serializeJson(doc, Serial);
    Serial.println();
  }
}