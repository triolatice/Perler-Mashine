/*
======================================================
  Perler Machine v2.15 - 2/10/2026   
  - English comments applied.
  - Dual-Profile Geometry (24 slots vs 8 slots).
  - Profile selection via ENC_PUSH (Encoder click) on startup.
  - Reset menu profile immediately upon work completion.
======================================================
*/
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include <ESP32Servo.h>
#include <Wire.h>
#include "Freenove_WS2812_Lib_for_ESP32.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Preferences.h> 
#include <OneButton.h>

// ---- DEBUG CONFIGURATION ----
const bool DEBUG = true; 

// ---- SPI / SD CARD PINS ----
#define REASSIGN_PINS
const int sck = 18;
const int miso = 47;
const int mosi = 38;
const int cs = 17;

// ---- ENCODER AND BUTTON PINS ----
#define CONFIRM 21
#define ENC_PUSH 9
#define ENC_A    8
#define ENC_B    7
#define BACK     6

// ---- RGB LED (WS2812) ----
#define LED_PIN       10
#define LED_COUNT     1   
#define LED_CHANNEL   3   
Freenove_ESP32_WS2812 strip(LED_COUNT, LED_PIN, LED_CHANNEL, TYPE_GRB);

// ---- INTERNAL LED AND BUZZER ----
#define LED_BLUE 45
#define LED_RED 46
#define BUZZER_PIN 48     

// ---- SERVO MOTOR ----
#define PIN_Servo1 5
Servo servo1;

// ---- HARDWARE I2C (SH1106 DISPLAY) ----
#define SDA_PIN 11 
#define SCL_PIN 12 
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---- STEPPER MOTOR (TMC2208) ----
#define EN1_PIN 4
#define STEP1_PIN 13
#define DIR1_PIN 14

// ---- IR AND TOUCH SENSORS ----
#define IR_SENSOR_1 1            
#define Touch_Switch1 3
#define Touch_Switch2 2  

// ---- DYNAMIC SD PROJECT DIRECTORIES (Add to global variables) ----
String projekti[30];      
int ukupnoProjekata = 0;      
int odabraniProjektIdx = 0;   


// ---- HARDWARE GEOMETRY PROFILES ----
const int TOTAL_STEPS_PUNI_KRUG = 3200; 

// Profile A: Standard 24 positions table
const int koraciProfil24[] = {
  0,    134,   267,  400,  533,  667,  800,  933,
 1067,  1200,  1333,  1466,  1600,  1733,  1867,  2000,
 2133, 2267, 2400, 2533, 2666, 2799, 2933, 3066  
};

// Profile B: New 8 positions table
const int koraciProfil8[] = {
  0, 400, 800, 1200, 1600, 2000, 2400, 2800
};

const int* koraciZaBoje; 
int maxDostupnihBoja = 24; 

// ---- COLOR DATA STRUCTURE ----
struct PodaciOBoje {
    int indeks = -1;
    uint8_t r = 0, g = 0, b = 0;
    String naziv = "";
};
PodaciOBoje tablicaBoja[24]; 


// ---- SYSTEM STATE MACHINE ----
enum StanjeSustava {
  STANJE_IZBOR_PROJEKTA,
  STANJE_IZBORNIK,
  STANJE_PODESAVANJE_OFFSET,
  STANJE_PODESAVANJE_SERVA_OTV,
  STANJE_PODESAVANJE_SERVA_ZAT,
  STANJE_HOMING,
  STANJE_RAD
};
StanjeSustava trenutnoStanje = STANJE_IZBOR_PROJEKTA;


File imagesFile;
int trenutniRed = 1;      
int trenutniStupac = 1;   
int trenutnaPozicijaStepera = 0; 
int praznihMjestaUNizu = 0;
bool krajRedaDetektiran = false; 

volatile long pozicijaEnkodera = 0;

OneButton tasterEnkodera(ENC_PUSH, true);
OneButton tasterSlijedeca(Touch_Switch1, false, false); 
OneButton tasterPreskoci(Touch_Switch2, false, false);  
OneButton tasterNazad(BACK, true); 

Preferences preferences;      
Preferences infoMemorija;    

int homingOffsetKoraci = 300;
int servoOtvorenoKut = 90;
int servoZatvorenoKut = 10;

int trenutnaStavkaIzbornika = 0;
int ukupnoStavkiIzbornika = 4; 
bool imaSpremljenogNapretka = false;

String stavkeIzbornika[] = {
  "1. STEPER OFFSET",
  "2. Servo START.",
  "3. Servo END.",
  "4. RUN",
  ""
};

bool osvjeziEkran = true;
bool flagSlijedecaPerlica = false;
bool flagPreskociPerlicu = false;
bool pokreniSaPremotavanjem = false;
bool uartPozdravPoslan = false; 

// ---- FUNCTION PROTOTYPES ----
void IRAM_ATTR azurirajEnkoder();
void klikSlijedeca();
void klikPreskoci(); 
void klikNaEnkoder();
void klikNaNazad();

void ucitajInformacijeOBoja();
void izvrsiHomingKoracnogMotora();
void prikaziNaEkranu(String linija1, String linija2, String linija3);
void prikaziSveLED(uint8_t r, uint8_t g, uint8_t b);
void kratkiBip();
void dugiBip();
void provjeriImaliNastavka();
void premotajNastavakSlaganja();
void pozicionirajSteperNaBoju(int ciljniIndeks);
void aktivirajServoDozatore();
void prikaziIzbornik();
void upravljajRadomStroja();
void pretvoriHexURGB(String hex, uint8_t &r, uint8_t &g, uint8_t &b);

void azurirajEnkoder() {
    int stanjeA = digitalRead(ENC_A);
    int stanjeB = digitalRead(ENC_B);
    if (stanjeA == stanjeB) {
        pozicijaEnkodera = pozicijaEnkodera - 1;
    } else {
        pozicijaEnkodera = pozicijaEnkodera + 1;
    }
}



void  klikSlijedeca() {
  if (trenutnoStanje == STANJE_RAD) {
    flagSlijedecaPerlica = true;
  }
}

void  klikPreskoci() {
  if (trenutnoStanje == STANJE_RAD) {
    flagPreskociPerlicu = true;
  }
}


// ---- ROTARY ENCODER CONFIRM CLICK HUB ----
void klikNaEnkoder() {
  if (trenutnoStanje == STANJE_IZBOR_PROJEKTA) {
    if (ukupnoProjekata == 0) return; 
    
    ucitajInformacijeOBoja();
    provjeriImaliNastavka();
    
    trenutnoStanje = STANJE_IZBORNIK;
    pozicijaEnkodera = 3 * 2; 
    trenutnaStavkaIzbornika = 3;
    osvjeziEkran = true;
    kratkiBip();
  } 
  else if (trenutnoStanje == STANJE_IZBORNIK) {
    if (trenutnaStavkaIzbornika == 0) {
      trenutnoStanje = STANJE_PODESAVANJE_OFFSET;
      pozicijaEnkodera = homingOffsetKoraci;
    } else if (trenutnaStavkaIzbornika == 1) {
      trenutnoStanje = STANJE_PODESAVANJE_SERVA_OTV;
      pozicijaEnkodera = servoOtvorenoKut;
    } else if (trenutnaStavkaIzbornika == 2) {
      trenutnoStanje = STANJE_PODESAVANJE_SERVA_ZAT;
      pozicijaEnkodera = servoZatvorenoKut;
    } else if (trenutnaStavkaIzbornika == 3) {
      pokreniSaPremotavanjem = imaSpremljenogNapretka; 
      trenutnoStanje = STANJE_HOMING;
    } else if (trenutnaStavkaIzbornika == 4) {
      String namespaceIme = "p_" + projekti[odabraniProjektIdx];
      if (namespaceIme.length() > 15) namespaceIme = namespaceIme.substring(0, 15);
      
      infoMemorija.begin(namespaceIme.c_str(), false);
      infoMemorija.clear();
      infoMemorija.end();
      
      imaSpremljenogNapretka = false;
      pokreniSaPremotavanjem = false;
      
      if (imagesFile) {
          imagesFile.close();
      }
      trenutniRed = 1;
      trenutniStupac = 1;
      praznihMjestaUNizu = 0;
      krajRedaDetektiran = false;
      
      trenutnoStanje = STANJE_HOMING;
    }
    osvjeziEkran = true;
  } 
  else if (trenutnoStanje == STANJE_PODESAVANJE_OFFSET) {
    homingOffsetKoraci = pozicijaEnkodera;
    preferences.begin("perler", false);
    preferences.putInt("offset", homingOffsetKoraci);
    preferences.end();
    trenutnoStanje = STANJE_IZBORNIK;
    pozicijaEnkodera = 0;
    osvjeziEkran = true;
  } else if (trenutnoStanje == STANJE_PODESAVANJE_SERVA_OTV) {
    servoOtvorenoKut = pozicijaEnkodera;
    preferences.begin("perler", false);
    preferences.putInt("servoOtv", servoOtvorenoKut);
    preferences.end();
    trenutnoStanje = STANJE_IZBORNIK;
    pozicijaEnkodera = 1 * 2; 
    osvjeziEkran = true;
  } else if (trenutnoStanje == STANJE_PODESAVANJE_SERVA_ZAT) {
    servoZatvorenoKut = pozicijaEnkodera;
    preferences.begin("perler", false);
    preferences.putInt("servoZat", servoZatvorenoKut);
    preferences.end();
    trenutnoStanje = STANJE_IZBORNIK;
    pozicijaEnkodera = 2 * 2; 
    osvjeziEkran = true;
  }
}

// ---- BACK BUTTON ACTION HUB ----
void klikNaNazad() {
  if (trenutnoStanje == STANJE_RAD) {
    if (DEBUG) Serial.println("Work paused. Returning to menu.");
    prikaziSveLED(255, 255, 0); 
    kratkiBip();
    trenutnoStanje = STANJE_IZBORNIK;
    pozicijaEnkodera = 3 * 2; 
    trenutnaStavkaIzbornika = 3;
    osvjeziEkran = true;
  } 
  else if (trenutnoStanje == STANJE_IZBORNIK) {
    trenutnoStanje = STANJE_IZBOR_PROJEKTA;
    pozicijaEnkodera = odabraniProjektIdx * 2;
    osvjeziEkran = true;
    kratkiBip();
  }
}



void setup() {
    pinMode(LED_BLUE, OUTPUT); pinMode(LED_RED, OUTPUT);
    digitalWrite(LED_BLUE, LOW); digitalWrite(LED_RED, LOW);
    pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);

    pinMode(EN1_PIN, OUTPUT); pinMode(STEP1_PIN, OUTPUT); pinMode(DIR1_PIN, OUTPUT);
    digitalWrite(EN1_PIN, HIGH); digitalWrite(DIR1_PIN, LOW);

    pinMode(IR_SENSOR_1, INPUT_PULLUP);
    pinMode(Touch_Switch1, INPUT_PULLUP); 
    pinMode(Touch_Switch2, INPUT_PULLUP);
    
    pinMode(ENC_A, INPUT_PULLUP);
    pinMode(ENC_B, INPUT_PULLUP);
    pinMode(BACK, INPUT_PULLUP); 
    pinMode(ENC_PUSH, INPUT_PULLUP); // Dynamic hardware pin init

    Serial1.begin(115200, SERIAL_8N1, 44, 43); 
    if (DEBUG) Serial.begin(115200);
    delay(200);

    Wire.setPins(SDA_PIN, SCL_PIN); 
    Wire.begin();
    
    display.begin(0x3C, true);
    display.setRotation(0); 
    display.clearDisplay();
    
    // ---- PROFILE SELECTION AT STARTUP (ENC_PUSH) ----
    if (digitalRead(ENC_PUSH) == HIGH) {
        koraciZaBoje = koraciProfil8; 
        maxDostupnihBoja = 8;
        prikaziNaEkranu("PERLER MASHINE v3.03", "PROFIL: 8 Colors", "Read SD Card...");
        digitalWrite(LED_RED, HIGH); delay(600); digitalWrite(LED_RED, LOW);
    } else {
        koraciZaBoje = koraciProfil24; 
        maxDostupnihBoja = 24;
        prikaziNaEkranu("PERLER MASHINE v3.03", "PROFIL: 24 Colors", "Read SD...");
        delay(300);
    }

    strip.begin();
    strip.setBrightness(20);
    prikaziSveLED(255, 255, 0); 

    preferences.begin("perler", true);
    homingOffsetKoraci = preferences.getInt("offset", 300); 
    servoOtvorenoKut = preferences.getInt("servoOtv", 90);
    servoZatvorenoKut = preferences.getInt("servoZat", 10);
    preferences.end();

    servo1.attach(PIN_Servo1); 
    servo1.write(servoZatvorenoKut); 

    // pinMode(miso, INPUT_PULLUP);
    SPI.begin(sck, miso, mosi, cs);
    if (!SD.begin(cs, SPI, 4000000)) {
        prikaziNaEkranu("SD ERROR", "Check the SD Card ","");
        digitalWrite(LED_RED, HIGH);
        while (true);
    }
    // test only

     uint8_t cardType = SD.cardType();
        if(cardType == CARD_NONE){
     if (DEBUG)   Serial.println("No SD card attached");
        return;
    }

    Serial.print("SD Card Type: ");
    if(cardType == CARD_MMC){
        Serial.println("MMC");
    } else if(cardType == CARD_SD){
        Serial.println("SDSC");
    } else if(cardType == CARD_SDHC){
        Serial.println("SDHC");
    } else {
        Serial.println("UNKNOWN");
    }



    // Connect OneButton library events safely
    tasterEnkodera.attachClick(klikNaEnkoder);
    tasterSlijedeca.attachClick(klikSlijedeca);
    tasterPreskoci.attachClick(klikPreskoci);
    tasterNazad.attachClick(klikNaNazad); 

    attachInterrupt(digitalPinToInterrupt(ENC_A), azurirajEnkoder, CHANGE);

    // Scan for available project folders inside SD card root directory
    skenirajSDMape();

    kratkiBip();
    osvjeziEkran = true;
}



void loop() {
  tasterEnkodera.tick();
  tasterSlijedeca.tick();
  tasterPreskoci.tick();
  tasterNazad.tick(); 

  switch (trenutnoStanje) {
    
    case STANJE_IZBOR_PROJEKTA: {
      long pozicija = pozicijaEnkodera / 2;
      if (pozicija != odabraniProjektIdx) {
        if (pozicija < 0) { pozicijaEnkodera = 0; pozicija = 0; }
        if (pozicija >= ukupnoProjekata) { pozicijaEnkodera = (ukupnoProjekata - 1) * 2; pozicija = ukupnoProjekata - 1; }
        odabraniProjektIdx = pozicija;
        osvjeziEkran = true;
      }
      if (osvjeziEkran) {
        prikaziIzborProjekata();
        osvjeziEkran = false;
      }
      break;
    }

    case STANJE_IZBORNIK: {
      long pozicija = pozicijaEnkodera / 2; 
      if (pozicija != trenutnaStavkaIzbornika) {
        if (pozicija < 0) { pozicijaEnkodera = 0; pozicija = 0; }
        if (pozicija >= ukupnoStavkiIzbornika) { pozicijaEnkodera = (ukupnoStavkiIzbornika - 1) * 2; pozicija = ukupnoStavkiIzbornika - 1; }
        trenutnaStavkaIzbornika = pozicija;
        osvjeziEkran = true;
      }
      if (osvjeziEkran) {
        prikaziIzbornik();
        osvjeziEkran = false;
      }
      break;
    }

    case STANJE_PODESAVANJE_OFFSET: {
      int trenutniOffset = pozicijaEnkodera; 
      if (trenutniOffset < 0) { pozicijaEnkodera = 0; trenutniOffset = 0; }
      if (trenutniOffset > 2000) { pozicijaEnkodera = 2000; trenutniOffset = 2000; } 
      
      static int zadnjiOffset = -1;
      if (trenutniOffset != zadnjiOffset || osvjeziEkran) {
        display.clearDisplay();
        display.setTextColor(SH110X_WHITE);
        display.setCursor(0, 10);
        display.println("PODESI STEPER OFFSET:");
        display.setTextSize(2);
        display.setCursor(20, 30);
        display.print(trenutniOffset);  display.print(" kor.");
        display.setTextSize(1); display.display();
        zadnjiOffset = trenutniOffset;
        osvjeziEkran = false;
      }
      break;
    }

    case STANJE_PODESAVANJE_SERVA_OTV: {
      int trenutniKut = pozicijaEnkodera;
      if (trenutniKut < 0) { pozicijaEnkodera = 0; trenutniKut = 0; }
      if (trenutniKut > 180) { pozicijaEnkodera = 180; trenutniKut = 180; }
      servo1.write(trenutniKut);
      
      static int zadnjiKut = -1;
      if (trenutniKut != zadnjiKut || osvjeziEkran) {
        display.clearDisplay();
        display.setTextColor(SH110X_WHITE);
        display.setCursor(0, 10);
        display.println("SET SERVO START:");
        display.setTextSize(2);
        display.setCursor(30, 30);
        display.print(trenutniKut); display.print(" deg");
        display.setTextSize(1); display.display();
        zadnjiKut = trenutniKut;
        osvjeziEkran = false;
      }
      break;
    }

    case STANJE_PODESAVANJE_SERVA_ZAT: {
      int trenutniKut = pozicijaEnkodera;
      if (trenutniKut < 0) { pozicijaEnkodera = 0; trenutniKut = 0; }
      if (trenutniKut > 180) { pozicijaEnkodera = 180; trenutniKut = 180; }
      servo1.write(trenutniKut);
      
      static int zadnjiKut = -1;
      if (trenutniKut != zadnjiKut || osvjeziEkran) {
        display.clearDisplay();
        display.setTextColor(SH110X_WHITE);
        display.setCursor(0, 10);
        display.println("SET SERVO END:");
        display.setTextSize(2);
        display.setCursor(30, 30);
        display.print(trenutniKut); display.print(" deg");
        display.setTextSize(1); display.display();
        zadnjiKut = trenutniKut;
        osvjeziEkran = false;
      }
      break;
    }

    case STANJE_HOMING: {
      prikaziNaEkranu("HOMING", "Executing.....", "Please wait");
      izvrsiHomingKoracnogMotora();
      
      if (!imagesFile) {
          String imagePath = "/" + projekti[odabraniProjektIdx] + "/Images.csv";
          imagesFile = SD.open(imagePath);
          if (!imagesFile) {
              prikaziNaEkranu("IMAGES ERROR",  "Images.csv", "missing");
              while (true);
          }
          
          if (pokreniSaPremotavanjem) {
              premotajNastavakSlaganja();
          } else {
              trenutniRed = 1;
              trenutniStupac = 1;
              krajRedaDetektiran = false;
          }
      } 
      kratkiBip();
      trenutnoStanje = STANJE_RAD;
      osvjeziEkran = true;
      break;
    }

    case STANJE_RAD: {
      upravljajRadomStroja();
      break;
    }
  }
}


void izvrsiHomingKoracnogMotora() {
    digitalWrite(EN1_PIN, LOW); 
    digitalWrite(DIR1_PIN, HIGH); 
    
    while(digitalRead(IR_SENSOR_1) == LOW) {
        digitalWrite(STEP1_PIN, HIGH); delayMicroseconds(1000);
        digitalWrite(STEP1_PIN, LOW);  delayMicroseconds(1000);
    }
    delay(200);
    
    digitalWrite(DIR1_PIN, LOW); 
    for (int i = 0; i < homingOffsetKoraci; i++) {
        digitalWrite(STEP1_PIN, HIGH); delayMicroseconds(800);
        digitalWrite(STEP1_PIN, LOW);  delayMicroseconds(800);
    }
    trenutnaPozicijaStepera = 0;
    if (DEBUG) Serial.println("Hardware homing over IR1 accomplished.");
}

void pozicionirajSteperNaBoju(int ciljniIndeks) {
    if (ciljniIndeks < 0 || ciljniIndeks > (maxDostupnihBoja - 1)) return;

    int ciljnaPozicija = koraciZaBoje[ciljniIndeks];
    int razlika = ciljnaPozicija - trenutnaPozicijaStepera;

    if (razlika > TOTAL_STEPS_PUNI_KRUG / 2) {
        razlika -= TOTAL_STEPS_PUNI_KRUG; 
    } else if (razlika < -TOTAL_STEPS_PUNI_KRUG / 2) {
        razlika += TOTAL_STEPS_PUNI_KRUG; 
    }

    // Serial1.print("Idx: ");          Serial1.print(ciljniIndeks);
    // Serial1.print(" | Step Target: "); Serial1.print(ciljnaPozicija);
    // Serial1.print(" | Real Diff: ");  Serial1.println(razlika);

    if (razlika == 0) return;

    if (razlika > 0) {
        digitalWrite(DIR1_PIN, HIGH);
    } else {
        digitalWrite(DIR1_PIN, LOW);
        razlika = -razlika; 
    }

    digitalWrite(EN1_PIN, LOW);
    for (int i = 0; i < razlika; i++) {
        digitalWrite(STEP1_PIN, HIGH); 
        delayMicroseconds(1000); //1500
        digitalWrite(STEP1_PIN, LOW);  
        delayMicroseconds(1000);  // 1500 
    }
    
    trenutnaPozicijaStepera = ciljnaPozicija;
}

void aktivirajServoDozatore() {
    servo1.write(servoOtvorenoKut); 
    delay(400); 
    servo1.write(servoZatvorenoKut); 
    delay(200);
}






void ucitajInformacijeOBoja() {
    String configPath = "/" + projekti[odabraniProjektIdx] + "/Information.csv";
    File configFile = SD.open(configPath);
    if (!configFile) {
        prikaziNaEkranu("CONFIG ERROR", "Information.csv", "missing .");
        while (true);
    }

    int brojac = 0;
    while (configFile.available() && brojac < maxDostupnihBoja) {
        String linija = configFile.readStringUntil('\n');
        linija.trim();
        if (linija.length() == 0) continue;

        int prvaZarez = linija.indexOf(',');
        int drugaZarez = linija.indexOf(',', prvaZarez + 1);

        if (prvaZarez != -1 && drugaZarez != -1) {
            tablicaBoja[brojac].indeks = linija.substring(0, prvaZarez).toInt();
            tablicaBoja[brojac].naziv = linija.substring(drugaZarez + 1);
            tablicaBoja[brojac].naziv.trim();
            
            String hexBoje = linija.substring(prvaZarez + 1, drugaZarez);
            pretvoriHexURGB(hexBoje, tablicaBoja[brojac].r, tablicaBoja[brojac].g, tablicaBoja[brojac].b);
            brojac++;
        }
    }
    configFile.close();
}

// ---- CSV AUTOMATIC RESUME PROGRESS LOADER ----
void provjeriImaliNastavka() {
    infoMemorija.begin("perler_data", true); 
    int spremljeniRed = infoMemorija.getInt("zadnjiRed", 1);
    int spremljeniStupac = infoMemorija.getInt("zadnjiStupac", 1);
    infoMemorija.end();

    // FIXED: Corrected matrix array boundaries for menu initialization
    if (spremljeniRed > 1 || spremljeniStupac > 1) {
        imaSpremljenogNapretka = true;
        stavkeIzbornika[3] = "4. CONTINUE WORK"; 
        stavkeIzbornika[4] = "5. START NEW WORK"; 
        ukupnoStavkiIzbornika = 5;
    } else {
        imaSpremljenogNapretka = false;
        stavkeIzbornika[3] = "4. RUN";
        ukupnoStavkiIzbornika = 4;
    }
}

void premotajNastavakSlaganja() {
    infoMemorija.begin("perler_data", true); 
    int metaRed = infoMemorija.getInt("zadnjiRed", 1);
    int metaStupac = infoMemorija.getInt("zadnjiStupac", 1);
    infoMemorija.end();

    prikaziNaEkranu("RESUME WORK...", "Wait CSV info...", "R: " + String(metaRed) + " C: " + String(metaStupac));
    
    trenutniRed = 1;
    trenutniStupac = 1;
    krajRedaDetektiran = false;

    while (imagesFile.available() && (trenutniRed < metaRed || (trenutniRed == metaRed && trenutniStupac < metaStupac))) {
        char c = imagesFile.read();
        if (c == ',') {
            trenutniStupac++;
        } else if (c == '\n') {
            trenutniRed++;
            trenutniStupac = 1;
        }
    }
}

// ---- MAIN BEAD DISPENSING MACHINE LOOP ----
void upravljajRadomStroja() {
    if (!uartPozdravPoslan) {
        Serial1.println("PERLER");
        uartPozdravPoslan = true;
    }

    if (trenutnoStanje != STANJE_RAD) return;

    if (imagesFile.available()) {
        String stavka = "";
        char c = 0;
        bool isNewLine = false; // Local flag to detect end of line for this specific token

        // Read characters until we hit a comma or a newline
        while (imagesFile.available()) {
            tasterNazad.tick();
            if (trenutnoStanje != STANJE_RAD) return;

            c = imagesFile.read();
            if (c == '\n') {
                isNewLine = true;
                break;
            }
            if (c == ',') {
                break;
            }
            if (c != '\r' && c != ' ') stavka += c;
        }
        stavka.trim();

        // 1. HANDLE EMPTY CELLS (GAPS)
        if (stavka.length() == 0) {
            // Count as a gap only if it is NOT a trailing empty cell at the very end of a line
            if (!isNewLine) {
                praznihMjestaUNizu++;
            }
        } 
        // 2. HANDLE VALID OR INVALID COLOR TOKENS
        else {
            // If there are accumulated gaps before this bead, process them now via Touch2
            if (praznihMjestaUNizu > 0) {
                prikaziNaEkranu("Pick up the bead.  R:" + String(trenutniRed) + " C:" + String(trenutniStupac), "Skip Empty", String(praznihMjestaUNizu) + "Press [Touch 2]");
                prikaziSveLED(255, 0, 0); 
                
                Serial1.println("S" + String(praznihMjestaUNizu));

                flagPreskociPerlicu = false;
                while (!flagPreskociPerlicu && trenutnoStanje == STANJE_RAD) {
                    tasterPreskoci.tick();
                    tasterNazad.tick(); 
                    delay(10);
                }
                if (trenutnoStanje != STANJE_RAD) return;
                flagPreskociPerlicu = false;

                trenutniStupac += praznihMjestaUNizu; 
                praznihMjestaUNizu = 0; // Clear accumulated gaps
            }

            int trazeniIndeks = stavka.toInt();
            bool pronadjena = false;
            PodaciOBoje trenutnaBoja;
            
            for (int i = 0; i < maxDostupnihBoja; i++) {
                if (tablicaBoja[i].indeks == trazeniIndeks && tablicaBoja[i].indeks != -1) {
                    trenutnaBoja = tablicaBoja[i];
                    pronadjena = true;
                    break;
                }
            }

            if (pronadjena) {
                if (DEBUG) Serial.printf("Color match: [%d] %s\n", trenutnaBoja.indeks, trenutnaBoja.naziv.c_str());
                
                prikaziNaEkranu(trenutnaBoja.naziv + " [" + String(trazeniIndeks) + "]", "Positioning...", "Row: " + String(trenutniRed) + "  Column: " + String(trenutniStupac));
                prikaziSveLED(0, 255, 0); 
                kratkiBip();

                pozicionirajSteperNaBoju(trenutnaBoja.indeks);
                
                prikaziNaEkranu(trenutnaBoja.naziv + " [" + String(trazeniIndeks) + "]", "[Touch 1] for next.", "Row: " + String(trenutniRed) + "  Column: " + String(trenutniStupac));
                Serial1.println("R" + String(trenutniRed) + "C" + String(trenutniStupac));

                flagSlijedecaPerlica = false;
                while (!flagSlijedecaPerlica && trenutnoStanje == STANJE_RAD) {
                    tasterSlijedeca.tick();
                    tasterNazad.tick(); 
                    if (digitalRead(IR_SENSOR_1) == LOW) digitalWrite(LED_BLUE, HIGH);
                    else digitalWrite(LED_BLUE, LOW);
                    delay(10);
                }
                if (trenutnoStanje != STANJE_RAD) return;
                flagSlijedecaPerlica = false;
                
                prikaziNaEkranu(trenutnaBoja.naziv + " [" + String(trazeniIndeks) + "]", "Dropping the bead", "Row: " + String(trenutniRed) + "  Column: " + String(trenutniStupac));
                aktivirajServoDozatore();  
            } 
            else {
                digitalWrite(LED_RED, HIGH); 
                prikaziNaEkranu("UNKNOWN COLOR!", "Indeks mising: " + String(trazeniIndeks), "[Touch 2] to skip");
                prikaziSveLED(255, 0, 0); 
                dugiBip();
                
                flagPreskociPerlicu = false;
                while (!flagPreskociPerlicu && trenutnoStanje == STANJE_RAD) {
                    tasterPreskoci.tick();
                    tasterNazad.tick(); 
                    delay(10);
                }
                if (trenutnoStanje != STANJE_RAD) return;
                flagPreskociPerlicu = false;
                
                digitalWrite(LED_RED, LOW);
            }

            // Increment column only after a real bead (valid or invalid) has been processed
            trenutniStupac++; 
        }

        // 3. CENTRALIZED NEWLINE GATEWAY (Executes at the end of the line)
        if (isNewLine) {
            if (DEBUG) Serial.println("--- End of CSV file line reached ---");
            trenutniRed++;
            trenutniStupac = 1; 
            praznihMjestaUNizu = 0; // CRITICAL FIX: Erase any carryover gaps immediately!
            
            infoMemorija.begin("perler_data", false);
            infoMemorija.putInt("zadnjiRed", trenutniRed);
            infoMemorija.putInt("zadnjiStupac", trenutniStupac);
            infoMemorija.end();
            dugiBip();
        } else {
            // Standard progressive saving within the same line
            infoMemorija.begin("perler_data", false);
            infoMemorija.putInt("zadnjiRed", trenutniRed);
            infoMemorija.putInt("zadnjiStupac", trenutniStupac);
            infoMemorija.end();
        }

    } else {
        // --- OPERATION COMPLETED IN FULL ---
        if (DEBUG) Serial.println("Job accomplished! Waiting for exit command.");
        prikaziNaEkranu("COMPLETED!", "All beads arranged", "[BACK] to menu");
        prikaziSveLED(0, 0, 255); 
        
        digitalWrite(EN1_PIN, HIGH); 
        
        infoMemorija.begin("perler_data", false);
        infoMemorija.clear();
        infoMemorija.end();
        
        // FIXED: Instantly refresh menu profile boundaries upon completion
        provjeriImaliNastavka();
        
        imagesFile.close(); 
        uartPozdravPoslan = false; 
        
        while (trenutnoStanje == STANJE_RAD) {
            tasterNazad.tick();
            tasterEnkodera.tick();
            delay(10);
        }
    }
}

// ---- HELPER DATA AND UI RENDERING FUNCTIONS ----
void prikaziIzbornik() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.println("- GLAVNI IZBORNIK -"); 
  display.println("");
  for (int i = 0; i < ukupnoStavkiIzbornika; i++) {
    if (i == trenutnaStavkaIzbornika) display.print("> "); 
    else display.print("  ");
    display.println(stavkeIzbornika[i]);
  }
  display.display();
}

void prikaziNaEkranu(String linija1, String linija2, String linija3) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE); 
    display.setCursor(0, 4);   display.println(linija1);
    display.setCursor(0, 24);  display.println(linija2);
    display.setCursor(0, 44);  display.println(linija3);
    display.display();
}

void prikaziSveLED(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < LED_COUNT; i++) {
        strip.setLedColor(i, r, g, b);
    }
    strip.show();
}

void kratkiBip() {
    digitalWrite(BUZZER_PIN, HIGH); delay(80); digitalWrite(BUZZER_PIN, LOW);
}

void dugiBip() {
    digitalWrite(BUZZER_PIN, HIGH); delay(300); digitalWrite(BUZZER_PIN, LOW);
}

void pretvoriHexURGB(String hex, uint8_t &r, uint8_t &g, uint8_t &b) {
    if (hex.startsWith("#")) hex = hex.substring(1);
    long broj = strtol(hex.c_str(), NULL, 16);
    r = (broj >> 16) & 0xFF;
    g = (broj >> 8) & 0xFF;
    b = broj & 0xFF;
}

void skenirajSDMape() {
    ukupnoProjekata = 0;
    File root = SD.open("/");
    if (!root) return;

    while (true) {
        File entry = root.openNextFile();
        if (!entry) break; 
        
        if (entry.isDirectory()) {
            String imeMape = entry.name();
            if (imeMape != "System Volume Information" && !imeMape.startsWith(".")) {
                projekti[ukupnoProjekata] = imeMape;
                ukupnoProjekata++;
                if (ukupnoProjekata >= 30) break; // Safety bound limit
            }
        }
        entry.close();
    }
    root.close();
}
void prikaziIzborProjekata() {
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0);
    display.println("- SELECT A PROJECT -");
    display.println("");
    if (ukupnoProjekata == 0) {
        display.println(" No maps on SD card!!");
    } else {
        for (int i = 0; i < ukupnoProjekata; i++) {
            if (i == odabraniProjektIdx) display.print("> ");
            else display.print("  ");
            display.println(projekti[i]);
        }
    }
    display.display();
}
