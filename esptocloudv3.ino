#include <WiFi.h>
#include <FirebaseESP32.h>

// ==== Konfigurasi WiFi ====
#define WIFI_SSID "" // SSID
#define WIFI_PASSWORD "" // SSID

// ==== Konfigurasi Firebase ====
#define FIREBASE_HOST "" // HOST 
#define FIREBASE_AUTH "" // AUTH TOKEN

FirebaseData fbData;
FirebaseAuth auth;
FirebaseConfig config;

// ==== Variabel Sensor & Target ====
float current_pH = 0;
float current_TDS = 0;
float target_ph_low = 5.5;
float target_ph_high = 7.0;
float target_tds_low = 700.0; // Pemicu Dosis TDS (Start dosing when below 700)
int modeAuto = 0;

// ==== Variabel Kontrol Otomatis Non-Blocking (Simultan) ====
// Dua set State Machine yang berjalan independen
int controlState_pH = 0;   // State untuk kontrol pH (0=Idle, 1-3=Aktif)
int controlState_TDS = 0;  // State untuk kontrol TDS (0=Idle, 10-15=Aktif)

unsigned long phPumpStartTime = 0;
unsigned long phWaitStartTime = 0;
int phPumpChannel = 0; 

unsigned long tdsPumpStartTime = 0;
unsigned long tdsWaitStartTime = 0; 
int tdsPumpChannel = 0; 

// ==== Interval ====
unsigned long lastUpload = 0;
const unsigned long uploadInterval = 10000; // kirim sensor tiap 10 detik
unsigned long lastAutoControl = 0;
const unsigned long autoControlInterval = 10000; // cek kondisi awal otomatis tiap 10 detik
unsigned long lastControlCheck = 0;
const unsigned long controlInterval = 500; // cek kontrol manual setiap 0.5 detik

String serialLine = "";

// --- FUNGSI UTAMA ---

// === Kirim perintah ke RF Nano ===
void sendCommandToNano(int ch, int state) {
  String cmd = "CTRL," + String(ch) + "," + String(state) + "\n";
  Serial.print(cmd);
  // Serial.println("Kirim ke Nano: " + cmd); // Uncomment untuk debugging
}

// === Parsing data sensor dari Nano & Kirim ke Firebase ===
void kirimKeFirebase(const String &csv) {
  float pH=0, TDS=0, AirTemp=0, UdaraTemp=0, Hum=0, Lux=0, Jarak=0, Flowwater=0, Volt=0;
  // Membaca 9 data dari string CSV
  int parsed = sscanf(csv.c_str(), "%f,%f,%f,%f,%f,%f,%f,%f,%f",
                      &pH, &TDS, &AirTemp, &UdaraTemp, &Hum, &Lux, &Jarak, &Flowwater, &Volt);

  if (parsed >= 1) {
    current_pH = pH;
    current_TDS = TDS;

    Firebase.setFloat(fbData, "/hidroponik/pH", pH);
    Firebase.setFloat(fbData, "/hidroponik/TDS", TDS);
    Firebase.setFloat(fbData, "/hidroponik/AirTemp", AirTemp);
    Firebase.setFloat(fbData, "/hidroponik/UdaraTemp", UdaraTemp);
    Firebase.setFloat(fbData, "/hidroponik/Kelembapan", Hum);
    Firebase.setFloat(fbData, "/hidroponik/Lux", Lux);
    Firebase.setFloat(fbData, "/hidroponik/Jarak", Jarak);
    Firebase.setFloat(fbData, "/hidroponik/waterflow", Flowwater);
    Firebase.setFloat(fbData, "/hidroponik/Tegangan", Volt);
  }
}

// === Cek mode & target dari Firebase ===
void cekModeDanTarget() {
  if (Firebase.getInt(fbData, "/hidroponik/control/mode_auto"))
    modeAuto = fbData.intData();

  if (Firebase.getFloat(fbData, "/hidroponik/target/ph_low"))
    target_ph_low = fbData.floatData();

  if (Firebase.getFloat(fbData, "/hidroponik/target/ph_high"))
    target_ph_high = fbData.floatData();

  if (Firebase.getFloat(fbData, "/hidroponik/target/tds_low"))
    target_tds_low = fbData.floatData();
}

// === Mode Manual Langsung ===
void cekPerintahManualLangsung() {
  for (int ch = 1; ch <= 4; ch++) {
    String child = "/hidroponik/control/ch" + String(ch);
    if (Firebase.getInt(fbData, child)) {
      int val = fbData.intData();
      sendCommandToNano(ch, val);
    }
  }
}

// === Kontrol pH (Ch 1 & 2) Non-Blocking ===
void kontrolPH() {
  unsigned long currentMillis = millis();
  unsigned long pumpDuration = 5000;
  unsigned long phWaitDuration = 30000;
  
  // STATE 0: IDLE / Cek Kondisi Awal
  if (controlState_pH == 0) {
    if (current_pH < target_ph_low && current_pH > 0.0) {
      phPumpChannel = 1; // pH Up
      controlState_pH = 1;
      Serial.println("Auto pH: Trigger pH Up (Ch1)");
    } else if (current_pH > target_ph_high) {
      phPumpChannel = 2; // pH Down
      controlState_pH = 1;
      Serial.println("Auto pH: Trigger pH Down (Ch2)");
    } 
    return;
  }

  // STATE 1: pH ON
  if (controlState_pH == 1) {
    sendCommandToNano(phPumpChannel, 1);
    phPumpStartTime = currentMillis;
    controlState_pH = 2; 
  }

  // STATE 2: Tunggu Durasi Pompa Selesai -> pH OFF
  else if (controlState_pH == 2 && currentMillis - phPumpStartTime >= pumpDuration) {
    sendCommandToNano(phPumpChannel, 0);
    Serial.println("Auto pH: Channel " + String(phPumpChannel) + " OFF");
    phWaitStartTime = currentMillis;
    controlState_pH = 3; // Tunggu Reaksi
  }

  // STATE 3: Tunggu Reaksi pH Selesai
  else if (controlState_pH == 3 && currentMillis - phWaitStartTime >= phWaitDuration) {
    controlState_pH = 0; // Kembali ke Idle
    Serial.println("Auto pH: Selesai tunggu reaksi, kembali ke Idle.");
  }
}

// === Kontrol TDS (Ch 3 & 4) Non-Blocking (Dosis Ganda) ===
void kontrolTDS() {
  unsigned long currentMillis = millis();
  unsigned long pumpDuration = 5000;
  unsigned long tdsMixDuration = 30000;
  unsigned long tdsWaitDuration = 60000;
  
  // STATE 0 (atau 10): IDLE / Cek Kondisi Awal
  if (controlState_TDS == 0) {
    // Cek TDS: Apakah perlu Nutrisi A & B? (TDS < 700)
    if (current_TDS < target_tds_low && current_TDS > 1.0) {
      tdsPumpChannel = 3; // Mulai dengan Nutrisi A
      controlState_TDS = 10; // Lanjut ke State 10
      Serial.println("Auto TDS: Trigger Nutrisi A (Ch3)");
    } 
    return;
  }
  
  // STATE 10: TDS A ON (Nutrisi A / Ch 3)
  else if (controlState_TDS == 10) {
    sendCommandToNano(3, 1);
    tdsPumpStartTime = currentMillis;
    controlState_TDS = 11; 
  }

  // STATE 11: Tunggu Durasi TDS A Selesai -> TDS A OFF
  else if (controlState_TDS == 11 && currentMillis - tdsPumpStartTime >= pumpDuration) {
    sendCommandToNano(3, 0);
    Serial.println("Auto TDS: Nutrisi A (Ch3) OFF");
    tdsWaitStartTime = currentMillis;
    controlState_TDS = 12; // Pindah ke State 12: Tunggu Waktu Campur (5 detik)
  }
  
  // STATE 12: Tunggu Waktu Campur (5 detik)
  else if (controlState_TDS == 12 && currentMillis - tdsWaitStartTime >= tdsMixDuration) {
    tdsPumpStartTime = currentMillis;
    controlState_TDS = 13; // Pindah ke State 13: TDS B ON
    Serial.println("Auto TDS: Selesai Campur A, Trigger Nutrisi B (Ch4)");
  }
  
  // STATE 13: TDS B ON (Nutrisi B / Ch 4)
  else if (controlState_TDS == 13) {
    sendCommandToNano(4, 1);
    tdsPumpStartTime = currentMillis;
    controlState_TDS = 14; 
  }

  // STATE 14: Tunggu Durasi TDS B Selesai -> TDS B OFF
  else if (controlState_TDS == 14 && currentMillis - tdsPumpStartTime >= pumpDuration) {
    sendCommandToNano(4, 0);
    Serial.println("Auto TDS: Nutrisi B (Ch4) OFF");
    tdsWaitStartTime = currentMillis;
    controlState_TDS = 15; // Pindah ke State 15: Tunggu Waktu Reaksi (60 detik)
  }
  
  // STATE 15: Tunggu Reaksi TDS Selesai
  else if (controlState_TDS == 15 && currentMillis - tdsWaitStartTime >= tdsWaitDuration) {
    controlState_TDS = 0; // Pindah kembali ke State 0: Idle
    Serial.println("Auto TDS: Selesai tunggu reaksi TDS, kembali ke Idle.");
  }
}

void setup() {
  Serial.begin(9600); 
  delay(100);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) delay(300);

  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

void loop() {
  unsigned long currentMillis = millis();
  
  // === Baca data dari RF Nano (Selalu Berjalan) ===
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialLine.length() > 0) {
        // Kirim data sensor ke Firebase
        if (currentMillis - lastUpload >= uploadInterval) {
          kirimKeFirebase(serialLine);
          lastUpload = currentMillis;
        }
        serialLine = "";
      }
    } else {
      serialLine += c;
      if (serialLine.length() > 300) serialLine.remove(0, serialLine.length() - 300);
    }
  }

  // === Cek Mode & Target dari Firebase ===
  cekModeDanTarget(); 

  // 1. JIKA MODE MANUAL AKTIF (modeAuto = 0)
  if (modeAuto == 0 && currentMillis - lastControlCheck >= controlInterval) {
    lastControlCheck = currentMillis;
    cekPerintahManualLangsung();
    
    // Reset kedua state jika beralih ke Manual
    if (controlState_pH != 0) controlState_pH = 0; 
    if (controlState_TDS != 0) controlState_TDS = 0; 
    
  // 2. JIKA MODE OTOMATIS AKTIF (modeAuto = 1)
  } else if (modeAuto == 1) {
    
    // A. Kontrol TDS (Prioritas Tinggi karena Nutrisi/Kekurangan cepat berbahaya)
    // Cek kondisi awal TDS atau lanjutkan proses yang sedang berjalan
    if (controlState_TDS == 0 && currentMillis - lastAutoControl >= autoControlInterval) {
      kontrolTDS();
    } else if (controlState_TDS != 0) {
      kontrolTDS();
    }

    // B. Kontrol pH
    // Cek kondisi awal pH atau lanjutkan proses yang sedang berjalan
    if (controlState_pH == 0 && currentMillis - lastAutoControl >= autoControlInterval) {
      kontrolPH();
    } else if (controlState_pH != 0) {
      kontrolPH();
    }
    
    // Update interval cek kondisi awal (State 0)
    if (currentMillis - lastAutoControl >= autoControlInterval) {
        lastAutoControl = currentMillis;
    }
  }
}
