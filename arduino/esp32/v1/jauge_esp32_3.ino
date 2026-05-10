/*
 * Système de mesure d'effort - Version 3.0 (Fusion BLE + Interface physique)
 * Cible : ESP32-C3 Supermini
 * 
 * Nouveautés v3.0 :
 * - Intégration BLE (inspiré de jauge_esp32_1.ino)
 * - Détection automatique USB / BLE pour la transmission
 * - Commandes série et BLE (TARE, CAL, FACTOR, STATUS, HELP)
 * - Prototype explicite de encoderISR (correction compilation)
 */

// ─── Types déclarés EN PREMIER ────────────────────────────────────────────
enum MenuState  { SPLASH, MENU_PRINCIPAL, MESURE, TARE, CALIBRATION, TRANSMISSION };
enum CalibState { CAL_IDLE, CAL_TARING, CAL_WAITING_WEIGHT, CAL_STABILIZING, CAL_COMPUTING };

// ─────────────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include <HX711_ADC.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>

// ─── GPIO ESP32-C3 Supermini ──────────────────────────────────────────────
#define HX711_DOUT  4
#define HX711_SCK   5
#define I2C_SDA     8
#define I2C_SCL     9
#define ENCODER_CLK 2
#define ENCODER_DT  3
#define ENCODER_SW  10

// ─── Constantes OLED ──────────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define OLED_ADDR      0x3C

// ─── Constantes Physiques ─────────────────────────────────────────────────
#define G_TO_N  0.00980665f

// ─── Gestion EEPROM robuste (adapté ESP32) ────────────────────────────────
#define EEPROM_SIZE         64
#define EEPROM_MAGIC_ADDR   0
#define EEPROM_CAL_ADDR     2
#define EEPROM_CRC_ADDR     6
#define EEPROM_MAGIC_0      0xBE
#define EEPROM_MAGIC_1      0xEF
#define CAL_DEFAULT        -44.04f
#define CAL_MIN             10.0f
#define CAL_MAX          1000000.0f

uint8_t crc8(const uint8_t* data, uint8_t len) {
  uint8_t crc = 0x00;
  while (len--) {
    crc ^= *data++;
    for (uint8_t i = 0; i < 8; i++)
      crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
  }
  return crc;
}

float eepromLireCalibration() {
  uint8_t m0 = EEPROM.read(EEPROM_MAGIC_ADDR);
  uint8_t m1 = EEPROM.read(EEPROM_MAGIC_ADDR + 1);
  if (m0 != EEPROM_MAGIC_0 || m1 != EEPROM_MAGIC_1) {
    return CAL_DEFAULT;
  }
  float val;
  EEPROM.get(EEPROM_CAL_ADDR, val);
  uint8_t buf[4];
  memcpy(buf, &val, 4);
  uint8_t storedCRC = EEPROM.read(EEPROM_CRC_ADDR);
  if (crc8(buf, 4) != storedCRC) {
    return CAL_DEFAULT;
  }
  if (fabsf(val) < CAL_MIN || fabsf(val) > CAL_MAX) {
    return CAL_DEFAULT;
  }
  return val;
}

void eepromSauverCalibration(float val) {
  if (fabsf(val) < CAL_MIN || fabsf(val) > CAL_MAX) return;
  EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_0);
  EEPROM.write(EEPROM_MAGIC_ADDR + 1, EEPROM_MAGIC_1);
  uint8_t buf[4];
  memcpy(buf, &val, 4);
  for (uint8_t i = 0; i < 4; i++) {
    EEPROM.write(EEPROM_CAL_ADDR + i, buf[i]);
  }
  uint8_t crc = crc8(buf, 4);
  EEPROM.write(EEPROM_CRC_ADDR, crc);
  EEPROM.commit();
}

// ─── UUIDs BLE ──────────────────────────────────────────────────────────
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// ─── Objets ───────────────────────────────────────────────────────────────
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
HX711_ADC balance(HX711_DOUT, HX711_SCK);

BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// ─── Variables globales ───────────────────────────────────────────────────
volatile int encoderPos = 0;
volatile bool encoderChanged = false;
volatile unsigned long lastEncoderTime = 0;
int lastEncoderPos = 0;

unsigned long lastButtonPress = 0;
const unsigned long debounceDelay = 50;
const unsigned long doubleClickDelay = 300;
int buttonClicks = 0;
bool waitingForDouble = false;

MenuState currentState = SPLASH;
int menuSelection = 0;
const int menuItems = 4;

bool mesureFigee = false;
float mesureFigeeValue = 0.0f;
const int MOYENNE_PERIODE = 500;
unsigned long dernierTempsMoyenne = 0;
float sommeMesures = 0.0f;
int nombreMesures = 0;
float moyenneMesure = 0.0f;

float poidsCalibration = 100.0f;
CalibState calibState = CAL_IDLE;
unsigned long calibTimerStart = 0;
const unsigned long CALIB_STABILIZE_MS = 3000UL;

bool transmissionActive = false;
unsigned long transmissionStartMs = 0;
unsigned long dernierEnvoiMs = 0;
unsigned long splashStartTime = 0;

const unsigned long INPUT_GUARD_MS = 400UL;
unsigned long guardUntil = 0;

float facteur_calibration = CAL_DEFAULT;

// ─── Déclarations anticipées ──────────────────────────────────────────────
void changerEtat(MenuState nouvelEtat);
void afficherCalibration();
void afficherMenuPrincipal();
void traiterCommande(String commande, bool depuisBle = false);
void envoyerReponse(String reponse);
void IRAM_ATTR encoderISR();   // Prototype indispensable

// ─── Callbacks BLE ──────────────────────────────────────────────────────
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      Serial.println("BLE: Connecte");
    }
    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      Serial.println("BLE: Deconnecte");
    }
};

class MyCharacteristicCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      String rxValue = pCharacteristic->getValue();
      if (rxValue.length() > 0) {
        rxValue.trim();
        Serial.printf("BLE recu: %s\n", rxValue.c_str());
        traiterCommande(rxValue, true);
      }
    }
};

// ─── Fonction centralisée de changement d'état ────────────────────────────
void changerEtat(MenuState nouvelEtat) {
  currentState     = nouvelEtat;
  guardUntil       = millis() + INPUT_GUARD_MS;
  buttonClicks     = 0;
  waitingForDouble = false;
  lastEncoderPos   = encoderPos;
  encoderChanged   = false;
}

// ─── setup() ──────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== BALANCE HX711 - ESP32-C3 (BLE+USB) ===");

  // EEPROM
  if (!EEPROM.begin(EEPROM_SIZE)) {
    Serial.println("ERREUR: EEPROM init");
  }
  facteur_calibration = eepromLireCalibration();
  Serial.printf("Facteur calibration EEPROM: %.2f\n", facteur_calibration);

  // Wire (I2C)
  Wire.begin(I2C_SDA, I2C_SCL);
  delay(100);

  // OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED non detectee. Continue sans...");
  }
  display.setRotation(2);
  afficherSplashScreen();
  splashStartTime = millis();

  // Boutons encodeur
  pinMode(ENCODER_CLK, INPUT_PULLUP);
  pinMode(ENCODER_DT,  INPUT_PULLUP);
  pinMode(ENCODER_SW,  INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENCODER_CLK), encoderISR, RISING);

  // Initialisation BLE
  BLEDevice::init("ESP32_Balance");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ   |
                      BLECharacteristic::PROPERTY_WRITE  |
                      BLECharacteristic::PROPERTY_NOTIFY
                    );
  pCharacteristic->setCallbacks(new MyCharacteristicCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());
  pService->start();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->start();
  Serial.println("BLE: Pret (ESP32_Balance)");

  // HX711
  balance.begin();
  Serial.println("Attente stabilisation HX711...");
  unsigned long debut = millis();
  while (!balance.update()) {
    if (millis() - debut > 5000) {
      Serial.println("TIMEOUT HX711!");
      break;
    }
  }
  balance.setCalFactor(facteur_calibration);
  balance.tareNoDelay();
  
  Serial.println("Systeme pret.");
  Serial.println("Cmd serie/BLE: TARE, CAL [g], FACTOR [val], STATUS, HELP");
}

// ─── loop() ───────────────────────────────────────────────────────────────
void loop() {
  // 1. Mise à jour continue du capteur HX711
  if (balance.update()) {
    sommeMesures += balance.getData();
    nombreMesures++;
  }

  unsigned long now = millis();
  // Calcul de la moyenne glissante
  if (now - dernierTempsMoyenne >= MOYENNE_PERIODE) {
    if (nombreMesures > 0) moyenneMesure = sommeMesures / nombreMesures;
    sommeMesures = 0;
    nombreMesures = 0;
    dernierTempsMoyenne = now;
  }

  // 2. Splash Screen (bloque au début)
  if (currentState == SPLASH) {
    if (now - splashStartTime >= 3000) {
      menuSelection = 0;
      changerEtat(MENU_PRINCIPAL);
      afficherMenuPrincipal();
    }
    // Traiter les commandes série même pendant le splash
    if (Serial.available()) {
      String cmd = Serial.readStringUntil('\n');
      traiterCommande(cmd);
    }
    return;
  }

  // 3. Gestion de l'interface physique (encodeur, bouton)
  if (now >= guardUntil) {
    gererBoutonChronologie();
    if (encoderChanged) {
      encoderChanged = false;
      gererEncodeur();
    }
  } else {
    buttonClicks     = 0;
    waitingForDouble = false;
    encoderChanged   = false;
  }

  // 4. Transmission active (série ou BLE selon connexion USB)
  if (transmissionActive) {
    // Déterminer si la liaison série est ouverte (USB connecté)
    bool usbConnecte = (bool)Serial;   // true si moniteur série ouvert

    if (usbConnecte) {
      // Envoi sur le port série
      gererTransmissionUSB();
    } else {
      // Envoi via BLE (notification)
      if (deviceConnected) {
        gererTransmissionBLE();
      }
    }
  }

  // 5. Mise à jour affichage / logique états
  switch (currentState) {
    case MESURE:      afficherMesure();      break;
    case TARE:        executerTare();         break;
    case CALIBRATION: executerCalibration();  break;
    default: break;
  }

  // 6. Traitement des commandes série
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    traiterCommande(cmd);
  }
  
  // Tâche asynchrone de tare en arrière-plan
  if (balance.getTareStatus()) {
    balance.refreshDataSet();
  }

  // 7. Gestion de la déconnexion BLE
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = true;
  }
  if (!deviceConnected && oldDeviceConnected) {
    oldDeviceConnected = false;
    delay(300);
    BLEDevice::startAdvertising();
  }

  delay(5);
}

// ─── ISR Encodeur ──────────────────────────────────────────────────────────
void IRAM_ATTR encoderISR() {
  unsigned long now = millis();
  if (now - lastEncoderTime > 15) {
    if (digitalRead(ENCODER_DT) == LOW) encoderPos++;
    else encoderPos--;
    encoderChanged = true;
    lastEncoderTime = now;
  }
}

// ─── Gestion Bouton ────────────────────────────────────────────────────────
void gererBoutonChronologie() {
  static bool lastButtonState = HIGH;
  bool reading = digitalRead(ENCODER_SW);
  unsigned long now = millis();

  if (reading == LOW && lastButtonState == HIGH) {
    if (now - lastButtonPress > debounceDelay) {
      buttonClicks++;
      lastButtonPress = now;
      waitingForDouble = true;
    }
  }
  lastButtonState = reading;

  if (waitingForDouble && (now - lastButtonPress > doubleClickDelay)) {
    if (buttonClicks == 1) traiterSimpleClic();
    else if (buttonClicks >= 2) traiterDoubleClic();
    buttonClicks = 0;
    waitingForDouble = false;
  }
}

void traiterSimpleClic() {
  switch (currentState) {
    case MENU_PRINCIPAL:
      executerMenuSelection();
      break;

    case MESURE:
      mesureFigee = !mesureFigee;
      if (mesureFigee) mesureFigeeValue = moyenneMesure;
      break;

    case CALIBRATION:
      if (calibState == CAL_IDLE) {
        calibState = CAL_TARING;
        balance.tareNoDelay();
        afficherCalibration();
      } else if (calibState == CAL_WAITING_WEIGHT) {
        calibState = CAL_STABILIZING;
        calibTimerStart = millis();
        afficherCalibration();
      }
      break;

    case TRANSMISSION:
      transmissionActive = !transmissionActive;
      if (transmissionActive) {
        transmissionStartMs = millis();
        dernierEnvoiMs = millis();
      }
      afficherTransmission();
      break;

    default: break;
  }
}

void traiterDoubleClic() {
  if (currentState == MESURE || currentState == TRANSMISSION || currentState == CALIBRATION) {
    transmissionActive = false;
    calibState         = CAL_IDLE;
    mesureFigee        = false;
    menuSelection      = 0;
    changerEtat(MENU_PRINCIPAL);
    afficherMenuPrincipal();
  }
}

void gererEncodeur() {
  int delta = encoderPos - lastEncoderPos;
  lastEncoderPos = encoderPos;
  if (delta == 0) return;

  int direction = (delta > 0) ? 1 : -1;

  if (currentState == MENU_PRINCIPAL) {
    menuSelection += direction;
    if (menuSelection < 0) menuSelection = menuItems - 1;
    if (menuSelection >= menuItems) menuSelection = 0;
    afficherMenuPrincipal();
  } else if (currentState == CALIBRATION && calibState == CAL_IDLE) {
    poidsCalibration += (direction * 10.0f);
    poidsCalibration = constrain(poidsCalibration, 10.0f, 5000.0f);
    afficherCalibration();
  }
}

// ─── Transmission série continue (appelée uniquement si USB connecté) ─────
void gererTransmissionUSB() {
  unsigned long now = millis();
  if (now - dernierEnvoiMs < 200UL) return;
  dernierEnvoiMs = now;

  float secondes = (now - transmissionStartMs) / 1000.0f;
  float newtons  = moyenneMesure * G_TO_N;

  Serial.print(secondes, 3);
  Serial.print(';');
  Serial.println(newtons, 4);
}

// ─── Transmission BLE (notification) ──────────────────────────────────────
void gererTransmissionBLE() {
  unsigned long now = millis();
  if (now - dernierEnvoiMs < 200UL) return;
  dernierEnvoiMs = now;

  float secondes = (now - transmissionStartMs) / 1000.0f;
  float newtons  = moyenneMesure * G_TO_N;

  char buffer[40];
  snprintf(buffer, sizeof(buffer), "%.3f;%.4f", secondes, newtons);
  if (pCharacteristic) {
    pCharacteristic->setValue(buffer);
    pCharacteristic->notify();
  }
}

// ─── Affichages OLED ───────────────────────────────────────────────────────
void afficherSplashScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(20, 8);  display.println(F("MESURE"));
  display.setCursor(10, 30); display.println(F("D'EFFORT"));
  display.setTextSize(1);
  display.setCursor(20, 52); display.println(F("Version 3.0"));
  display.display();
}

void afficherMenuPrincipal() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(20, 0);
  display.println(F("=== MENU ==="));
  display.println();

  const char* items[] = {" Tarer", " Mesurer", " Calibrer", " Transmission"};
  for (int i = 0; i < menuItems; i++) {
    display.print(menuSelection == i ? F(">") : F(" "));
    display.println(items[i]);
  }
  if (transmissionActive) {
    display.setCursor(90, 0); display.print(F("[TX]"));
  }
  if (deviceConnected) {
    display.setCursor(90, 56); display.print(F("BLE"));
  }
  display.display();
}

void afficherTransmission() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("== TRANSMISSION =="));
  if (transmissionActive) {
    display.setTextSize(2);
    display.setCursor(20, 20); display.println(F("ACTIVE"));
    display.setTextSize(1);
    // Afficher le mode (USB ou BLE)
    display.setCursor(0, 38);
    if ((bool)Serial) {
      display.print(F("USB"));
    } else {
      display.print(F("BLE"));
    }
    display.setCursor(0, 46);  display.println(F("Clic: Arreter"));
    display.setCursor(0, 56);  display.println(F("Dbl Clic: Menu"));
  } else {
    display.setCursor(10, 15); display.println(F("Liaison"));
    display.setCursor(25, 28); display.println(F("INACTIVE"));
    display.setCursor(0, 46);  display.println(F("Clic: Demarrer"));
    display.setCursor(0, 56);  display.println(F("Dbl Clic: Menu"));
  }
  display.display();
}

void executerMenuSelection() {
  switch (menuSelection) {
    case 0:
      changerEtat(TARE);
      break;
    case 1:
      mesureFigee = false;
      changerEtat(MESURE);
      break;
    case 2:
      calibState = CAL_IDLE;
      changerEtat(CALIBRATION);
      afficherCalibration();
      break;
    case 3:
      // Activation immédiate de la transmission
      transmissionActive = true;
      transmissionStartMs = millis();
      dernierEnvoiMs = millis();
      changerEtat(TRANSMISSION);
      afficherTransmission();
      break;
  }
}

void executerTare() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 10); display.println(F("TARE EN COURS..."));
  display.display();
  balance.tareNoDelay();
  delay(2000);
  menuSelection = 0;
  changerEtat(MENU_PRINCIPAL);
  afficherMenuPrincipal();
}

void afficherMesure() {
  static unsigned long dernierAffichage = 0;
  if (millis() - dernierAffichage < 100) return;
  dernierAffichage = millis();

  float valeur = mesureFigee ? mesureFigeeValue : moyenneMesure;
  display.clearDisplay();
  if (mesureFigee) {
    display.fillRect(0, 0, 128, 64, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.setTextColor(SSD1306_WHITE);
  }

  display.setCursor(0, 0); display.print(F("MESURE"));
  if (mesureFigee) { display.setCursor(70, 0); display.print(F("[FIGE]")); }
  display.setTextSize(2);
  display.setCursor(5, 18); display.print(valeur, 1); display.print(F(" g"));
  display.setTextSize(1);
  display.setCursor(5, 40); display.print(valeur * G_TO_N, 3); display.print(F(" N"));
  if (transmissionActive) { display.setCursor(90, 40); display.print(F("[TX]")); }
  if (deviceConnected)     { display.setCursor(90, 56); display.print(F("BLE")); }
  display.setCursor(0, 56); display.print(F("Dbl clic: menu"));
  display.display();
}

void afficherCalibration() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0); display.println(F("=CALIBRATION="));

  switch (calibState) {
    case CAL_IDLE:
      display.setTextSize(2);
      display.setCursor(10, 20); display.print(poidsCalibration, 0); display.println(F(" g"));
      display.setTextSize(1);
      display.setCursor(0, 48); display.println(F("Regler & Cliquer"));
      break;

    case CAL_TARING:
      display.setCursor(0, 20); display.println(F("Tare en cours..."));
      break;

    case CAL_WAITING_WEIGHT:
      display.setCursor(0, 20); display.println(F("Posez le poids"));
      display.setCursor(0, 32); display.print(poidsCalibration, 0); display.println(F(" g"));
      display.setCursor(0, 48); display.println(F("puis Cliquer"));
      break;

    case CAL_STABILIZING:
      display.setCursor(0, 20); display.println(F("Stabilisation..."));
      display.setCursor(0, 32); display.println(F("Ne pas toucher"));
      break;

    case CAL_COMPUTING:
      display.setCursor(0, 20); display.println(F("Calcul en cours"));
      break;
  }
  display.display();
}

void executerCalibration() {
  unsigned long now = millis();

  switch (calibState) {
    case CAL_IDLE:
      break;

    case CAL_TARING:
      if (balance.getTareStatus()) {
        calibState = CAL_WAITING_WEIGHT;
        afficherCalibration();
      }
      break;

    case CAL_WAITING_WEIGHT:
      break;

    case CAL_STABILIZING:
      if (now - calibTimerStart >= CALIB_STABILIZE_MS) {
        calibState = CAL_COMPUTING;
        afficherCalibration();
      }
      break;

    case CAL_COMPUTING: {
      balance.refreshDataSet();
      float newCal = balance.getNewCalibration(poidsCalibration);
      facteur_calibration = newCal;

      if (fabsf(newCal) >= CAL_MIN && fabsf(newCal) <= CAL_MAX) {
        eepromSauverCalibration(newCal);
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 10); display.println(F("CALIBRATION OK!"));
        display.setCursor(0, 28); display.print(F("Cal: ")); display.println(newCal, 2);
        display.display();
        if (!transmissionActive) Serial.printf("CMD: CAL_OK %.2f\n", newCal);
      } else {
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 10); display.println(F("ERREUR CALIB!"));
        display.setCursor(0, 28); display.println(F("Verifier le poids"));
        display.display();
        if (!transmissionActive) Serial.println("CMD: CAL_ERR");
      }

      delay(2500);
      calibState    = CAL_IDLE;
      menuSelection = 0;
      changerEtat(MENU_PRINCIPAL);
      afficherMenuPrincipal();
      break;
    }
  }
}

// ========== TRAITEMENT COMMANDES (Série + BLE) ==========
void traiterCommande(String commande, bool depuisBle) {
  commande.trim();
  if (commande.length() == 0) return;
  if (commande.startsWith(";")) return;  // ignorer commentaires

  // Pendant la transmission série, on minimise les réponses série
  bool silencieux = transmissionActive && !depuisBle;

  String reponse = "";

  if (commande == "TARE" || commande == "tare") {
    balance.tareNoDelay();
    unsigned long startWait = millis();
    while (!balance.getTareStatus() && (millis() - startWait < 5000)) {
      delay(10);
    }
    balance.refreshDataSet();
    reponse = "Tare terminee";
  }
  else if (commande.startsWith("CAL ") || commande.startsWith("cal ")) {
    float poids_connu = commande.substring(4).toFloat();
    if (poids_connu > 0) {
      if (currentState == CALIBRATION) {
        calibState = CAL_IDLE;
        changerEtat(MENU_PRINCIPAL);
        afficherMenuPrincipal();
      }
      balance.refreshDataSet();
      float newCal = balance.getNewCalibration(poids_connu);
      if (fabsf(newCal) >= CAL_MIN && fabsf(newCal) <= CAL_MAX) {
        facteur_calibration = newCal;
        balance.setCalFactor(facteur_calibration);
        eepromSauverCalibration(facteur_calibration);
        reponse = "Cal OK: " + String(facteur_calibration, 2);
      } else {
        reponse = "Erreur calibration. Verifiez le poids.";
      }
    } else {
      reponse = "Usage: CAL [poids_en_grammes]";
    }
  }
  else if (commande.startsWith("FACTOR ") || commande.startsWith("factor ")) {
    float val = commande.substring(7).toFloat();
    if (val > 0) {
      facteur_calibration = val;
      balance.setCalFactor(facteur_calibration);
      eepromSauverCalibration(facteur_calibration);
      reponse = "Facteur defini: " + String(facteur_calibration, 2);
    } else {
      reponse = "Usage: FACTOR [valeur]";
    }
  }
  else if (commande == "STATUS" || commande == "status") {
    char msg[80];
    snprintf(msg, sizeof(msg), "Etat:%d | Factor:%.2f | Poids:%.2fg | TX:%s BLE:%s",
             currentState, facteur_calibration, moyenneMesure,
             transmissionActive ? "ON" : "OFF",
             deviceConnected ? "ON" : "OFF");
    reponse = String(msg);
  }
  else if (commande == "HELP" || commande == "help") {
    reponse = "CMD: TARE, CAL [g], FACTOR [val], STATUS, HELP";
  }
  else {
    reponse = "CMD inconnue. Tapez HELP.";
  }

  // Envoyer la réponse sur le média approprié
  if (depuisBle) {
    if (deviceConnected && pCharacteristic && reponse.length() > 0) {
      pCharacteristic->setValue(reponse.c_str());
      pCharacteristic->notify();
      delay(10);
    }
  } else {
    if (!silencieux) Serial.println(reponse);
  }
}

// Placeholder pour envoi de réponse
void envoyerReponse(String reponse) {
  // Actuellement utilisé dans traiterCommande
}