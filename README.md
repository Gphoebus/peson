# Visualiseur Mesure d'Effort – Arduino HX711
## Lycée de la Borde Basse
![Image description](/python/prog.PNG)
---

## 📦 Contenu du projet

```
mesure_effort/
├── arduino/
│   └── mesure_effort.ino       ← Sketch Arduino (à flasher sur le Nano)
├── python/
│   └── visualiseur_effort.py   ← Application PyQt5
├── requirements.txt
└── README.md
```

---

## 🔌 Matériel requis

| Composant | Connexion |
|-----------|-----------|
| Arduino Nano | USB PC |
| HX711 (jauge de contrainte) | DOUT=9, SCK=8 |
| Écran OLED SSD1306 128×64 I2C | SDA=A4, SCL=A5 |
| Encodeur rotatif | CLK=2, DT=3, SW=4 |

---

## 🖥️ Installation Python

```bash
pip install pyqt5 pyqtgraph pyserial
```

Ou avec le fichier requirements :

```bash
pip install -r requirements.txt
```

---

## 🚀 Utilisation

### 1. Arduino

1. Ouvrir `mesure_effort.ino` dans l'IDE Arduino
2. Installer les bibliothèques : `HX711_ADC`, `Adafruit_SSD1306`, `Adafruit_GFX`
3. Flasher sur l'Arduino Nano
4. Sur l'écran OLED, naviguer jusqu'à **Transmission** dans le menu
5. Appuyer sur le bouton de l'encodeur pour **démarrer/arrêter** la transmission

### 2. Python

```bash
python visualiseur_effort.py
```

Sélectionner le port COM (Windows) ou `/dev/ttyUSB0` (Linux/Mac), choisir le baudrate **9600**, puis cliquer **Connecter**.

---

## 📡 Format de la trame USB

```
secondes;newtons\n
```

**Exemple :**
```
12.345;3.1416
25.000;0.0000
```

- `secondes` : temps écoulé depuis l'activation de la transmission (3 décimales)
- `newtons` : force mesurée (4 décimales), conversion : grammes × 9.80665 / 1000

---

## 💾 Sauvegarde CSV

Le fichier CSV généré utilise le point-virgule comme séparateur (compatible Excel français) :

```
N°;Temps (s);Effort (N)
1;1.000;0.4903
2;2.000;0.4905
...
```

---

## ⚙️ Menu Arduino

| Entrée menu | Action |
|-------------|--------|
| Tarer | Mise à zéro de la balance |
| Mesurer | Affichage en grammes et Newtons |
| Calibrer | Procédure de calibration (EEPROM) |
| **Transmission** | **Démarre/arrête l'envoi USB (1 trame/s)** |

> L'indicateur **[TX]** apparaît sur l'écran OLED tant que la transmission est active.
