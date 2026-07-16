/*
 * ============================================================
 *  ESP32 — LEMARI ARSIP CERDAS (Final v4 + WiFi)
 * ============================================================
 *
 *  Base: v4 yang sudah 100% jalan (tidak ada perubahan logika)
 *  Tambahan: WiFi HTTP ke https://padil.bilgisa.id/api
 *
 *  LIBRARY:
 *   MFRC522, Keypad, LiquidCrystal_I2C, EEPROM, WiFi, HTTPClient
 * ============================================================
 */

// ── GANTI SESUAI JARINGAN ANDA ──────────────────────────────
#define WIFI_SSID   "Fadhil"
#define WIFI_PASS   "dhil2004"
#define SERVER_URL  "https://fadhilta.my.id/api"
#define NODE_ID     "NODE-A"
#define ROOM_ID     1

#define TELEGRAM_BOT_TOKEN "8934757507:AAFtxfEG-9Odat081ZpJGqbxzgjcYzl2lpc"
#define TELEGRAM_CHAT_ID   "-5182965830"
// ────────────────────────────────────────────────────────────

#define USE_NANO

#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// ============================================================
//  PIN ESP32
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
//  EEPROM
// ============================================================
#define EEPROM_SIZE     256
#define EEPROM_JUMLAH   0
#define EEPROM_KARTU    1
#define BYTES_PER_KARTU 8
#define MAX_KARTU       10

// ============================================================
//  KONFIGURASI
// ============================================================
const char PIN_BENAR[] = "1234";

const unsigned long GRACE_MS       = 20000;
const unsigned long ALARM_MS       = 60000;
const unsigned long LOCKOUT_MS     = 300000;
const unsigned long DS_DEBOUNCE_MS = 1000;

const int VIB_WINDOW_MS  = 10000;
const int VIB_DANGER_CNT = 3;

// ============================================================
//  STATE MACHINE
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
//  KEYPAD
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
//  VARIABEL
// ============================================================
int  stokPct[4]  = {100,100,100,100};
int  sensorCm[4] = {0,0,0,0};
const int STOK_PENUH_CM  = 79;
const int STOK_KOSONG_CM = 0;

int hitungPct(long cm) {
  if (cm < 0) return -1;
  if (cm >= STOK_PENUH_CM) return 100;
  return constrain(map(cm, STOK_KOSONG_CM, STOK_PENUH_CM, 0, 100), 0, 100);
}

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

bool wifiOK = false;
unsigned long lastSensorMs    = 0;
unsigned long lastCmdMs       = 0;
unsigned long lastWifiCheckMs = 0;

String telegramPendingMsg   = "";
bool   telegramHasPending   = false;
unsigned long lastTelegramRetryMs = 0;
const unsigned long TELEGRAM_RETRY_MS = 15000;

// ── FORWARD DECLARATION ──────────────────────────────────────
void beep(int n, int ms = 80);
void beepDurasi(unsigned long totalMs, int onMs = 150, int offMs = 100);
void logSerial(String ev, String det = "");

void resetKeIdle() {
  sysState = S_IDLE;
  rfidGagal=0; pinGagal=0;
  pinLen=0; memset(pinBuf,0,sizeof(pinBuf));
  rfid.PCD_Init(); delay(50);
  setLCD("  SCAN KARTU  ", "                ");
}

// ============================================================
//  EEPROM HELPERS
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
//  WIFI FUNCTIONS
// ============================================================
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

// Kirim pesan ke Telegram, return true jika berhasil (HTTP 200)
bool sendTelegramMessage(String text) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(TELEGRAM_BOT_TOKEN) + "/sendMessage";
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.setConnectTimeout(2000);
  http.setTimeout(2000);
  String body = "{\"chat_id\":\"" + String(TELEGRAM_CHAT_ID) + "\",\"text\":\"" + text + "\"}";
  int code = http.POST(body);
  http.end();
  return (code == 200);
}

// Kirim alert Telegram. Kalau gagal (WiFi/timeout), pesan disimpan
// sebagai pending dan otomatis dicoba lagi saat alert berikutnya
// atau secara berkala lewat retryTelegramPending() di loop().
void sendTelegramAlert(String text) {
  if (telegramHasPending) {
    if (sendTelegramMessage(telegramPendingMsg)) {
      logSerial("TELEGRAM_RETRY_OK");
      telegramHasPending = false;
      telegramPendingMsg = "";
    }
  }
  if (sendTelegramMessage(text)) {
    logSerial("TELEGRAM_OK");
  } else {
    logSerial("TELEGRAM_GAGAL", "disimpan utk retry");
    telegramPendingMsg = telegramHasPending ? (telegramPendingMsg + " || " + text) : text;
    telegramHasPending = true;
  }
}

void retryTelegramPending() {
  if (!telegramHasPending) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastTelegramRetryMs < TELEGRAM_RETRY_MS) return;
  lastTelegramRetryMs = millis();
  if (sendTelegramMessage(telegramPendingMsg)) {
    logSerial("TELEGRAM_RETRY_OK");
    telegramHasPending = false;
    telegramPendingMsg = "";
  }
}

// Verifikasi RFID ke web
// httpOK = false jika koneksi error (bukan penolakan server)
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

// Verifikasi PIN ke web, fallback PIN lokal kalau koneksi error
bool verifyPinWeb(String pin, String &outUser, bool &httpOK) {
  httpOK = false;
  if (!wifiOK || WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.begin(String(SERVER_URL) + "/rfid_auth.php");
  http.addHeader("Content-Type", "application/json");
  http.setConnectTimeout(1000);
  http.setTimeout(1000);
  String body = "{\"action\":\"verify_pin\",\"pin\":\"" + pin +
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
    const char* lv = (vibRecv[i] == 2) ? "bahaya" :
                     (vibRecv[i] == 1) ? "mencurigakan" : "normal";
    vibs += "{\"value\":" + String(vibRecv[i]) + ",\"level\":\"" + String(lv) + "\"}";
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
      digitalWrite(RELAY, LOW); relayOpen=false;
      door1Open=false; door2Open=false;
      logSerial("CMD_FORCE_LOCK","dari web"); beep(2,100); resetKeIdle();
    }
  } else if (resp.indexOf("\"command\":\"force_unlock\"") >= 0) {
    if (!relayOpen && sysState==S_IDLE) {
      digitalWrite(RELAY, HIGH); relayOpen=true;
      sysState=S_DOOR_OPEN; door1Open=true; door2Open=true;
      door1BukaMs=door2BukaMs=millis();
      logSerial("CMD_FORCE_UNLOCK","dari web"); beep(3,60);
      setLCD("BUKA (WEB CMD)","Silakan akses...");
    }
  }
}

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
    EEPROM.write(EEPROM_JUMLAH, 0); EEPROM.commit();
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
  setupWifi();
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
  if (sysState == S_IDLE) {
    if (millis() - lastWifiCheckMs > 30000) {
      wifiOK = (WiFi.status() == WL_CONNECTED);
      if (!wifiOK) WiFi.reconnect();
      lastWifiCheckMs = millis();
    }
    if (wifiOK && millis() - lastSensorMs > 5000) {
      sendSensorData(); lastSensorMs = millis();
    }
    if (wifiOK && millis() - lastCmdMs > 10000) {
      pollCommand(); lastCmdMs = millis();
    }
  }
  handleRFID();
  handleKeypad();
  handleDoor();
  handleVibration();
  handleLockout();
  handleStok();
  retryTelegramPending();
}

// ============================================================
//  TERIMA DATA NANO
// ============================================================
#ifdef USE_NANO
void parseNanoData() {
  if (!Serial2.available()) {
    if (millis() - lastNanoMs > 3000 && nanoOK) {
      nanoOK = false;
      for (int i=0; i<4; i++) { stokPct[i]=-1; sensorCm[i]=-1; }
      Serial.println("[NANO] Koneksi terputus!");
    }
    return;
  }
  String raw = Serial2.readStringUntil('\n');
  raw.trim();
  if (raw.length() == 0) return;
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
  sensorCm[0]=vals[0]; sensorCm[1]=vals[1];
  sensorCm[2]=vals[2]; sensorCm[3]=vals[3];
  for (int i=0; i<4; i++) stokPct[i] = hitungPct(sensorCm[i]);
  dsRecv[0]=vals[4];  dsRecv[1]=vals[5];
  vibRecv[0]=vals[6]; vibRecv[1]=vals[7];
  lastNanoMs = millis(); nanoOK = true;

  static unsigned long lastPrintMs = 0;
  if (millis() - lastPrintMs > 2000) {
    lastPrintMs = millis();
    Serial.println("------------------------------------------");
    Serial.print("Jarak  : ");
    for (int i=0;i<4;i++) {
      if (sensorCm[i]<0) Serial.print("-1cm");
      else { Serial.print(sensorCm[i]); Serial.print("cm"); }
      if (i<3) Serial.print(" | ");
    }
    Serial.println();
    Serial.print("Stok % : ");
    for (int i=0;i<4;i++) {
      if (stokPct[i]<0) Serial.print("-1%");
      else { Serial.print(stokPct[i]); Serial.print("%"); }
      if (i<3) Serial.print(" | ");
    }
    Serial.println();
    Serial.print("Pintu  : 1="); Serial.print(dsRecv[0]==0?"TUTUP":"BUKA ");
    Serial.print("  2="); Serial.println(dsRecv[1]==0?"TUTUP":"BUKA");
    Serial.print("Getaran: 1="); Serial.print(vibRecv[0]==1?"TERDETEKSI":"diam");
    Serial.print("  2="); Serial.println(vibRecv[1]==1?"TERDETEKSI":"diam");
  }
}
#endif

// ============================================================
//  CEK PINTU
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
//  RFID
// ============================================================
void handleRFID() {
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

  // ── Mode ENROLL ──────────────────────────────────────────
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
      setLCD("Scan kartu lagi", "atau B utk batal");
    } else {
      int jml = bacaJumlahKartu();
      Serial.println("========================================");
      if (jml >= MAX_KARTU) {
        Serial.println("  ENROLL GAGAL: Slot penuh");
        logSerial("ENROLL_PENUH"); beep(3,100);
        setLCD("SLOT PENUH!", String(MAX_KARTU)+" kartu maks");
      } else {
        Serial.println("  ENROLL GAGAL: Kartu sudah terdaftar");
        Serial.println("  UID  : " + uidStr);
        logSerial("ENROLL_DUPLIKAT", uidStr); beep(2,200);
        setLCD("KARTU SUDAH ADA", uidStr.substring(0,16));
      }
      Serial.println("========================================");
      delay(2000);
      setLCD("Scan kartu baru", "atau B utk batal");
    }
    return;
  }

  if (sysState != S_IDLE) return;

  // ── Mode IDLE: verifikasi RFID ────────────────────────────
  // Urutan: (1) WiFi konek -> cek web, (2) WiFi mati -> cek EEPROM
  bool valid = false;
  String aksesUser = "";
  String viaKeterangan = "";

  if (wifiOK) {
    bool httpOK = false;
    valid = verifyRfidWeb(uidStr, aksesUser, httpOK);
    if (!httpOK) {
      // Koneksi/timeout error -> fallback EEPROM
      logSerial("RFID_WEB_ERR", "fallback EEPROM");
      valid = cariKartu(uid, size);
      viaKeterangan = "EEPROM (web error)";
    } else {
      viaKeterangan = valid ? "Web API" : "Web API (ditolak)";
    }
  } else {
    // WiFi mati -> langsung cek EEPROM
    valid = cariKartu(uid, size);
    viaKeterangan = valid ? "EEPROM (offline)" : "EEPROM (tidak ada)";
    if (!valid) Serial.println("[INFO] WiFi mati, kartu tidak ada di EEPROM");
  }

  if (valid) {
    rfidGagal=0; pinLen=0; pinGagal=0;
    memset(pinBuf,0,sizeof(pinBuf));
    logSerial("RFID_OK", uidStr + (aksesUser.length() ? " | "+aksesUser : ""));
    Serial.println("========================================");
    Serial.println("  KARTU DITERIMA");
    Serial.println("  UID  : " + uidStr);
    Serial.println("  Size : " + String(size) + " byte");
    if (aksesUser.length()) Serial.println("  User : " + aksesUser);
    Serial.println("  Via  : " + viaKeterangan);
    Serial.println("========================================");
    beep(3, 60);
    setLCD("AKSES DITERIMA!", aksesUser.length() ? aksesUser.substring(0,16) : "Selamat datang!");
    delay(1000);
    digitalWrite(RELAY, HIGH); relayOpen=true;
    sysState=S_DOOR_OPEN; door1Open=true; door2Open=true;
    door1BukaMs=door2BukaMs=millis();
    setLCD("PINTU TERBUKA", "Silakan akses...");
  } else {
    rfidGagal++; beep(1, 1000);
    logSerial("RFID_GAGAL", uidStr+" ("+rfidGagal+"/3)");
    Serial.println("========================================");
    Serial.println("  KARTU DITOLAK");
    Serial.println("  UID  : " + uidStr);
    Serial.println("  Size : " + String(size) + " byte");
    Serial.println("  Percobaan: " + String(rfidGagal) + "/3");
    Serial.println("  Via  : " + viaKeterangan);
    Serial.println("========================================");
    if (rfidGagal >= 3) {
      sysState=S_PIN_ONLY; pinLen=0; memset(pinBuf,0,sizeof(pinBuf));
      setLCD("RFID GAGAL 3x!", "Masukkan PIN:   ");
    } else {
      setLCD("AKSES DITOLAK!", "Kartu: "+String(rfidGagal)+"/3 coba");
      delay(1800);
      setLCD("  SCAN KARTU  ", "                ");
    }
  }
}

// ============================================================
//  KEYPAD + PIN
// ============================================================
void handleKeypad() {
  if (sysState==S_LOCKOUT || sysState==S_VIB_ALERT || sysState==S_DOOR_OPEN) return;
  char key = keypad.getKey();
  if (!key) return;

  if (sysState == S_ENROLL) {
    if (key=='B') { logSerial("ENROLL_BATAL"); beep(1,50); resetKeIdle(); }
    return;
  }

  if (sysState == S_ENROLL_PIN) {
    if (key=='B') { logSerial("ENROLL_PIN_BATAL"); beep(1,50); resetKeIdle(); return; }
    if (key=='A'||key=='C'||key=='D') return;
    if (key=='#') {
      if (pinLen>0) {pinLen--; pinBuf[pinLen]='\0';}
      String m=""; for(int i=0;i<pinLen;i++) m+="*";
      setLCD("PIN Enroll:", m+"_");
      return;
    }
    if (key=='*') {
      pinBuf[pinLen]='\0';
      bool cocok=(strcmp(pinBuf,PIN_BENAR)==0);
      pinLen=0; memset(pinBuf,0,sizeof(pinBuf));
      if (cocok) {
        sysState=S_ENROLL; logSerial("ENROLL_PIN_OK"); beep(2,60);
        int jml=bacaJumlahKartu();
        setLCD("MODE ENROLL","Slot:"+String(jml)+"/"+String(MAX_KARTU));
        delay(1200);
        setLCD("Scan kartu baru","B = batal       ");
      } else {
        logSerial("ENROLL_PIN_SALAH"); beep(2,150);
        setLCD("PIN SALAH!","Enroll dibatal.."); delay(1500); resetKeIdle();
      }
      return;
    }
    if (pinLen<8) { pinBuf[pinLen++]=key; String m=""; for(int i=0;i<pinLen;i++) m+="*"; setLCD("PIN Enroll:",m); }
    return;
  }

  if (sysState==S_IDLE) {
    if (key=='B') {
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
    bool valid = false;
    String aksesUser = "";
    if (wifiOK) {
      bool httpOK = false;
      valid = verifyPinWeb(pinBuf, aksesUser, httpOK);
      if (!httpOK) valid = (strcmp(pinBuf, PIN_BENAR)==0);  // fallback lokal
    } else {
      valid = (strcmp(pinBuf, PIN_BENAR)==0);  // offline -> PIN lokal
    }
    if (valid) {
      rfidGagal=0; pinGagal=0;
      logSerial("PIN_OK", aksesUser.length() ? aksesUser : "PIN Lokal");
      beep(3,60);
      setLCD("AKSES DITERIMA!", aksesUser.length() ? aksesUser.substring(0,16) : "Selamat datang!");
      delay(1000);
      digitalWrite(RELAY,HIGH); relayOpen=true;
      sysState=S_DOOR_OPEN; door1Open=true; door2Open=true;
      door1BukaMs=door2BukaMs=millis();
      setLCD("PINTU TERBUKA","Silakan akses...");
    } else {
      pinGagal++; beep(1,1000);
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
//  DOOR
// ============================================================
void handleDoor() {
  if (sysState!=S_DOOR_OPEN) return;
  if (!relayOpen) return;
  if (millis() - door1BukaMs < 4000) return;  // grace 4 detik
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
//  STOK
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
//  GETARAN
// ============================================================
void handleVibration() {
  for (int i=0;i<2;i++) {
    bool now=(vibRecv[i]==1);
    if (millis()-vibWin[i]>(unsigned long)VIB_WINDOW_MS) {
      vibCnt[i]=0; vibWin[i]=millis();
    }
    if (now&&!vibPrev[i]) {
      vibCnt[i]++;
      Serial.print("[VIB] Sensor "); Serial.print(i+1);
      Serial.print(" count: "); Serial.print(vibCnt[i]);
      Serial.println("/" + String(VIB_DANGER_CNT));
    }
    vibPrev[i]=now;
    if (vibCnt[i]>=VIB_DANGER_CNT) {
      logSerial("GETARAN_BAHAYA","Sensor "+String(i+1));
      sysState=S_VIB_ALERT;
      setLCD("!! PERINGATAN !!","Getaran keras "+String(i+1)+"!");
      sendTelegramAlert("PERINGATAN! Getaran keras terdeteksi pada Sensor " +
                         String(i+1) + " (Node: " + String(NODE_ID) +
                         ", Room: " + String(ROOM_ID) + ")");
      beepDurasi(3000, 150, 100); // buzzer alarm selama 3 detik
      vibCnt[i]=0; resetKeIdle();
    }
  }
}

// ============================================================
//  LOCKOUT
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

// Buzzer menyala putus-nyambung selama totalMs (dipakai untuk alarm bahaya)
void beepDurasi(unsigned long totalMs, int onMs, int offMs) {
  unsigned long start = millis();
  while (millis() - start < totalMs) {
    digitalWrite(BUZZER,HIGH); delay(onMs);
    digitalWrite(BUZZER,LOW); delay(offMs);
  }
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