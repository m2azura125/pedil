/*
 * ============================================================
 *  ARDUINO NANO — Sensor Hub (5V) — Final v3
 *  Lemari Arsip Cerdas
 * ============================================================
 *
 *  Wiring:
 *   D2       -> TRIG semua HC-SR04 (share)
 *   D3       <- ECHO Stok Kotak 1
 *   D4       <- ECHO Stok Kotak 2
 *   D5       <- ECHO Stok Kotak 3
 *   D6       <- ECHO Stok Kotak 4
 *   D7       <- Reed Switch Pintu 1   (INPUT_PULLUP, LOW=tertutup)
 *   D8       <- Reed Switch Pintu 2   (INPUT_PULLUP, LOW=tertutup)
 *   D12      <- Sensor Getaran SW-420 #1 (INPUT, HIGH=aktif)
 *   D11      <- Sensor Getaran SW-420 #2 (INPUT, HIGH=aktif)
 *   A1 (TX)  -> 1kOhm -> titik tengah -> ESP32 GPIO34
 *               titik tengah -> 2kOhm -> GND
 *   A0 (RX)  -> tidak dipakai
 *   GND      -> GND (WAJIB share dengan ESP32!)
 *
 *  Format data ke ESP32 (tiap ~200ms):
 *   "pct1,pct2,pct3,pct4,ds1,ds2,vib1,vib2\n"
 *
 *   pct1-4  : persentase stok kotak 1-4 (0-100, -1 jika error)
 *   ds1,ds2 : door switch (0=tertutup/LOW, 1=terbuka/HIGH)
 *   vib1,vib2: hasil threshold (1=getaran terdeteksi, 0=diam)
 *
 *  Kalibrasi THRESHOLD_VIB:
 *   - Buka Serial Monitor (9600), biarkan sensor diam
 *   - Lihat baris "Getaran:" -> nilai raw saat diam = baseline
 *   - Set THRESHOLD_VIB sedikit di atas baseline tersebut
 * ============================================================
 */

#include <SoftwareSerial.h>

// ── PIN ────────────────────────────────────────────────────
#define TRIG     2
#define ECHO_S1  3
#define ECHO_S2  4
#define ECHO_S3  5
#define ECHO_S4  6
#define DS1      7
#define DS2      8
#define VIB1     12
#define VIB2     11

// ── THRESHOLD GETARAN (kalibrasi via Serial Monitor) ───────
#define THRESHOLD_VIB  3000

// ── SERIAL KE ESP32 ────────────────────────────────────────
// A0=RX (tidak dipakai), A1=TX -> voltage divider -> GPIO34
SoftwareSerial espSerial(A0, A1);

// ── KONFIGURASI STOK ───────────────────────────────────────
const int STOK_PENUH_CM  = 79;
const int STOK_KOSONG_CM = 0;

// ── VARIABEL ───────────────────────────────────────────────
unsigned long lastKirimMs  = 0;
unsigned long lastDebugMs  = 0;
unsigned long vibCount1    = 0;
unsigned long vibCount2    = 0;

// ── FUNGSI ULTRASONIK ──────────────────────────────────────
long bacaUS(int echoPin) {
  digitalWrite(TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  long dur = pulseIn(echoPin, HIGH, 25000);
  if (dur == 0) return -1;
  return dur * 0.034 / 2;
}

int hitungPct(long cm) {
  if (cm < 0) return -1;
  if (cm >= STOK_PENUH_CM) return 100;
  return constrain(map(cm, STOK_KOSONG_CM, STOK_PENUH_CM, 0, 100), 0, 100);
}

// ── SETUP ──────────────────────────────────────────────────
void setup() {
  Serial.begin(9600);
  espSerial.begin(9600);

  pinMode(TRIG, OUTPUT); digitalWrite(TRIG, LOW);
  pinMode(ECHO_S1, INPUT);
  pinMode(ECHO_S2, INPUT);
  pinMode(ECHO_S3, INPUT);
  pinMode(ECHO_S4, INPUT);
  pinMode(DS1,  INPUT_PULLUP);
  pinMode(DS2,  INPUT_PULLUP);
  pinMode(VIB1, INPUT);
  pinMode(VIB2, INPUT);

  Serial.println("================================");
  Serial.println("  NANO SENSOR HUB v3");
  Serial.println("================================");
  Serial.println("4x HC-SR04 + 2x DoorSwitch + 2x Getaran");
  Serial.println("TX ke ESP32: A1 -> voltage divider -> GPIO34");
  Serial.print("THRESHOLD_VIB = "); Serial.println(THRESHOLD_VIB);
  Serial.println("================================");
  delay(500);
}

// ── LOOP ───────────────────────────────────────────────────
void loop() {
  // Polling getaran terus-menerus (tangkap pulsa HIGH sekecil apapun)
  if (digitalRead(VIB1) == HIGH) vibCount1++;
  if (digitalRead(VIB2) == HIGH) vibCount2++;

  if (millis() - lastKirimMs < 200) return;
  lastKirimMs = millis();

  // Baca ultrasonik
  long s1 = bacaUS(ECHO_S1); delay(30);
  long s2 = bacaUS(ECHO_S2); delay(30);
  long s3 = bacaUS(ECHO_S3); delay(30);
  long s4 = bacaUS(ECHO_S4);

  // Baca door switch
  int ds1 = digitalRead(DS1);   // 0=tertutup, 1=terbuka
  int ds2 = digitalRead(DS2);

  // Evaluasi getaran
  int vib1 = (vibCount1 > THRESHOLD_VIB) ? 1 : 0;
  int vib2 = (vibCount2 > THRESHOLD_VIB) ? 1 : 0;
  unsigned long raw1 = vibCount1;
  unsigned long raw2 = vibCount2;
  vibCount1 = 0;
  vibCount2 = 0;

  // Kirim ke ESP32 (kirim data cm mentah)
  String data = String(s1) + "," +
                String(s2) + "," +
                String(s3) + "," +
                String(s4) + "," +
                String(ds1)  + "," + String(ds2)  + "," +
                String(vib1) + "," + String(vib2);
  espSerial.println(data);

  // Debug ke Serial Monitor tiap 2 detik
  if (millis() - lastDebugMs >= 2000) {
    lastDebugMs = millis();
    Serial.println("------------------------------------------");
    Serial.print("Jarak  : ");
    Serial.print(s1); Serial.print("cm | ");
    Serial.print(s2); Serial.print("cm | ");
    Serial.print(s3); Serial.print("cm | ");
    Serial.print(s4); Serial.println("cm");
    Serial.print("Stok % : ");
    Serial.print(hitungPct(s1)); Serial.print("% | ");
    Serial.print(hitungPct(s2)); Serial.print("% | ");
    Serial.print(hitungPct(s3)); Serial.print("% | ");
    Serial.print(hitungPct(s4)); Serial.println("%");
    Serial.print("Pintu  : 1=");
    Serial.print(ds1 == 0 ? "TUTUP" : "BUKA ");
    Serial.print("  2=");
    Serial.println(ds2 == 0 ? "TUTUP" : "BUKA");
    Serial.print("Getaran: 1=");
    Serial.print(vib1 ? "TERDETEKSI" : "diam");
    Serial.print(" (raw=" + String(raw1) + ")");
    Serial.print("  2=");
    Serial.print(vib2 ? "TERDETEKSI" : "diam");
    Serial.println(" (raw=" + String(raw2) + ")");
  }
}
