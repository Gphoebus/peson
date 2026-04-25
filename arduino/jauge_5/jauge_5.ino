/*
 * Système de mesure d'effort - Version 2.1
 * Corrections v2.0 :
 * - Gestion EEPROM robuste : magic number + CRC8 pour valider les données stockées
 * - Calibration HX711 : séquence correcte tare → pose poids → attente stabilisation → calcul
 * - Flag taringDone réinitialisé correctement à chaque entrée en calibration
 * - Sauvegarde EEPROM uniquement si la valeur calculée est plausible
 * - Protection contre les écritures EEPROM répétées (EEPROM.update vs EEPROM.put)
 * - Validation complète au démarrage avec fallback sur valeur par défaut
 * Corrections v2.1 :
 * - Fenêtre de garde (INPUT_GUARD_MS) après tout changement d'état : ignore
 *   bouton et encodeur pendant 400 ms pour éviter sélections/retours parasites
 * - Fonction centralisée changerEtat() : applique la garde, vide le pipeline
 *   bouton (buttonClicks/waitingForDouble), resynchronise lastEncoderPos
 * - Suppression des chemins de code qui appelaient directement currentState=
 *   hors de changerEtat()
 * - FIX compilation Arduino IDE : enum MenuState et CalibState déclarés AVANT
 *   les #include — l'IDE injecte les prototypes automatiques en tête du fichier
 *   compilé ; si MenuState n'est pas encore défini à ce point, le prototype de
 *   changerEtat(MenuState) génère "variable or field declared void".
 */

// ─── Types déclarés EN PREMIER (avant les #include) ──────────────────────────
// L'IDE Arduino génère automatiquement les prototypes de toutes les fonctions
// et les insère juste AVANT le premier #include. Si un paramètre utilise un
// type défini APRÈS ce point, la compilation échoue. On déclare donc les enum
// ici, en tout premier, pour qu'ils soient visibles lors de la génération du
// prototype de changerEtat(MenuState).

enum MenuState  { SPLASH, MENU_PRINCIPAL, MESURE, TARE, CALIBRATION, TRANSMISSION };
enum CalibState { CAL_IDLE, CAL_TARING, CAL_WAITING_WEIGHT, CAL_STABILIZING, CAL_COMPUTING };

// ─────────────────────────────────────────────────────────────────────────────
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

// ─── Gestion EEPROM robuste ──────────────────────────────────────────────────
//  Structure stockée en EEPROM :
//    [0..1]  magic number  (0xBE, 0xEF)
//    [2..5]  float calibrationValue
//    [6]     CRC8 sur les 4 octets du float
//
#define EEPROM_MAGIC_ADDR   0
#define EEPROM_CAL_ADDR     2
#define EEPROM_CRC_ADDR     6
#define EEPROM_MAGIC_0      0xBE
#define EEPROM_MAGIC_1      0xEF
#define CAL_DEFAULT        -44.04f   // valeur constructeur / première mise en service
#define CAL_MIN             10.0f    // facteur de calibration minimal plausible (valeur absolue)
#define CAL_MAX          1000000.0f  // facteur de calibration maximal plausible

// Calcul CRC8 simple (polynôme 0x07)
uint8_t crc8(const uint8_t* data, uint8_t len) {
  uint8_t crc = 0x00;
  while (len--) {
    crc ^= *data++;
    for (uint8_t i = 0; i < 8; i++)
      crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
  }
  return crc;
}

// Lecture calibration depuis EEPROM avec validation complète
float eepromLireCalibration() {
  uint8_t m0 = EEPROM.read(EEPROM_MAGIC_ADDR);
  uint8_t m1 = EEPROM.read(EEPROM_MAGIC_ADDR + 1);
  if (m0 != EEPROM_MAGIC_0 || m1 != EEPROM_MAGIC_1) {
    return CAL_DEFAULT; // Premier démarrage : magic absent
  }

  float val;
  EEPROM.get(EEPROM_CAL_ADDR, val);

  uint8_t buf[4];
  memcpy(buf, &val, 4);
  uint8_t storedCRC = EEPROM.read(EEPROM_CRC_ADDR);
  if (crc8(buf, 4) != storedCRC) {
    return CAL_DEFAULT; // Données corrompues
  }

  // Vérification plausibilité (valeur absolue)
  if (fabsf(val) < CAL_MIN || fabsf(val) > CAL_MAX) {
    return CAL_DEFAULT;
  }

  return val;
}

// Écriture calibration en EEPROM (n'écrit que si la valeur a changé → préserve les cycles)
void eepromSauverCalibration(float val) {
  // Vérification plausibilité avant écriture
  if (fabsf(val) < CAL_MIN || fabsf(val) > CAL_MAX) return;

  EEPROM.update(EEPROM_MAGIC_ADDR,     EEPROM_MAGIC_0);
  EEPROM.update(EEPROM_MAGIC_ADDR + 1, EEPROM_MAGIC_1);

  // Écriture octet par octet avec update (évite usure inutile)
  uint8_t buf[4];
  memcpy(buf, &val, 4);
  for (uint8_t i = 0; i < 4; i++) {
    EEPROM.update(EEPROM_CAL_ADDR + i, buf[i]);
  }

  uint8_t crc = crc8(buf, 4);
  EEPROM.update(EEPROM_CRC_ADDR, crc);
}

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

// ─── Machine d'états calibration ─────────────────────────────────────────────
//  IDLE           → l'utilisateur règle le poids avec l'encodeur
//  TARING         → la tare est en cours (balance.tareNoDelay lancé)
//  WAITING_WEIGHT → affichage "Posez le poids", attente clic utilisateur
//  STABILIZING    → on attend la stabilisation (3 s)
//  COMPUTING      → calcul + sauvegarde EEPROM
CalibState calibState = CAL_IDLE;
unsigned long calibTimerStart = 0;
const unsigned long CALIB_STABILIZE_MS = 3000UL;

bool transmissionActive = false;
unsigned long transmissionStartMs = 0;
unsigned long dernierEnvoiMs = 0;
unsigned long splashStartTime = 0;

// ─── Fenêtre de garde anti-rebond inter-états ─────────────────────────────────
//  Après tout changement d'état, les entrées bouton ET encodeur sont ignorées
//  pendant INPUT_GUARD_MS ms. Cela évite qu'un relâchement de bouton ou une
//  impulsion résiduelle de l'encodeur soit interprété dans le nouvel état.
const unsigned long INPUT_GUARD_MS = 400UL;
unsigned long guardUntil = 0;

// Fonction centralisée de changement d'état :
// vide le pipeline bouton, resynchronise l'encodeur, arme la garde.
void changerEtat(MenuState nouvelEtat) {
  currentState     = nouvelEtat;
  guardUntil       = millis() + INPUT_GUARD_MS;
  buttonClicks     = 0;
  waitingForDouble = false;
  lastEncoderPos   = encoderPos;
  encoderChanged   = false;
}

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
  balance.start(2000); // stabilisation initiale 2 s

  float calVal = eepromLireCalibration();
  balance.setCalFactor(calVal);
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
      menuSelection = 0;
      changerEtat(MENU_PRINCIPAL);
      afficherMenuPrincipal();
    }
    return;
  }

  // 3. Bouton + Encodeur (bloqués pendant la fenêtre de garde inter-états)
  if (now >= guardUntil) {
    gererBoutonChronologie();

    if (encoderChanged) {
      encoderChanged = false;
      gererEncodeur();
    }
  } else {
    // Vider silencieusement le pipeline bouton pendant la garde
    buttonClicks     = 0;
    waitingForDouble = false;
    encoderChanged   = false;
  }

  // 4. Transmission
  if (transmissionActive) {
    gererTransmissionUSB();
  }

  // 5. Affichage / logique états
  switch (currentState) {
    case MESURE:      afficherMesure();      break;
    case TARE:        executerTare();         break;
    case CALIBRATION: executerCalibration();  break;
    default: break;
  }
}

// ─── ISR Encodeur ─────────────────────────────────────────────────────────────
void encoderISR() {
  unsigned long now = millis();
  if (now - lastEncoderTime > 15) {
    if (digitalRead(ENCODER_DT) == LOW) encoderPos++;
    else encoderPos--;
    encoderChanged = true;
    lastEncoderTime = now;
  }
}

// ─── Gestion bouton ──────────────────────────────────────────────────────────
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

// ─── Transmission ─────────────────────────────────────────────────────────────
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

// ─── Affichages ──────────────────────────────────────────────────────────────
void afficherSplashScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(20, 8);  display.println(F("MESURE"));
  display.setCursor(10, 30); display.println(F("D'EFFORT"));
  display.setTextSize(1);
  display.setCursor(20, 52); display.println(F("Version 2.0"));
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
  display.setCursor(0, 56); display.print(F("Dbl clic: menu"));
  display.display();
}

// ─── Calibration : affichage contextuel selon l'état ─────────────────────────
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

// ─── Calibration : machine d'états non bloquante ─────────────────────────────
//
//  Séquence correcte HX711_ADC :
//   1. balance.tareNoDelay()          ← zéro sans poids
//   2. Attendre balance.getTareStatus() == true
//   3. Afficher "posez le poids" et attendre confirmation utilisateur
//   4. Attendre CALIB_STABILIZE_MS pour que la lecture se stabilise
//   5. balance.refreshDataSet()       ← capture un jeu de données propre
//   6. balance.getNewCalibration(poids) ← calcule et applique le facteur
//   7. Sauvegarder en EEPROM
//
void executerCalibration() {
  unsigned long now = millis();

  switch (calibState) {

    case CAL_IDLE:
      // Rien : on attend le clic utilisateur (géré dans traiterSimpleClic)
      break;

    case CAL_TARING:
      // Attente fin de tare non-bloquante
      if (balance.getTareStatus()) {
        calibState = CAL_WAITING_WEIGHT;
        afficherCalibration();
      }
      break;

    case CAL_WAITING_WEIGHT:
      // Rien : on attend le clic utilisateur (géré dans traiterSimpleClic)
      break;

    case CAL_STABILIZING:
      // Attente non-bloquante de CALIB_STABILIZE_MS
      if (now - calibTimerStart >= CALIB_STABILIZE_MS) {
        calibState = CAL_COMPUTING;
        afficherCalibration();
      }
      break;

    case CAL_COMPUTING: {
      balance.refreshDataSet();
      float newCal = balance.getNewCalibration(poidsCalibration);

      if (fabsf(newCal) >= CAL_MIN && fabsf(newCal) <= CAL_MAX) {
        eepromSauverCalibration(newCal);
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 10); display.println(F("CALIBRATION OK!"));
        display.setCursor(0, 28); display.print(F("Cal: ")); display.println(newCal, 2);
        display.display();
      } else {
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 10); display.println(F("ERREUR CALIB!"));
        display.setCursor(0, 28); display.println(F("Verifier le poids"));
        display.display();
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
