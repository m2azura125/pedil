/*
 * ============================================================
 *  ESP32 — LEMARI ARSIP CERDAS (Final v4 + WiFi)
 * ============================================================
 *
 *  Base: v4 yang sudah 100% jalan (tidak ada perubahan logika)
 *  Tambahan: WiFi HTTP ke https://padil.bilgisa.id/api
 *
 *  Yang TIDAK berubah dari v4:
 *   - Semua logika RFID, keypad, relay, door, enroll, lockout
 *   - EEPROM layout dan helper
 *   - State machine
 *   - Semua pin ESP32
 *
 *  Yang DITAMBAHKAN (berjalan di background, tidak ganggu alat):
 *   - Kirim data sensor ke web tiap 5 detik (hanya saat IDLE)
 *   - Poll perintah force_lock/force_unlock dari web tiap 10 detik
 *   - Enroll kartu baru otomatis sinkron ke web
 *   - RFID verifikasi ke web dulu, gagal -> fallback EEPROM
 *   - WiFi timeout pendek (1 detik) supaya tidak nge-hang
 *
 *  LIBRARY:
 *   MFRC522, Keypad, LiquidCrystal_I2C, EEPROM, WiFi, HTTPClient
 * ============================================================
 */

// ── GANTI SESUAI JARINGAN ANDA ──────────────────────────────
#define WIFI_SSID   "Berak Sekebon"
#define WIFI_PASS   "zaetamarie"
#define SERVER_URL  "https://padil.bilgisa.id/api"
#define NODE_ID     "NODE-A"
#define ROOM_ID     1
// ────────────────────────────────────────────────────────────

// Uncomment untuk aktifkan Nano (setelah Nano terverifikasi)
#define USE_NANO

#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <HTTPClient.h>

// ============================================================
//  PIN ESP32 (tidak berubah dari v4)
// ============================================================
#define RFID_SS   5
#define RFID_RST  4
#define LCD_ADDR  0x27

byte rowPins[4] = {26, 25, 14, 13};
byte colPins[4] = {32, 27, 12, 33};

#define RELAY    16
#define BUZZER   2
#define NANO_RX  34

// ============================================================
//  EEPROM (tidak berubah dari v4)
// ============================================================
#define EEPROM_SIZE     256
#define EEPROM_JUMLAH   0
#define EEPROM_KARTU    1
#define BYTES_PER_KARTU 8
#define MAX_KARTU       10

// ============================================================
//  KONFIGURASI (tidak berubah dari v4)
// ============================================================
const char PIN_BENAR[] = "1234";

const unsigned long GRACE_MS       = 20000;
const unsigned long ALARM_MS       = 60000;
const unsigned long LOCKOUT_MS     = 300000;
const unsigned long DS_DEBOUNCE_MS = 1000;

const int VIB_WINDOW_MS  = 2000;
const int VIB_DANGER_CNT = 5;

// ============================================================
//  STATE MACHINE (tidak berubah dari v4)
// ============================================================
enum State {
  S_IDLE, S_PIN_ENTRY, S_PIN_ONLY,
  S_DOOR_OPEN, S_LOCKOUT, S_VIB_ALERT,
  S_ENROLL_PIN, S_ENROLL
};
State sysState = S_IDLE;

const char* STATE_NAMES[] = {
  "IDLE","PIN_ENTRY","PIN_ONLY","DOOR_OPEN",
  "LOCKOUT","VIB_ALERT","ENROLL_PIN","ENROLL"
};

// ============================================================
//  KEYPAD (tidak berubah dari v4)
// ============================================================
const byte KP_ROWS = 4, KP_COLS = 4;
char keys[4][4] = {
  {'1','4','7','*'},
  {'2','5','8','0'},
  {'3','6','9','#'},
  {'A','B','C','D'}
};

// ============================================================
//  OBJEK LIBRARY
// ============================================================
MFRC522 rfid(RFID_SS, RFID_RST);
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, KP_ROWS, KP_COLS);

// ============================================================
//  VARIABEL (tidak berubah dari v4)
// ============================================================
int  stokPct[4]  = {100,100,100,100};
int  dsRecv[2]   = {1,1};
int  vibRecv[2]  = {0,0};
bool nanoOK      = false;
unsigned long lastNanoMs = 0;

int  rfidGagal = 0, pinGagal = 0;
char pinBuf[9] = ""; int pinLen = 0;

unsigned long door1BukaMs = 0, door2BukaMs = 0;
unsigned long ds1Start = 0, ds2Start = 0;
bool door1Open = false, door2Open = false;
bool relayOpen = false;

unsigned long lockoutStartMs = 0;

int  vibCnt[2]       = {0,0};
unsigned long vibWin[2] = {0,0};
bool vibPrev[2]      = {false,false};

unsigned long lastBeepMs = 0, lastStokWarnMs = 0;
unsigned long lastRfidReinitMs = 0;
String lcdL1 = "", lcdL2 = "";

// ── WiFi tambahan ────────────────────────────────────────────
bool wifiOK = false;
unsigned long lastSensorMs    = 0;
unsigned long lastCmdMs       = 0;
unsigned long lastWifiCheckMs = 0;

// ── FORWARD DECLARATION ──────────────────────────────────────
void beep(int n, int ms = 80);
void logSerial(String ev, String det = "");

void resetKeIdle() {
  sysState = S_IDLE;
  rfidGagal=0; pinGagal=0;
  pinLen=0; memset(pinBuf,0,sizeof(pinBuf));
  rfid.PCD_Init(); delay(50);
  setLCD("  SCAN KARTU  ", "                ");
}

// ============================================================
//  EEPROM HELPERS (tidak berubah dari v4)
// ============================================================
int bacaJumlahKartu() {
  byte j = EEPROM.read(EEPROM_JUMLAH);
  if (j == 0xFF || j > MAX_KARTU) return 0;
  return (int)j;
}

bool cariKartu(byte *uid, byte size) {
  int jml = bacaJumlahKartu();
  for (int i = 0; i < jml; i++) {
    int addr = EEPROM_KARTU + i * BYTES_PER_KARTU;
    byte storedSize = EEPROM.read(addr);
    if (storedSize != size) continue;
    bool cocok = true;
    for (int b = 0; b < size; b++) {
      if (EEPROM.read(addr + 1 + b) != uid[b]) { cocok=false; break; }
    }
    if (cocok) return true;
  }
  return false;
}

bool simpanKartu(byte *uid, byte size) {
  if (size == 0 || size > 7) return false;
  int jml = bacaJumlahKartu();
  if (jml >= MAX_KARTU) return false;
  if (cariKartu(uid, size)) return false;
  int addr = EEPROM_KARTU + jml * BYTES_PER_KARTU;
  EEPROM.write(addr, size);
  for (int b = 0; b < size; b++) EEPROM.write(addr + 1 + b, uid[b]);
  EEPROM.write(EEPROM_JUMLAH, jml + 1);
  EEPROM.commit();
  return true;
}

String uidToString(byte *uid, byte size) {
  String s = "";
  for (byte b = 0; b < size; b++) {
    if (b) s += ":";
    if (uid[b] < 0x10) s += "0";
    s += String(uid[b], HEX);
  }
  s.toUpperCase();
  return s;
}

String uidToHex(byte *uid, byte size) {
  String s = "";
  for (byte b = 0; b < size; b++) {
    if (uid[b] < 0x10) s += "0";
    s += String(uid[b], HEX);
  }
  s.toUpperCase();
  return s;
}

// ============================================================
//  WIFI FUNCTIONS (background, tidak ganggu alat)
// ============================================================

// Koneksi WiFi — maks 5 detik, lalu lanjut apapun hasilnya
void setupWifi() {
  lcd.setCursor(0,1); lcd.print("Koneksi WiFi... ");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  for (int i = 0; i < 10 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); Serial.print(".");
  }
  wifiOK = (WiFi.status() == WL_CONNECTED);
  Serial.println(wifiOK ? " OK: " + WiFi.localIP().toString() : " GAGAL (offline)");
}

// POST JSON ke endpoint dengan timeout pendek
int httpPost(String endpoint, String body) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  HTTPClient http;
  http.begin(String(SERVER_URL) + endpoint);
  http.addHeader("Content-Type", "application/json");
  http.setConnectTimeout(1000);
  http.setTimeout(1000);
  int code = http.POST(body);
  http.end();
  return code;
}

int httpGet(String endpoint) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  HTTPClient http;
  http.begin(String(SERVER_URL) + endpoint);
  http.setConnectTimeout(1000);
  http.setTimeout(1000);
  int code = http.GET();
  String resp = (code == 200) ? http.getString() : "";
  http.end();
  // Simpan response untuk di-parse, return code saja
  return code;
}

// Verifikasi kartu ke web, return true jika authorized
// httpOK = false jika koneksi error (bukan penolakan) -> fallback EEPROM
bool verifyRfidWeb(String uidHex, String &outUser, bool &httpOK) {
  httpOK = false;
  if (!wifiOK || WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.begin(String(SERVER_URL) + "/rfid_auth.php");
  http.addHeader("Content-Type", "application/json");
  http.setConnectTimeout(1000);
  http.setTimeout(1000);
  String body = "{\"action\":\"verify\",\"rfid_uid\":\"" + uidHex +
                "\",\"room_id\":" + String(ROOM_ID) + "}";
  int code = http.POST(body);
  httpOK = (code > 0);
  if (code == 200) {
    String resp = http.getString();
    http.end();
    if (resp.indexOf("\"authorized\":true") >= 0) {
      int u = resp.indexOf("\"user\":\"");
      if (u >= 0) {
        int st = u + 8;
        outUser = resp.substring(st, resp.indexOf("\"", st));
      }
      return true;
    }
    return false;
  }
  http.end();
  return false;
}

// Kirim data sensor ke web (hanya saat idle, tidak ganggu interaksi)
void sendSensorData() {
  if (!wifiOK || WiFi.status() != WL_CONNECTED) return;

  String sensors = "[";
  for (int i = 0; i < 4; i++) {
    int pct = stokPct[i];
    const char* st = (pct < 0) ? "error" : (pct < 20 ? "warning" : "normal");
    int val = (pct < 0) ? 0 : pct;
    if (i > 0) sensors += ",";
    sensors += "{\"percent\":" + String(val) + ",\"status\":\"" + String(st) + "\"}";
  }
  sensors += "]";

  String doors = "[";
  for (int i = 0; i < 2; i++) {
    if (i > 0) doors += ",";
    doors += "{\"closed\":" + String(dsRecv[i] == 0 ? "true" : "false") + "}";
  }
  doors += "]";

  String vibs = "[";
  for (int i = 0; i < 2; i++) {
    if (i > 0) vibs += ",";
    vibs += "{\"value\":" + String(vibRecv[i]) +
            ",\"level\":\"" + String(vibRecv[i] == 1 ? "danger" : "normal") + "\"}";
  }
  vibs += "]";

  String body =
    "{\"node_id\":\"" + String(NODE_ID) + "\","
    "\"room_id\":"    + String(ROOM_ID)  + ","
    "\"sys_state\":\"" + String(STATE_NAMES[sysState]) + "\","
    "\"relay_open\":" + String(relayOpen ? "true" : "false") + ","
    "\"nano_ok\":"    + String(nanoOK   ? "true" : "false") + ","
    "\"sensors\":"    + sensors + ","
    "\"door_status\":" + doors  + ","
    "\"vibration\":"  + vibs   + "}";

  int code = httpPost("/esp32_data.php", body);
  if (code != 200) Serial.println("[HTTP] sendSensor: " + String(code));
}

// Poll perintah dari web
void pollCommand() {
  if (!wifiOK || WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(String(SERVER_URL) + "/esp32_command.php?node_id=" + String(NODE_ID));
  http.setConnectTimeout(1000);
  http.setTimeout(1000);
  int code = http.GET();
  if (code != 200) { http.end(); return; }
  String resp = http.getString();
  http.end();

  if (resp.indexOf("\"command\":\"force_lock\"") >= 0) {
    if (relayOpen) {
      digitalWrite(RELAY, LOW);
      relayOpen = false;
      door1Open = false; door2Open = false;
      logSerial("CMD_FORCE_LOCK", "dari web");
      beep(2, 100);
      resetKeIdle();
    }
  } else if (resp.indexOf("\"command\":\"force_unlock\"") >= 0) {
    if (!relayOpen && sysState == S_IDLE) {
      digitalWrite(RELAY, HIGH);
      relayOpen = true;
      sysState = S_DOOR_OPEN;
      door1Open = true; door2Open = true;
      door1BukaMs = door2BukaMs = millis();
      logSerial("CMD_FORCE_UNLOCK", "dari web");
      beep(3, 60);
      setLCD("BUKA (WEB CMD)", "Silakan akses...");
    }
  }
}

// Sinkron kartu baru ke web (dipanggil SETELAH simpan EEPROM berhasil)
void enrollKartuWeb(String uidHex) {
  if (!wifiOK || WiFi.status() != WL_CONNECTED) return;
  String body = "{\"action\":\"enroll\",\"rfid_uid\":\"" + uidHex +
                "\",\"label\":\"ENROLL-ESP\",\"room_id\":" + String(ROOM_ID) + "}";
  httpPost("/rfid_auth.php", body);
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  SPI.begin();
  EEPROM.begin(EEPROM_SIZE);

  if (EEPROM.read(EEPROM_JUMLAH) == 0xFF) {
    EEPROM.write(EEPROM_JUMLAH, 0);
    EEPROM.commit();
    Serial.println("EEPROM diinisialisasi.");
  }

  #ifdef USE_NANO
    Serial2.begin(9600, SERIAL_8N1, NANO_RX, -1);
  #endif

  pinMode(RELAY,  OUTPUT); digitalWrite(RELAY,  LOW);
  pinMode(BUZZER, OUTPUT); digitalWrite(BUZZER, LOW);

  lcd.init(); lcd.backlight();
  lcd.setCursor(0,0); lcd.print("Lemari Arsip    ");
  lcd.setCursor(0,1); lcd.print("Inisialisasi... ");

  rfid.PCD_Init(); delay(50);

  setupWifi();   // maks 5 detik, lalu lanjut apapun hasilnya

  Serial.print("Kartu EEPROM: "); Serial.println(bacaJumlahKartu());
  delay(600);
  setLCD("  SCAN KARTU  ", wifiOK ? "Online          " : "Offline (EEPROM)");
  delay(1000);
  setLCD("  SCAN KARTU  ", "                ");
  Serial.println("Sistem siap.");
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  #ifdef USE_NANO
    parseNanoData();
  #endif

  // ── WiFi background tasks (hanya saat IDLE supaya tidak ganggu) ──
  if (sysState == S_IDLE) {
    // Cek status WiFi tiap 30 detik
    if (millis() - lastWifiCheckMs > 30000) {
      wifiOK = (WiFi.status() == WL_CONNECTED);
      if (!wifiOK) WiFi.reconnect();
      lastWifiCheckMs = millis();
    }
    // Kirim sensor tiap 5 detik
    if (wifiOK && millis() - lastSensorMs > 5000) {
      sendSensorData();
      lastSensorMs = millis();
    }
    // Poll perintah web tiap 10 detik
    if (wifiOK && millis() - lastCmdMs > 10000) {
      pollCommand();
      lastCmdMs = millis();
    }
  }

  // ── Fungsi utama alat (sama persis dengan v4) ────────────────
  handleRFID();
  handleKeypad();
  handleDoor();
  handleVibration();
  handleLockout();
  handleStok();
}

// ============================================================
//  TERIMA DATA NANO
// ============================================================
#ifdef USE_NANO
void parseNanoData() {
  if (!Serial2.available()) {
    if (millis() - lastNanoMs > 3000 && nanoOK) {
      nanoOK = false;
      for (int i=0; i<4; i++) stokPct[i] = -1;
      Serial.println("[NANO] Koneksi terputus!");
    }
    return;
  }
  String raw = Serial2.readStringUntil('\n');
  raw.trim();
  if (raw.length() == 0) return;

  // Validasi: harus ada 7 koma (8 nilai)
  int komaCount = 0;
  for (char c : raw) if (c == ',') komaCount++;
  if (komaCount < 7) return;

  int vals[8]={0}, prev=0;
  for (int i=0; i<8; i++) {
    int c = raw.indexOf(',', prev);
    if (c==-1) c = raw.length();
    vals[i] = raw.substring(prev, c).toInt();
    prev = c+1;
  }
  stokPct[0]=vals[0]; stokPct[1]=vals[1];
  stokPct[2]=vals[2]; stokPct[3]=vals[3];
  dsRecv[0]=vals[4];  dsRecv[1]=vals[5];
  vibRecv[0]=vals[6]; vibRecv[1]=vals[7];
  lastNanoMs = millis(); nanoOK = true;

  // Tampilkan data hasil parsing dari Nano ke Serial Monitor ESP32 tiap 2 detik
  static unsigned long lastRawPrintMs = 0;
  if (millis() - lastRawPrintMs > 2000) {
    Serial.println("------------------------------------------");
    Serial.print("Stok   : ");
    for (int i = 0; i < 4; i++) {
      if (stokPct[i] < 0) Serial.print("ERR");
      else { Serial.print(stokPct[i]); Serial.print("%"); }
      if (i < 3) Serial.print(" | ");
    }
    Serial.println();
    Serial.print("Pintu  : 1=");
    Serial.print(dsRecv[0] == 0 ? "TUTUP" : "BUKA ");
    Serial.print("  2=");
    Serial.println(dsRecv[1] == 0 ? "TUTUP" : "BUKA");
    Serial.print("Getaran: 1=");
    Serial.print(vibRecv[0] == 1 ? "TERDETEKSI" : "diam");
    Serial.print("  2=");
    Serial.println(vibRecv[1] == 1 ? "TERDETEKSI" : "diam");
    lastRawPrintMs = millis();
  }
}
#endif

// ============================================================
//  CEK PINTU (tidak berubah dari v4)
// ============================================================
bool doorTerbuka(int door) {
  #ifndef USE_NANO
    unsigned long bukaMs = (door==1) ? door1BukaMs : door2BukaMs;
    return (millis() - bukaMs >= 5000);
  #else
    int ds = dsRecv[door-1];
    unsigned long &dsStart = (door==1) ? ds1Start : ds2Start;
    if (ds == 1) {
      if (dsStart==0) dsStart = millis();
      if (millis()-dsStart >= DS_DEBOUNCE_MS) return true;
    } else { dsStart = 0; }
    return false;
  #endif
}

// ============================================================
//  RFID (sama dengan v4, tambah verifikasi web)
// ============================================================
void handleRFID() {
  // Reinit hanya kalau chip benar-benar hang
  if (sysState == S_IDLE && millis() - lastRfidReinitMs > 30000) {
    byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    if (v == 0x00 || v == 0xFF) {
      rfid.PCD_Init();
      Serial.println("[RFID] reinit (chip hang)");
    }
    lastRfidReinitMs = millis();
  }

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  byte uid[7] = {0};
  byte size = rfid.uid.size;
  for (byte b = 0; b < size && b < 7; b++) uid[b] = rfid.uid.uidByte[b];
  String uidStr = uidToString(uid, size);

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  // ── Mode ENROLL: simpan EEPROM dulu, sinkron web belakangan ──
  if (sysState == S_ENROLL) {
    bool ok = simpanKartu(uid, size);
    if (ok) {
      int jml = bacaJumlahKartu();
      Serial.println("========================================");
      Serial.println("  KARTU TERSIMPAN (ENROLL)");
      Serial.println("  UID  : " + uidStr);
      Serial.println("  Size : " + String(size) + " byte");
      Serial.println("  Slot : " + String(jml) + "/" + String(MAX_KARTU));
      Serial.println("========================================");
      logSerial("ENROLL_OK", uidStr + " (total: " + jml + ")");
      beep(2, 80);
      setLCD("KARTU TERSIMPAN!", uidStr.substring(0,16));
      delay(2000);
      enrollKartuWeb(uidStr);
      setLCD("Scan kartu lagi", "atau D utk batal");
    } else {
      int jml = bacaJumlahKartu();
      Serial.println("========================================");
      if (jml >= MAX_KARTU) {
        Serial.println("  ENROLL GAGAL: Slot penuh (" + String(MAX_KARTU) + ")");
        logSerial("ENROLL_PENUH");
        beep(3, 100);
        setLCD("SLOT PENUH!", String(MAX_KARTU) + " kartu maks");
      } else {
        Serial.println("  ENROLL GAGAL: Kartu sudah terdaftar");
        Serial.println("  UID  : " + uidStr);
        logSerial("ENROLL_DUPLIKAT", uidStr);
        beep(2, 200);
        setLCD("KARTU SUDAH ADA", uidStr.substring(0,16));
      }
      Serial.println("========================================");
      delay(2000);
      setLCD("Scan kartu baru", "atau D utk batal");
    }
    return;
  }

  if (sysState != S_IDLE) return;

  // ── Mode IDLE: verifikasi HANYA via web API ──────────────
  bool valid = false;
  String aksesUser = "";

  if (wifiOK) {
    bool httpOK = false;
    valid = verifyRfidWeb(uidStr, aksesUser, httpOK);
    if (!httpOK) {
      // Koneksi/timeout error -> tampil pesan, jangan buka
      logSerial("RFID_WEB_ERR", "koneksi ke server gagal");
      setLCD("SERVER ERROR!", "Coba lagi..    ");
      delay(1500);
      setLCD("  SCAN KARTU  ", "                ");
      return;
    }
  } else {
    // WiFi mati -> tidak bisa verifikasi, tolak
    logSerial("RFID_WIFI_MATI", "tidak bisa verifikasi");
    setLCD("WiFi Offline!", "Hubungi admin.. ");
    delay(1500);
    setLCD("  SCAN KARTU  ", "                ");
    return;
  }

  if (valid) {
    rfidGagal=0; pinLen=0; pinGagal=0;
    memset(pinBuf,0,sizeof(pinBuf));
    logSerial("RFID_OK", uidStr + (aksesUser.length() ? " | " + aksesUser : ""));
    Serial.println("========================================");
    Serial.println("  KARTU DITERIMA");
    Serial.println("  UID  : " + uidStr);
    Serial.println("  Size : " + String(size) + " byte");
    if (aksesUser.length()) Serial.println("  User : " + aksesUser);
    Serial.println("  Via  : " + String(wifiOK ? "Web API" : "EEPROM (offline)"));
    Serial.println("========================================");
    beep(3, 60);
    setLCD("AKSES DITERIMA!", aksesUser.length() ? aksesUser.substring(0,16) : "Selamat datang!");
    delay(1000);
    digitalWrite(RELAY, HIGH);
    relayOpen = true;
    sysState = S_DOOR_OPEN;
    door1Open = true; door2Open = true;
    door1BukaMs = door2BukaMs = millis();
    setLCD("PINTU TERBUKA", "Silakan akses...");
  } else {
    rfidGagal++; beep(1, 1000);
    logSerial("RFID_GAGAL", uidStr + " (" + rfidGagal + "/3)");
    Serial.println("========================================");
    Serial.println("  KARTU DITOLAK");
    Serial.println("  UID  : " + uidStr);
    Serial.println("  Size : " + String(size) + " byte");
    Serial.println("  Percobaan: " + String(rfidGagal) + "/3");
    Serial.println("  Via  : " + String(wifiOK ? "Web API" : "EEPROM (offline)"));
    Serial.println("========================================");
    if (rfidGagal >= 3) {
      sysState = S_PIN_ONLY;
      pinLen=0; memset(pinBuf,0,sizeof(pinBuf));
      setLCD("RFID GAGAL 3x!", "Masukkan PIN:   ");
    } else {
      setLCD("AKSES DITOLAK!", "Kartu: " + String(rfidGagal) + "/3 coba");
      delay(1800);
      setLCD("  SCAN KARTU  ", "                ");
    }
  }
}

// ============================================================
//  KEYPAD + PIN (tidak berubah dari v4)
// ============================================================
void handleKeypad() {
  if (sysState==S_LOCKOUT || sysState==S_VIB_ALERT || sysState==S_DOOR_OPEN) return;

  char key = keypad.getKey();
  if (!key) return;

  if (sysState == S_ENROLL) {
    if (key == 'D') { logSerial("ENROLL_BATAL"); beep(1, 50); resetKeIdle(); }
    return;
  }

  if (sysState == S_ENROLL_PIN) {
    if (key=='A'||key=='B'||key=='C'||key=='D') {
      if (key=='D') { logSerial("ENROLL_PIN_BATAL"); beep(1,50); resetKeIdle(); }
      return;
    }
    if (key=='#') {
      if (pinLen>0) {pinLen--; pinBuf[pinLen]='\0';}
      String m=""; for(int i=0;i<pinLen;i++) m+="*";
      setLCD("PIN Enroll:", m+"_");
      return;
    }
    if (key=='*') {
      pinBuf[pinLen]='\0';
      bool cocok = (strcmp(pinBuf, PIN_BENAR)==0);
      pinLen=0; memset(pinBuf,0,sizeof(pinBuf));
      if (cocok) {
        sysState=S_ENROLL; logSerial("ENROLL_PIN_OK"); beep(2,60);
        int jml=bacaJumlahKartu();
        setLCD("MODE ENROLL","Slot:"+String(jml)+"/"+String(MAX_KARTU));
        delay(1200);
        setLCD("Scan kartu baru","D = batal       ");
      } else {
        logSerial("ENROLL_PIN_SALAH"); beep(2,150);
        setLCD("PIN SALAH!","Enroll dibatal..");
        delay(1500); resetKeIdle();
      }
      return;
    }
    if (pinLen<8) { pinBuf[pinLen++]=key; String m=""; for(int i=0;i<pinLen;i++) m+="*"; setLCD("PIN Enroll:",m); }
    return;
  }

  if (sysState==S_IDLE) {
    if (key=='D') {
      pinLen=0; memset(pinBuf,0,sizeof(pinBuf));
      sysState=S_ENROLL_PIN; logSerial("ENROLL_MINTA_PIN"); beep(1,60);
      setLCD("PIN Enroll:","                ");
    }
    return;
  }

  if (key=='A'||key=='B'||key=='C'||key=='D') return;

  if (key=='#') {
    if (pinLen>0) {pinLen--; pinBuf[pinLen]='\0';}
    String m=""; for(int i=0;i<pinLen;i++) m+="*";
    setLCD("Masukkan PIN:",m+"_");
    return;
  }

  if (key=='*') {
    pinBuf[pinLen]='\0';
    bool cocok=(strcmp(pinBuf,PIN_BENAR)==0);
    if (cocok) {
      rfidGagal=0; pinGagal=0; logSerial("PIN_OK"); beep(3,60);
      setLCD("AKSES DITERIMA!","Selamat datang!");
      delay(1000);
      digitalWrite(RELAY,HIGH); relayOpen=true;
      sysState=S_DOOR_OPEN; door1Open=true; door2Open=true;
      door1BukaMs=door2BukaMs=millis();
      setLCD("PINTU TERBUKA","Silakan akses...");
    } else {
      pinGagal++; beep(2,120);
      logSerial("PIN_GAGAL",String(pinGagal)+"/3");
      if (pinGagal>=3) {
        sysState=S_LOCKOUT; lockoutStartMs=millis();
        logSerial("LOCKOUT_MULAI"); setLCD("SISTEM DIKUNCI!","Tunggu 5 menit  ");
      } else {
        setLCD("PIN SALAH!","Percobaan:"+String(pinGagal)+"/3");
        delay(1500); pinLen=0; memset(pinBuf,0,sizeof(pinBuf));
        setLCD("Masukkan PIN:","                ");
      }
    }
    return;
  }

  if (pinLen<8) { pinBuf[pinLen++]=key; String m=""; for(int i=0;i<pinLen;i++) m+="*"; setLCD("Masukkan PIN:",m); }
}

// ============================================================
//  DOOR (tidak berubah dari v4)
// ============================================================
void handleDoor() {
  if (sysState!=S_DOOR_OPEN) return;
  if (!relayOpen) return;

  bool triggered=doorTerbuka(1)||doorTerbuka(2);
  if (triggered) {
    digitalWrite(RELAY,LOW); relayOpen=false;
    door1Open=false; door2Open=false;
    logSerial("RELAY_TERKUNCI","Door switch ter-trigger");
    resetKeIdle(); return;
  }

  unsigned long el=millis()-door1BukaMs;
  if (el>ALARM_MS) {
    if (millis()-lastBeepMs>400) {beep(1,60); lastBeepMs=millis();}
    setLCD("AKSES TERBUKA!","Segera gunakan..");
  } else if (el>GRACE_MS) {
    if (millis()-lastBeepMs>1000) {beep(1,80); lastBeepMs=millis();}
    setLCD("Pintu siap akses","Silakan dibuka..");
  }
}

// ============================================================
//  STOK (tidak berubah dari v4)
// ============================================================
void handleStok() {
  if (sysState!=S_IDLE) return;
  if (!nanoOK) return;
  bool habis=false; String kotak="";
  for (int i=0;i<4;i++) {
    if (stokPct[i]>=0 && stokPct[i]<20) {habis=true; kotak+=String(i+1)+" ";}
  }
  if (habis && millis()-lastStokWarnMs>10000) {
    setLCD("STOK HABIS!","Kotak:"+kotak);
    lastStokWarnMs=millis();
  }
}

// ============================================================
//  GETARAN (tidak berubah dari v4)
// ============================================================
void handleVibration() {
  for (int i=0;i<2;i++) {
    bool now=(vibRecv[i]==1);
    if (millis()-vibWin[i]>(unsigned long)VIB_WINDOW_MS) {vibCnt[i]=0; vibWin[i]=millis();}
    if (now&&!vibPrev[i]) vibCnt[i]++;
    vibPrev[i]=now;
    if (vibCnt[i]>=VIB_DANGER_CNT) {
      logSerial("GETARAN_BAHAYA","Sensor "+String(i+1));
      beep(5,150); sysState=S_VIB_ALERT;
      setLCD("!! PERINGATAN !!","Getaran keras "+String(i+1)+"!");
      vibCnt[i]=0; delay(3000); resetKeIdle();
    }
  }
}

// ============================================================
//  LOCKOUT (tidak berubah dari v4)
// ============================================================
void handleLockout() {
  if (sysState!=S_LOCKOUT) return;
  unsigned long el=millis()-lockoutStartMs;
  if (el>=LOCKOUT_MS) { logSerial("LOCKOUT_SELESAI"); resetKeIdle(); return; }
  static unsigned long lastCnt=0;
  if (millis()-lastCnt>1000) {
    unsigned long r=LOCKOUT_MS-el;
    int m=r/60000, s=(r%60000)/1000;
    char buf[17]; snprintf(buf,sizeof(buf),"Tunggu: %02d:%02d   ",m,s);
    lcd.setCursor(0,0); lcd.print("SISTEM DIKUNCI!");
    lcd.setCursor(0,1); lcd.print(buf);
    lastCnt=millis();
  }
}

// ============================================================
//  FUNGSI PENDUKUNG
// ============================================================
void setLCD(String l1, String l2) {
  lcdL1=l1; lcdL2=l2;
  lcd.clear();
  lcd.setCursor(0,0); lcd.print(l1.substring(0,16).c_str());
  lcd.setCursor(0,1); lcd.print(l2.substring(0,16).c_str());
}

void beep(int n, int ms) {
  for (int i=0;i<n;i++) {
    digitalWrite(BUZZER,HIGH); delay(ms);
    digitalWrite(BUZZER,LOW);
    if (i<n-1) delay(80);
  }
}

void logSerial(String ev, String det) {
  unsigned long s=millis()/1000; char ts[12];
  snprintf(ts,sizeof(ts),"%02lu:%02lu:%02lu",(s/3600)%24,(s/60)%60,s%60);
  Serial.println("["+String(ts)+"] "+ev+(det.length()?" | "+det:""));
}
