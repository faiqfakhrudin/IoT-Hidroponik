
#include <EEPROM.h>
#include "GravityTDS.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include "DHT.h"
#include <Wire.h>
#include <BH1750.h>
#include <LiquidCrystal_I2C.h>

// ======== pH Sensor ========
#define ph_Pin A0    // pin sensor pH
float TeganganPh;
int nilai_analog_PH;
float pHValue;
float PH4 = 0.85;    // Tegangan terukur pada pH 4.0
float PH7 = 2.10;    // Tegangan terukur pada pH 6.8
float PH9 = 3.20;    // Tegangan terukur pada PH 9.2

// ======== TDS Sensor ========
#define TdsSensorPin A1
GravityTDS gravityTds;
float tdsValue = 0;

// ======== DS18B20 (Suhu Air) ========
#define ONE_WIRE_BUS 2
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
float airTemp = 0; // Suhu air

// ======== DHT22 (Suhu Udara & Kelembaban) ========
#define DHTPIN 3
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);
float hum = 0;
float airTempUdara = 0; // Suhu udara

// ======== BH1750 (Lux) ========
BH1750 lightMeter;
uint16_t lux = 0;
bool bh1750_ok = false;

// ======== HC-SR04 (Jarak) ========
#define TRIG_PIN 9
#define ECHO_PIN 12
long duration;
float jarak = 0;

// ======== ZMPT101B (AC Volt) ========
#define ZMPT_PIN A2
float acVolt = 0;
const float VREF = 5.0;
float offsetzmpt = 512.0;

// ======== Flow water  ========
const int port_waterflow = 4;
int flow_counter = 0; // 
unsigned long oldTime_waterflow;
float waterflowValue = 0.0; // Hasil laju alir (L/menit)
bool flow_last_state = HIGH; // Status pin terakhir untuk deteksi polling

// ======== LCD 20x4 I2C ========
LiquidCrystal_I2C lcd(0x27, 20, 4);

// ======== Relay (perintah kontrol) ========
const uint8_t RELAY_CH1 = 8;
const uint8_t RELAY_CH2 = 7;
const uint8_t RELAY_CH3 = 6;
const uint8_t RELAY_CH4 = 5;
const bool RELAY_ACTIVE_LOW = true;

// Variabel untuk kontrol waktu
unsigned long lastSensorRead = 0;
const unsigned long sensorInterval = 2000;

unsigned long lastSwitch = 0;
const unsigned long pageInterval = 5000; // 5 detik per halaman
int page = 0; // INT: 0, 1, 2 untuk 3 halaman

// FUNGSI POLLING FLOW WATER BARU
void update_waterflow() {
    // 1. Logika Polling: Cek perubahan status pin
    bool currentState = digitalRead(port_waterflow);
    
    // Deteksi perubahan status (tepi jatuh / FALLING edge)
    if (currentState != flow_last_state) {
        flow_last_state = currentState;
        // Hanya hitung pulse jika transisi dari HIGH ke LOW
        if (currentState == LOW) {
            flow_counter++; 
        }
    }
    
    // 2. Perhitungan Laju Alir (setiap 1 detik)
    if (millis() - oldTime_waterflow >= 1000) {
        // Konversi pulse ke L/menit (asumsi 4.8 pulse/L)
        waterflowValue = (float)flow_counter / 4.8;
        
        // Reset counter dan waktu
        flow_counter = 0;
        oldTime_waterflow = millis();
    }
}

void setup() {
    Serial.begin(9600);
    lcd.init();
    lcd.backlight();

    // boot screen 
    lcd.clear();
    lcd.setCursor(2,0); lcd.print("Hydroponic System");
    lcd.setCursor(4,1); lcd.print("Monitoring");
    lcd.setCursor(3,2); lcd.print("Please Wait...");
    lcd.setCursor(0,3);
    for (int i=0; i<20; i++) { lcd.print((char)255); delay(50); }

    gravityTds.setPin(TdsSensorPin);
    gravityTds.setAref(5.0);
    gravityTds.setAdcRange(1024);
    gravityTds.begin();

    sensors.begin();
    dht.begin();
    bh1750_ok = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);

    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);

    // Flow Water Polling setup
    pinMode(port_waterflow, INPUT_PULLUP); 

    // Relay pins init
    pinMode(RELAY_CH1, OUTPUT);
    pinMode(RELAY_CH2, OUTPUT);
    pinMode(RELAY_CH3, OUTPUT);
    pinMode(RELAY_CH4, OUTPUT);
    // set default OFF
    digitalWrite(RELAY_CH1, RELAY_ACTIVE_LOW ? HIGH : LOW);
    digitalWrite(RELAY_CH2, RELAY_ACTIVE_LOW ? HIGH : LOW);
    digitalWrite(RELAY_CH3, RELAY_ACTIVE_LOW ? HIGH : LOW);
    digitalWrite(RELAY_CH4, RELAY_ACTIVE_LOW ? HIGH : LOW);
}

// FUNGSI UPDATE LCD (3 Halaman)
void updateLcd() {
    // Logika Pergantian Halaman: 0 -> 1 -> 2 -> 0 (Setiap 5 detik)
    if (millis() - lastSwitch >= pageInterval) {
        page++; 
        if (page > 2) page = 0; 
        lastSwitch = millis();
        lcd.clear(); 
    }
    // ===================================
    // Halaman 0: Air Temp, pH, TDS, AC Volt 
    // ===================================
    if (page == 0) {
        lcd.setCursor(0,0); lcd.print("Air Temp : ");
        if (airTemp == DEVICE_DISCONNECTED_C) lcd.print("OFF");
        else if (airTemp < -50 || airTemp > 125) lcd.print("0");
        else { lcd.print(airTemp,1); lcd.print(" C"); }

        lcd.setCursor(0,1); lcd.print("pH       : ");
        if (pHValue < 0 || pHValue > 14) lcd.print("0");
        else lcd.print(pHValue,2);

        lcd.setCursor(0,2); lcd.print("TDS      : ");
        if (analogRead(TdsSensorPin) == 0) lcd.print("OFF");
        else if (tdsValue < 1) lcd.print("0");
        else { lcd.print(tdsValue,0); lcd.print(" ppm"); }

        lcd.setCursor(0,3); lcd.print("AC Volt  : ");
        if (acVolt == -1) lcd.print("OFF");
        else if (acVolt == 0) lcd.print("0");
        else { lcd.print(acVolt,0); lcd.print(" V"); }

    // ===================================
    // Halaman 1: Udara T, Humidity, Lux 
    // ===================================
    } else if (page == 1) { 
        lcd.setCursor(0,0); lcd.print("Udara T  : ");
        if (isnan(airTempUdara)) lcd.print("OFF");
        else if (airTempUdara < -20 || airTempUdara > 80) lcd.print("0");
        else { lcd.print(airTempUdara,1); lcd.print(" C"); }

        lcd.setCursor(0,1); lcd.print("Humidity : ");
        if (isnan(hum)) lcd.print("OFF");
        else if (hum < 1) lcd.print("0");
        else {lcd.print(hum,0); lcd.print(" %"); }

        lcd.setCursor(0,2); lcd.print("Lux      : ");
        if (!bh1750_ok) lcd.print("OFF");
        else if (lux == 0) lcd.print("0");
        else { lcd.print(lux); lcd.print(" lx"); }
        
        // Baris 3 kosong
        lcd.setCursor(0,3); lcd.print("                 "); 

    // ===================================
    // Halaman 2: Flow Water dan Jarak
    // ===================================
    } else if (page == 2) { 
        lcd.setCursor(0,0); lcd.print("Water Flow : ");
        if (waterflowValue < 0) lcd.print("OFF");
        else { lcd.print(waterflowValue, 2); lcd.print(" L"); }

        lcd.setCursor(0,1); lcd.print("Jarak      : ");
        if (jarak <= 0 || jarak > 400) lcd.print("0");
        else lcd.print(jarak,0); lcd.print(" cm");
        
        // Baris 2 & 3 dikosongkan
        lcd.setCursor(0,2); lcd.print("                 "); 
        lcd.setCursor(0,3); lcd.print("                 "); 
    }
}


void loop() {
    // 1. Menerima Perintah Serial (Non-blocking / Cepat)
    handleIncomingSerialCommands();
    update_waterflow(); // Flow water polling dipanggil di setiap loop

    // 3. Membaca dan Mengirim Data Sensor (Setiap 2 detik)
    if (millis() - lastSensorRead >= sensorInterval) {
        lastSensorRead = millis();

        nilai_analog_PH = analogRead(ph_Pin);
        TeganganPh = nilai_analog_PH * 5.0 / 1023.0;
        float offset = 0.75; 

        // Hitung kemiringan (slope)
        // PH_REF_7=7.0, PH_REF_4=4.0, PH_REF_9=9.0 dari deklarasi
        float PH_step_low = (PH7 - PH4) / (6.8 - 4.0); 
        float PH_step_high = (PH9 - PH7) / (9.2 - 6.8); 

        if (TeganganPh <= PH7) {
            // Rentang Asam/Netral
            pHValue = 6.8 + ((TeganganPh - PH7) / PH_step_low);
            
        } else {
            // Rentang Basa
            pHValue = 6.8 + ((TeganganPh - PH7) / PH_step_high);
            
        }
        
        // Terapkan koreksi offset
        pHValue = pHValue - offset;

        // ... (Logika filter pH)
        const int PH_SAMPLES = 10; 
        static float phBuffer[PH_SAMPLES];
        static int phIndex = 0;
        static bool phBufferFull = false;

        phBuffer[phIndex++] = pHValue;
        if (phIndex >= PH_SAMPLES) {
            phIndex = 0;
            phBufferFull = true;
        }

        float phSum = 0;
        int phCount = phBufferFull ? PH_SAMPLES : phIndex;
        for (int i = 0; i < phCount; i++) phSum += phBuffer[i];
        float pHFiltered = phSum / phCount;
        pHValue = pHFiltered;

        // === TDS ===
        float suhuAirUntukTDS = airTemp;
        if (airTemp == DEVICE_DISCONNECTED_C || airTemp < 0 || airTemp > 50) suhuAirUntukTDS = 30;
        gravityTds.setTemperature(suhuAirUntukTDS);
        gravityTds.update();
        float tdsRaw = gravityTds.getTdsValue();

        // ... (Logika filter TDS)
        const int TDS_SAMPLES = 10;
        static float tdsBuffer[TDS_SAMPLES];
        static int tdsIndex = 0;
        static bool bufferFilled = false;
        float tdsSum = 0;

        tdsBuffer[tdsIndex] = tdsRaw;
        tdsIndex = (tdsIndex + 1) % TDS_SAMPLES;
        if (tdsIndex == 0) bufferFilled = true;

        int count = bufferFilled ? TDS_SAMPLES : tdsIndex;
        for (int i = 0; i < count; i++) tdsSum += tdsBuffer[i];
        float tdsFiltered = tdsSum / count;

        static float lastTds = 0;
        if (abs(tdsFiltered - lastTds) > 5) lastTds = tdsFiltered;

        const float A = 0.7700;
        const float B = 160.0;
        float tds_corrected = (A * lastTds) + B;
        if (tds_corrected < 0) tds_corrected = 0;
        tdsValue = tds_corrected;

        // === DS18B20 ===
        sensors.requestTemperatures();
        airTemp = (sensors.getTempCByIndex(0)) + 2 ;

        // === DHT22  ===
        airTempUdara = dht.readTemperature() + 1;
        hum = dht.readHumidity() - 14;

        // === BH1750  ===
        if (bh1750_ok) lux = lightMeter.readLightLevel();

        // === HC-SR04  ===
        digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
        digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
        digitalWrite(TRIG_PIN, LOW);
        duration = pulseIn(ECHO_PIN, HIGH);
        jarak = duration * 0.034 / 2;

        // === ZMPT101B  ===
        int nilai_adc = analogRead(ZMPT_PIN);
        acVolt = map(nilai_adc, 0, 700, 0, 220);

        // === Kirim data ke ESP32 (CSV) ===
        Serial.print(pHValue); Serial.print(",");
        Serial.print(tdsValue); Serial.print(",");
        Serial.print(airTemp); Serial.print(",");
        Serial.print(airTempUdara); Serial.print(",");
        Serial.print(hum); Serial.print(",");
        Serial.print(lux); Serial.print(",");
        Serial.print(jarak); Serial.print(",");
        Serial.print(waterflowValue); Serial.print(","); 
        Serial.println(acVolt);
    }
    
    updateLcd(); // Update LCD
}

// ======== Fungsi penerima perintah serial & Averaging ========
void handleIncomingSerialCommands() {
    static String line = "";
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (line.length() > 0) {
                processCommand(line);
                line = "";
            }
        } else {
            line += c;
            if (line.length() > 200) line = ""; 
        }
    }
}

void processCommand(const String &cmd) {
    String s = cmd;
    s.trim();
    if (!s.startsWith("CTRL")) return;
    int p1 = s.indexOf(',');
    int p2 = s.indexOf(',', p1 + 1);
    if (p1 < 0 || p2 < 0) return;
    String chStr = s.substring(p1+1, p2);
    String valStr = s.substring(p2+1);
    int ch = chStr.toInt();
    int val = valStr.toInt();

    uint8_t pin = 255;
    if (ch == 1) pin = RELAY_CH1;
    else if (ch == 2) pin = RELAY_CH2;
    else if (ch == 3) pin = RELAY_CH3;
    else if (ch == 4) pin = RELAY_CH4;
    else return;

    if (RELAY_ACTIVE_LOW) {
        digitalWrite(pin, val ? LOW : HIGH);
    } else {
        digitalWrite(pin, val ? HIGH : LOW);
    }
}

double avergearray(int* arr, int number){
    int i;
    int max,min;
    double avg;
    long amount = 0;
    if(number <= 0) return 0;
    if(number < 5){
        for(i=0;i<number;i++) amount += arr[i];
        avg = (double)amount / number;
        return avg;
    } else {
        if(arr[0]<arr[1]){ min=arr[0]; max=arr[1]; }
        else { min=arr[1]; max=arr[0]; }
        for(i=2;i<number;i++){
            if(arr[i]<min){ amount+=min; min=arr[i]; }
            else if(arr[i]>max){ amount+=max; max=arr[i]; }
            else { amount+=arr[i]; }
        }
        avg = (double)amount / (number-2);
    }
    return avg;
}