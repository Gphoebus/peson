/*
 * Système de mesure d'effort - Version 1.5
 * Corrections : 
 * - Distinction précise entre Simple Clic et Double Clic (évite le retour menu involontaire)
 * - Lecture balance continue et transmission Newtons OK
 * - Encodeur stabilisé par interruption RISING + filtrage temporel
 */

#include <HX711_ADC.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>

// ─── Constantes matérielles ───────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET      -1

#define HX711_DOUT  9
#define HX711_SCK   8

#define ENCODER_CLK 2
#define ENCODER_DT  3
#define ENCODER_SW  4

#define G_TO_N  0.00980665f
#define EEPROM_CALIBRATION_ADDR 0

// ─── Objets ──────────────────────────────────────────────────────────────────
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
HX711_ADC balance(HX711_DOUT, HX711_SCK);

// ─── Variables globales ──────────────────────────────────────────────────────
volatile int encoderPos = 0;
volatile bool encoderChanged = false;
volatile unsigned long lastEncoderTime = 0;
int lastEncoderPos = 0;

unsigned long lastButtonPress = 0;
const unsigned long debounceDelay = 50;
const unsigned long doubleClickDelay = 300;
int buttonClicks = 0;
bool waitingForDouble = false;

enum MenuState { SPLASH, MENU_PRINCIPAL, MESURE, TARE, CALIBRATION, TRANSMISSION };
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
bool calibrationEnCours = false;
const unsigned long stabilizationTime = 2000;

bool transmissionActive = false;
unsigned long transmissionStartMs = 0;
unsigned long dernierEnvoiMs = 0;
unsigned long splashStartTime = 0;

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(9600);

  pinMode(ENCODER_CLK, INPUT_PULLUP);
  pinMode(ENCODER_DT,  INPUT_PULLUP);
  pinMode(ENCODER_SW,  INPUT_PULLUP);
  
  attachInterrupt(digitalPinToInterrupt(ENCODER_CLK), encoderISR, RISING);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    for (;;);
  }
  display.setRotation(2);
  afficherSplashScreen();
  splashStartTime = millis();

  balance.begin();
  balance.start(stabilizationTime);

  float calibrationValue;
  EEPROM.get(EEPROM_CALIBRATION_ADDR, calibrationValue);
  if (calibrationValue > 0 && calibrationValue < 1000000.0f) {
    balance.setCalFactor(calibrationValue);
  } else {
    balance.setCalFactor(-44.04f);
  }
  balance.tareNoDelay();
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
  // 1. Lecture continue balance
  if (balance.update()) {
    sommeMesures += balance.getData();
    nombreMesures++;
  }
  
  unsigned long now = millis();
  if (now - dernierTempsMoyenne >= MOYENNE_PERIODE) {
    if (nombreMesures > 0) moyenneMesure = sommeMesures / nombreMesures;
    sommeMesures = 0; 
    nombreMesures = 0;
    dernierTempsMoyenne = now;
  }

  // 2. Splash Screen
  if (currentState == SPLASH) {
    if (now - splashStartTime >= 3000) {
      currentState = MENU_PRINCIPAL;
      menuSelection = 0;
      afficherMenuPrincipal();
    }
    return;
  }

  // 3. Gestion du bouton (Logique de distinction clic/double-clic)
  gererBoutonChronologie();

  // 4. Encodeur
  if (encoderChanged) {
    encoderChanged = false;
    gererEncodeur();
  }

  // 5. Transmission
  if (transmissionActive) {
    gererTransmissionUSB();
  }

  // 6. Affichage
  switch (currentState) {
    case MESURE:      afficherMesure(); break;
    case TARE:        executerTare(); break;
    case CALIBRATION: if (calibrationEnCours) executerCalibration(); break;
    default: break;
  }
}

// ─── Gestion des entrées ─────────────────────────────────────────────────────
void encoderISR() {
  unsigned long now = millis();
  if (now - lastEncoderTime > 15) { // Debounce temporel plus robuste
    if (digitalRead(ENCODER_DT) == LOW) encoderPos++;
    else encoderPos--;
    encoderChanged = true;
    lastEncoderTime = now;
  }
}

void gererBoutonChronologie() {
  static bool lastButtonState = HIGH;
  bool reading = digitalRead(ENCODER_SW);
  unsigned long now = millis();

  // Détection de l'appui (Front descendant)
  if (reading == LOW && lastButtonState == HIGH) {
    if (now - lastButtonPress > debounceDelay) {
      buttonClicks++;
      lastButtonPress = now;
      waitingForDouble = true;
    }
  }
  lastButtonState = reading;

  // Si on attend de voir s'il y a un 2ème clic et que le délai est passé
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
      if (!calibrationEnCours) {
        calibrationEnCours = true;
        balance.tareNoDelay();
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
  }
}

void traiterDoubleClic() {
  // Le double clic sert uniquement à quitter les modes actifs
  if (currentState == MESURE || currentState == TRANSMISSION) {
    transmissionActive = false; 
    currentState = MENU_PRINCIPAL;
    menuSelection = 0;
    mesureFigee = false;
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
  } 
  else if (currentState == CALIBRATION && !calibrationEnCours) {
    poidsCalibration += (direction * 10.0f);
    poidsCalibration = constrain(poidsCalibration, 10.0f, 5000.0f);
    afficherCalibration();
  }
}

// ─── Transmission ────────────────────────────────────────────────────────────
void gererTransmissionUSB() {
  unsigned long now = millis();
  if (now - dernierEnvoiMs < 1000UL) return; 
  dernierEnvoiMs = now;
  
  float secondes = (now - transmissionStartMs) / 1000.0f;
  float newtons = moyenneMesure * G_TO_N;
  
  Serial.print(secondes, 3);
  Serial.print(';');
  Serial.println(newtons, 4);
}

// ─── Affichages ──────────────────────────────────────────────────────────────
void afficherSplashScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(20, 8);  display.println(F("MESURE"));
  display.setCursor(10, 30); display.println(F("D'EFFORT"));
  display.setTextSize(1);
  display.setCursor(20, 52); display.println(F("Version 1.5"));
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
  if (transmissionActive) { display.setCursor(90, 0); display.print(F("[TX]")); }
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
    display.setCursor(0, 46);  display.println(F("Clic: ON/OFF"));
    display.setCursor(0, 56);  display.println(F("Dbl Clic: Menu"));
  } else {
    display.setCursor(10, 15); display.println(F("Liaison USB"));
    display.setCursor(25, 28); display.println(F("INACTIVE"));
    display.setCursor(0, 46);  display.println(F("Clic: Demarrer"));
    display.setCursor(0, 56);  display.println(F("Dbl Clic: Menu"));
  }
  display.display();
}

void executerMenuSelection() {
  switch (menuSelection) {
    case 0: currentState = TARE; break;
    case 1: currentState = MESURE; mesureFigee = false; break;
    case 2: currentState = CALIBRATION; calibrationEnCours = false; afficherCalibration(); break;
    case 3: currentState = TRANSMISSION; afficherTransmission(); break;
  }
}

void executerTare() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 10); display.println(F("TARE EN COURS..."));
  display.display();
  balance.tareNoDelay();
  delay(2000);
  currentState = MENU_PRINCIPAL;
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
  display.setCursor(0, 56); display.print(F("Dbl clic: menu"));
  display.display();
}

void afficherCalibration() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0); display.println(F("=CALIBRATION="));
  display.setTextSize(2);
  display.setCursor(10, 25); display.print(poidsCalibration, 0); display.println(F(" g"));
  display.setTextSize(1);
  display.setCursor(0, 50); display.println(calibrationEnCours ? F("Patientez...") : F("Regler & Cliquer"));
  display.display();
}

void executerCalibration() {
  static unsigned long calibStartTime = 0;
  static bool taringDone = false;

  if (!taringDone) {
    if (balance.getTareStatus()) {
      taringDone = true;
      calibStartTime = millis();
      afficherCalibration();
    }
    return;
  }

  if (millis() - calibStartTime < 3000) return;

  balance.refreshDataSet();
  float newCal = balance.getNewCalibration(poidsCalibration);
  EEPROM.put(EEPROM_CALIBRATION_ADDR, newCal);

  display.clearDisplay();
  display.setCursor(0, 10); display.println(F("CALIBRATION OK!"));
  display.display();
  delay(2000);

  currentState = MENU_PRINCIPAL;
  calibrationEnCours = false;
  taringDone = false;
  afficherMenuPrincipal();
}