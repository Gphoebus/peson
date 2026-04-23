/*
 * Système de mesure d'effort avec jauge de contrainte
 * Hardware:
 * - Arduino Nano
 * - HX711 avec jauge de contrainte (DOUT=9, SCK=8)
 * - Écran OLED SSD1306 128x64 I2C
 * - Encodeur rotatif (CLK=2, DT=3, SW=4)
 *
 * Trame USB : "<secondes>;<newtons>\n"
 * Exemple   : "12.345;3.14\n"
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

// Conversion grammes → Newtons (g × 9.80665 / 1000)
#define G_TO_N  0.00980665f

// EEPROM
#define EEPROM_CALIBRATION_ADDR 0

// ─── Objets ──────────────────────────────────────────────────────────────────
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
HX711_ADC balance(HX711_DOUT, HX711_SCK);

// ─── Encodeur ────────────────────────────────────────────────────────────────
volatile int encoderPos    = 0;
volatile bool encoderChanged = false;
int lastEncoderPos = 0;
int lastCLK = HIGH;

// ─── Bouton ──────────────────────────────────────────────────────────────────
unsigned long lastButtonPress  = 0;
unsigned long lastClickTime    = 0;
const unsigned long debounceDelay   = 50;
const unsigned long doubleClickDelay = 300;
int clickCount = 0;

// ─── États / menus ───────────────────────────────────────────────────────────
enum MenuState {
  SPLASH,
  MENU_PRINCIPAL,
  MESURE,
  TARE,
  CALIBRATION,
  TRANSMISSION          // ← nouveau
};

MenuState currentState = SPLASH;
int menuSelection = 0;
const int menuItems = 4;  // Tarer, Mesurer, Calibrer, Transmission

// ─── Mesure ──────────────────────────────────────────────────────────────────
bool  mesureFigee     = false;
float mesureFigeeValue = 0.0f;

const int    MOYENNE_PERIODE   = 500;
unsigned long dernierTempsMoyenne = 0;
float sommeMesures  = 0.0f;
int   nombreMesures = 0;
float moyenneMesure = 0.0f;

// ─── Calibration ─────────────────────────────────────────────────────────────
float poidsCalibration = 100.0f;
bool  calibrationEnCours = false;
const unsigned long stabilizationTime = 2000;

// ─── Transmission USB ────────────────────────────────────────────────────────
bool   transmissionActive  = false;
unsigned long transmissionStartMs = 0;   // instant d'activation
unsigned long dernierEnvoiMs      = 0;   // dernier envoi

// ─── Splash ──────────────────────────────────────────────────────────────────
unsigned long splashStartTime = 0;

// ═════════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(9600);

  pinMode(ENCODER_CLK, INPUT_PULLUP);
  pinMode(ENCODER_DT,  INPUT_PULLUP);
  pinMode(ENCODER_SW,  INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENCODER_CLK), encoderISR, CHANGE);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("Echec init SSD1306"));
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

// ═════════════════════════════════════════════════════════════════════════════
void loop() {
  // ── Splash ──
  if (currentState == SPLASH) {
    if (millis() - splashStartTime >= 3000) {
      currentState  = MENU_PRINCIPAL;
      menuSelection = 0;
      afficherMenuPrincipal();
    }
    return;
  }

  if (balance.getTareStatus()) {
    Serial.println(F("Tare terminee"));
  }
  balance.update();

  gererBouton();

  if (encoderChanged) {
    encoderChanged = false;
    gererEncodeur();
  }

  // ── Envoi USB en continu si transmission active ──
  if (transmissionActive) {
    gererTransmissionUSB();
  }

  switch (currentState) {
    case MENU_PRINCIPAL:
      break;
    case MESURE:
      afficherMesure();
      break;
    case TARE:
      executerTare();
      break;
    case CALIBRATION:
      if (calibrationEnCours) executerCalibration();
      break;
    case TRANSMISSION:
      // affichage statique mis à jour par gererBouton / gererEncodeur
      break;
  }
}

// ─── ISR encodeur ────────────────────────────────────────────────────────────
void encoderISR() {
  int CLK = digitalRead(ENCODER_CLK);
  int DT  = digitalRead(ENCODER_DT);
  if (CLK != lastCLK) {
    if (DT != CLK) encoderPos++;
    else           encoderPos--;
    encoderChanged = true;
  }
  lastCLK = CLK;
}

// ─── Bouton ──────────────────────────────────────────────────────────────────
void gererBouton() {
  static bool lastButtonState = HIGH;
  bool buttonState = digitalRead(ENCODER_SW);

  if (buttonState == LOW && lastButtonState == HIGH) {
    unsigned long t = millis();
    if (t - lastButtonPress > debounceDelay) {
      clickCount    = (t - lastClickTime < doubleClickDelay) ? 2 : 1;
      lastClickTime = t;
      lastButtonPress = t;
      delay(10);
      traiterClicBouton();
    }
  }
  lastButtonState = buttonState;
}

void traiterClicBouton() {
  // Double-clic en mode MESURE → retour menu
  if (clickCount == 2 && currentState == MESURE) {
    currentState  = MENU_PRINCIPAL;
    menuSelection = 0;
    mesureFigee   = false;
    afficherMenuPrincipal();
    clickCount = 0;
    return;
  }

  if (clickCount == 1) {
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
        // Clic : bascule ON / OFF
        transmissionActive = !transmissionActive;
        if (transmissionActive) {
          transmissionStartMs = millis();
          dernierEnvoiMs      = millis();
        }
        afficherTransmission();
        break;
    }
  }
  clickCount = 0;
}

// ─── Encodeur ────────────────────────────────────────────────────────────────
void gererEncodeur() {
  if (encoderPos == lastEncoderPos) return;
  int delta = encoderPos - lastEncoderPos;

  switch (currentState) {
    case MENU_PRINCIPAL:
      menuSelection += delta;
      if (menuSelection < 0) menuSelection = menuItems - 1;
      if (menuSelection >= menuItems) menuSelection = 0;
      afficherMenuPrincipal();
      break;

    case CALIBRATION:
      if (!calibrationEnCours) {
        poidsCalibration += delta * 10.0f;
        if (poidsCalibration < 10.0f)   poidsCalibration = 10.0f;
        if (poidsCalibration > 5000.0f) poidsCalibration = 5000.0f;
        afficherCalibration();
      }
      break;

    case TRANSMISSION:
      // L'encodeur ne fait rien de plus ici (une seule option ON/OFF)
      break;
  }
  lastEncoderPos = encoderPos;
}

// ─── Transmission USB ────────────────────────────────────────────────────────
void gererTransmissionUSB() {
  unsigned long now = millis();
  if (now - dernierEnvoiMs < 1000UL) return;   // une fois par seconde
  dernierEnvoiMs = now;

  // Temps écoulé depuis l'activation (secondes)
  float secondes = (now - transmissionStartMs) / 1000.0f;

  // Mesure actuelle en grammes → Newtons
  float grammes = moyenneMesure;
  float newtons = grammes * G_TO_N;

  // Trame : "secondes;newtons\n"
  Serial.print(secondes, 3);
  Serial.print(';');
  Serial.println(newtons, 4);
}

// ─── Affichages ──────────────────────────────────────────────────────────────
void afficherSplashScreen() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(20,  8); display.println(F("MESURE"));
  display.setCursor(10, 30); display.println(F("D'EFFORT"));
  display.setTextSize(1);
  display.setCursor(20, 52); display.println(F("Version 1.1"));
  display.display();
}

void afficherMenuPrincipal() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(20, 0);
  display.println(F("=== MENU ==="));
  display.println();

  const char* items[] = {" Tarer", " Mesurer", " Calibrer", " Transmission"};
  for (int i = 0; i < menuItems; i++) {
    display.print(menuSelection == i ? F(">") : F(" "));
    display.println(F(items[i]));
  }

  // Indicateur transmission active
  if (transmissionActive) {
    display.setCursor(90, 0);
    display.print(F("[TX]"));
  }

  display.display();
}

void afficherTransmission() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.println(F("== TRANSMISSION =="));
  display.println();

  if (transmissionActive) {
    display.setTextSize(2);
    display.setCursor(20, 20);
    display.println(F("ACTIVE"));
    display.setTextSize(1);
    display.setCursor(0, 50);
    display.println(F("Clic : arreter"));
  } else {
    display.setTextSize(1);
    display.setCursor(10, 15);
    display.println(F("Transmission USB"));
    display.setCursor(25, 28);
    display.println(F("INACTIVE"));
    display.println();
    display.println(F("Trame: sec;N"));
    display.setCursor(0, 50);
    display.println(F("Clic : demarrer"));
  }
  display.display();
}

void executerMenuSelection() {
  switch (menuSelection) {
    case 0:  // Tarer
      currentState = TARE;
      break;

    case 1:  // Mesurer
      currentState = MESURE;
      mesureFigee  = false;
      dernierTempsMoyenne = millis();
      sommeMesures = 0.0f;
      nombreMesures = 0;
      moyenneMesure = 0.0f;
      break;

    case 2:  // Calibrer
      currentState       = CALIBRATION;
      calibrationEnCours = false;
      afficherCalibration();
      break;

    case 3:  // Transmission
      currentState = TRANSMISSION;
      afficherTransmission();
      break;
  }
}

void executerTare() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 10); display.println(F("TARE EN COURS..."));
  display.setCursor(0, 30); display.println(F("Patientez..."));
  display.display();

  balance.tareNoDelay();
  delay(2000);

  currentState = MENU_PRINCIPAL;
  afficherMenuPrincipal();
}

void afficherMesure() {
  static unsigned long dernierAffichage = 0;
  unsigned long now = millis();

  if (balance.update()) {
    sommeMesures += balance.getData();
    nombreMesures++;
  }

  if (now - dernierTempsMoyenne >= MOYENNE_PERIODE) {
    if (nombreMesures > 0) moyenneMesure = sommeMesures / nombreMesures;
    sommeMesures  = 0.0f;
    nombreMesures = 0;
    dernierTempsMoyenne = now;
  }

  if (now - dernierAffichage < 100) return;
  dernierAffichage = now;

  float valeur = mesureFigee ? mesureFigeeValue : moyenneMesure;

  display.clearDisplay();
  if (mesureFigee) {
    display.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.setTextColor(SSD1306_WHITE);
  }

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("MESURE"));
  if (mesureFigee) { display.setCursor(70, 0); display.println(F("[FIGE]")); }

  display.setTextSize(2);
  display.setCursor(5, 18);
  display.print(valeur, 1);
  display.println(F(" g"));

  // Affichage Newton en petit
  display.setTextSize(1);
  display.setCursor(5, 40);
  display.print(valeur * G_TO_N, 3);
  display.println(F(" N"));

  // Indicateur TX
  if (transmissionActive) {
    display.setCursor(90, 40);
    display.print(F("[TX]"));
  }

  display.setCursor(0, 56);
  display.print(F("Clic:figer Dbl:menu"));
  display.display();
}

void afficherCalibration() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0); display.println(F("=CALIBRATION="));
  display.println();

  if (!calibrationEnCours) {
    display.println(F("Regler le poids:"));
    display.println();
    display.setTextSize(2);
    display.print(F(" ")); display.print(poidsCalibration, 0); display.println(F(" g"));
    display.setTextSize(1);
    display.println();
    display.println(F("Puis cliquer"));
  } else {
    display.println(F("Placez le poids"));
    display.setTextSize(2);
    display.print(F(" ")); display.print(poidsCalibration, 0); display.println(F(" g"));
    display.setTextSize(1);
    display.println();
    display.println(F("sur la balance..."));
  }
  display.display();
}

void executerCalibration() {
  static unsigned long calibStartTime = 0;
  static bool taringDone = false;

  if (!taringDone) {
    if (balance.getTareStatus()) {
      taringDone     = true;
      calibStartTime = millis();
      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 10); display.println(F("Placez le poids"));
      display.setTextSize(2);
      display.setCursor(10, 30); display.print(poidsCalibration, 0); display.println(F(" g"));
      display.display();
    }
    return;
  }

  if (millis() - calibStartTime < 3000) return;

  balance.refreshDataSet();
  float newCal = balance.getNewCalibration(poidsCalibration);
  EEPROM.put(EEPROM_CALIBRATION_ADDR, newCal);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 10); display.println(F("CALIBRATION OK!"));
  display.setCursor(0, 30); display.print(F("Valeur: ")); display.println(newCal, 2);
  display.display();
  delay(3000);

  currentState       = MENU_PRINCIPAL;
  calibrationEnCours = false;
  taringDone         = false;
  afficherMenuPrincipal();
}
