#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Visualiseur de mesure d'effort – Arduino HX711
Réception trame USB : "secondes;newtons\n"
Auteur : M. Parent – Lycée de la Borde Basse
"""

import sys
import csv
import datetime
import serial
import serial.tools.list_ports

from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QSplitter, QTableWidget, QTableWidgetItem, QHeaderView,
    QPushButton, QLabel, QComboBox, QSpinBox, QFileDialog,
    QGroupBox, QMessageBox, QStatusBar, QFrame, QDoubleSpinBox
)
from PyQt5.QtCore import Qt, QTimer, QThread, pyqtSignal
from PyQt5.QtGui import QFont, QColor, QPalette
from pyqtgraph import GraphicsLayoutWidget

import pyqtgraph as pg


try:
    pg.setConfigOptions(
        background='#1e1e2e',
        foreground='#cdd6f4'
    )
except AttributeError:
    pass


# ─────────────────────────────────────────────────────────────────────────────
# Thread de lecture série
# ─────────────────────────────────────────────────────────────────────────────
class SerialReaderThread(QThread):
    """Lit le port série en arrière-plan et émet les trames valides."""
    nouvelle_trame = pyqtSignal(float, float)   # (secondes, newtons)
    erreur         = pyqtSignal(str)

    def __init__(self, port: str, baudrate: int):
        super().__init__()
        self.port     = port
        self.baudrate = baudrate
        self._running = True
        self._ser     = None

    def run(self):
        try:
            self._ser = serial.Serial(self.port, self.baudrate, timeout=2)
        except serial.SerialException as e:
            self.erreur.emit(f"Impossible d'ouvrir {self.port} : {e}")
            return

        while self._running:
            try:
                line = self._ser.readline().decode('utf-8', errors='ignore').strip()
                if not line or ';' not in line:
                    continue
                parts = line.split(';')
                if len(parts) != 2:
                    continue
                t  = float(parts[0])
                fn = float(parts[1])
                self.nouvelle_trame.emit(t, fn)
            except (ValueError, serial.SerialException):
                continue

    def stop(self):
        self._running = False
        if self._ser and self._ser.is_open:
            self._ser.close()
        self.wait()


# ─────────────────────────────────────────────────────────────────────────────
# Fenêtre principale
# ─────────────────────────────────────────────────────────────────────────────
class MainWindow(QMainWindow):
    COULEUR_ACCENT  = '#89b4fa'   # bleu Catppuccin
    COULEUR_OK      = '#a6e3a1'   # vert
    COULEUR_ERREUR  = '#f38ba8'   # rouge
    COULEUR_WARN    = '#fab387'   # orange
    COULEUR_BG      = '#1e1e2e'
    COULEUR_SURFACE = '#313244'
    COULEUR_TEXT    = '#cdd6f4'

    def __init__(self):
        super().__init__()
        self.setWindowTitle("Visualiseur Mesure d'Effort – Arduino HX711")
        self.resize(1200, 720)
        self.setMinimumSize(900, 600)

        # Données
        self.temps_data:  list[float] = []
        self.effort_data: list[float] = []
        self._thread: SerialReaderThread | None = None
        self._nb_total = 0

        self._appliquer_theme()
        self._build_ui()
        self._rafraichir_ports()

    # ── Thème ────────────────────────────────────────────────────────────────
    def _appliquer_theme(self):
        self.setStyleSheet(f"""
            QMainWindow, QWidget {{
                background-color: {self.COULEUR_BG};
                color: {self.COULEUR_TEXT};
                font-family: 'Segoe UI', 'Ubuntu', sans-serif;
                font-size: 13px;
            }}
            QGroupBox {{
                border: 1px solid {self.COULEUR_SURFACE};
                border-radius: 8px;
                margin-top: 12px;
                padding: 8px;
                font-weight: bold;
                color: {self.COULEUR_ACCENT};
            }}
            QGroupBox::title {{
                subcontrol-origin: margin;
                left: 10px;
                padding: 0 6px;
            }}
            QPushButton {{
                background-color: {self.COULEUR_SURFACE};
                color: {self.COULEUR_TEXT};
                border: 1px solid #45475a;
                border-radius: 6px;
                padding: 6px 14px;
                font-weight: bold;
            }}
            QPushButton:hover  {{ background-color: #45475a; }}
            QPushButton:pressed {{ background-color: #585b70; }}
            QPushButton#btn_connect {{
                background-color: {self.COULEUR_OK};
                color: #1e1e2e;
            }}
            QPushButton#btn_connect:hover {{ background-color: #94e2a8; }}
            QPushButton#btn_disconnect {{
                background-color: {self.COULEUR_ERREUR};
                color: #1e1e2e;
            }}
            QPushButton#btn_disconnect:hover {{ background-color: #f5a0b5; }}
            QPushButton#btn_save {{
                background-color: {self.COULEUR_ACCENT};
                color: #1e1e2e;
            }}
            QPushButton#btn_save:hover {{ background-color: #a6c8ff; }}
            QComboBox, QSpinBox, QDoubleSpinBox {{
                background-color: {self.COULEUR_SURFACE};
                color: {self.COULEUR_TEXT};
                border: 1px solid #45475a;
                border-radius: 5px;
                padding: 4px 8px;
            }}
            QComboBox::drop-down {{ border: none; }}
            QTableWidget {{
                background-color: {self.COULEUR_SURFACE};
                gridline-color: #45475a;
                border: none;
                border-radius: 6px;
            }}
            QHeaderView::section {{
                background-color: #313244;
                color: {self.COULEUR_ACCENT};
                padding: 6px;
                font-weight: bold;
                border: none;
            }}
            QTableWidget::item:alternate {{ background-color: #292c3c; }}
            QStatusBar {{ background-color: {self.COULEUR_SURFACE}; }}
            QLabel#lbl_valeur_actuelle {{
                font-size: 36px;
                font-weight: bold;
                color: {self.COULEUR_ACCENT};
            }}
            QLabel#lbl_valeur_max {{
                font-size: 14px;
                color: {self.COULEUR_WARN};
            }}
        """)

    # ── Construction UI ──────────────────────────────────────────────────────
    def _build_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(10, 10, 10, 6)
        root.setSpacing(8)

        # ── Barre de configuration ──
        root.addWidget(self._build_config_bar())

        # ── Zone principale (splitter) ──
        splitter = QSplitter(Qt.Horizontal)
        splitter.setHandleWidth(6)

        # Gauche : tableau + valeurs live
        gauche = QWidget()
        gl = QVBoxLayout(gauche)
        gl.setContentsMargins(0, 0, 0, 0)
        gl.addWidget(self._build_live_values())
        gl.addWidget(self._build_table())
        splitter.addWidget(gauche)

        # Droite : courbe
        splitter.addWidget(self._build_graph())
        splitter.setSizes([320, 860])
        root.addWidget(splitter, 1)

        # ── Barre de statut ──
        self.status_bar = QStatusBar()
        self.setStatusBar(self.status_bar)
        self._set_status("Déconnecté", self.COULEUR_ERREUR)

    def _build_config_bar(self) -> QGroupBox:
        grp = QGroupBox("Configuration port série")
        h = QHBoxLayout(grp)
        h.setSpacing(10)

        # Port
        h.addWidget(QLabel("Port :"))
        self.combo_port = QComboBox()
        self.combo_port.setMinimumWidth(140)
        h.addWidget(self.combo_port)

        btn_refresh = QPushButton("↺ Rafraîchir")
        btn_refresh.clicked.connect(self._rafraichir_ports)
        h.addWidget(btn_refresh)

        # Baudrate
        h.addWidget(QLabel("Baudrate :"))
        self.combo_baud = QComboBox()
        for b in [9600, 19200, 38400, 57600, 115200]:
            self.combo_baud.addItem(str(b))
        h.addWidget(self.combo_baud)

        h.addSpacing(20)

        # Boutons connexion
        self.btn_connect = QPushButton("▶  Connecter")
        self.btn_connect.setObjectName("btn_connect")
        self.btn_connect.clicked.connect(self._connecter)
        h.addWidget(self.btn_connect)

        self.btn_disconnect = QPushButton("■  Déconnecter")
        self.btn_disconnect.setObjectName("btn_disconnect")
        self.btn_disconnect.setEnabled(False)
        self.btn_disconnect.clicked.connect(self._deconnecter)
        h.addWidget(self.btn_disconnect)

        h.addStretch()

        # Boutons données
        btn_clear = QPushButton("🗑 Effacer")
        btn_clear.clicked.connect(self._effacer_donnees)
        h.addWidget(btn_clear)

        self.btn_save = QPushButton("💾 Sauvegarder CSV")
        self.btn_save.setObjectName("btn_save")
        self.btn_save.clicked.connect(self._sauvegarder_csv)
        h.addWidget(self.btn_save)

        return grp

    def _build_live_values(self) -> QGroupBox:
        grp = QGroupBox("Valeurs en direct")
        v = QVBoxLayout(grp)
        v.setSpacing(4)

        self.lbl_valeur_actuelle = QLabel("— N")
        self.lbl_valeur_actuelle.setObjectName("lbl_valeur_actuelle")
        self.lbl_valeur_actuelle.setAlignment(Qt.AlignCenter)
        v.addWidget(self.lbl_valeur_actuelle)

        self.lbl_temps_actuel = QLabel("t = — s")
        self.lbl_temps_actuel.setAlignment(Qt.AlignCenter)
        v.addWidget(self.lbl_temps_actuel)

        sep = QFrame()
        sep.setFrameShape(QFrame.HLine)
        sep.setStyleSheet("color: #45475a;")
        v.addWidget(sep)

        self.lbl_valeur_max = QLabel("Max : — N")
        self.lbl_valeur_max.setObjectName("lbl_valeur_max")
        self.lbl_valeur_max.setAlignment(Qt.AlignCenter)
        v.addWidget(self.lbl_valeur_max)

        self.lbl_valeur_min = QLabel("Min : — N")
        self.lbl_valeur_min.setAlignment(Qt.AlignCenter)
        v.addWidget(self.lbl_valeur_min)

        self.lbl_nb_mesures = QLabel("Mesures : 0")
        self.lbl_nb_mesures.setAlignment(Qt.AlignCenter)
        v.addWidget(self.lbl_nb_mesures)

        return grp

    def _build_table(self) -> QGroupBox:
        grp = QGroupBox("Liste des mesures reçues")
        v = QVBoxLayout(grp)

        self.table = QTableWidget(0, 3)
        self.table.setHorizontalHeaderLabels(["#", "Temps (s)", "Effort (N)"])
        self.table.horizontalHeader().setSectionResizeMode(QHeaderView.Stretch)
        self.table.setAlternatingRowColors(True)
        self.table.setEditTriggers(QTableWidget.NoEditTriggers)
        self.table.setSelectionBehavior(QTableWidget.SelectRows)
        v.addWidget(self.table)

        return grp

    def _build_graph(self) -> QGroupBox:
        grp = QGroupBox("Courbe Effort = f(temps)")
        v = QVBoxLayout(grp)

        self.plot_widget = pg.PlotWidget()
        self.plot_widget.setLabel('left',   'Effort (N)')
        self.plot_widget.setLabel('bottom', 'Temps (s)')
        self.plot_widget.showGrid(x=True, y=True, alpha=0.3)
        self.plot_widget.addLegend()

        # Courbe principale
        pen = pg.mkPen(color=self.COULEUR_ACCENT, width=2)
        self.courbe = self.plot_widget.plot(
            [], [], pen=pen, name="Effort (N)",
            symbol='o', symbolSize=5,
            symbolBrush=self.COULEUR_ACCENT,
            symbolPen=pg.mkPen(self.COULEUR_ACCENT)
        )

        # Ligne horizontale max
        self.ligne_max = pg.InfiniteLine(
            angle=0, movable=False,
            pen=pg.mkPen(color=self.COULEUR_WARN, width=1, style=Qt.DashLine),
            label='Max', labelOpts={'color': self.COULEUR_WARN}
        )
        self.plot_widget.addItem(self.ligne_max)

        v.addWidget(self.plot_widget)
        return grp

    # ── Ports série ─────────────────────────────────────────────────────────
    def _rafraichir_ports(self):
        self.combo_port.clear()
        ports = serial.tools.list_ports.comports()
        for p in ports:
            self.combo_port.addItem(p.device)
        if not ports:
            self.combo_port.addItem("(aucun port)")

    # ── Connexion / déconnexion ──────────────────────────────────────────────
    def _connecter(self):
        port = self.combo_port.currentText()
        baud = int(self.combo_baud.currentText())

        if not port or port.startswith("("):
            QMessageBox.warning(self, "Erreur", "Veuillez sélectionner un port série valide.")
            return

        self._thread = SerialReaderThread(port, baud)
        self._thread.nouvelle_trame.connect(self._recevoir_trame)
        self._thread.erreur.connect(self._erreur_serie)
        self._thread.start()

        self.btn_connect.setEnabled(False)
        self.btn_disconnect.setEnabled(True)
        self.combo_port.setEnabled(False)
        self.combo_baud.setEnabled(False)
        self._set_status(f"Connecté sur {port} @ {baud} baud", self.COULEUR_OK)

    def _deconnecter(self):
        if self._thread:
            self._thread.stop()
            self._thread = None

        self.btn_connect.setEnabled(True)
        self.btn_disconnect.setEnabled(False)
        self.combo_port.setEnabled(True)
        self.combo_baud.setEnabled(True)
        self._set_status("Déconnecté", self.COULEUR_ERREUR)

    def _erreur_serie(self, msg: str):
        self._deconnecter()
        QMessageBox.critical(self, "Erreur série", msg)

    # ── Réception trame ──────────────────────────────────────────────────────
    def _recevoir_trame(self, t: float, fn: float):
        self._nb_total += 1
        self.temps_data.append(t)
        self.effort_data.append(fn)

        # ── Tableau ──
        row = self.table.rowCount()
        self.table.insertRow(row)
        item_n  = QTableWidgetItem(str(self._nb_total))
        item_t  = QTableWidgetItem(f"{t:.3f}")
        item_fn = QTableWidgetItem(f"{fn:.4f}")
        for item in (item_n, item_t, item_fn):
            item.setTextAlignment(Qt.AlignCenter)
        self.table.setItem(row, 0, item_n)
        self.table.setItem(row, 1, item_t)
        self.table.setItem(row, 2, item_fn)
        self.table.scrollToBottom()

        # ── Valeurs live ──
        self.lbl_valeur_actuelle.setText(f"{fn:.4f} N")
        self.lbl_temps_actuel.setText(f"t = {t:.3f} s")

        fmax = max(self.effort_data)
        fmin = min(self.effort_data)
        self.lbl_valeur_max.setText(f"Max : {fmax:.4f} N")
        self.lbl_valeur_min.setText(f"Min : {fmin:.4f} N")
        self.lbl_nb_mesures.setText(f"Mesures : {self._nb_total}")

        # ── Courbe ──
        self.courbe.setData(self.temps_data, self.effort_data)
        self.ligne_max.setPos(fmax)

        # ── Barre statut ──
        self._set_status(
            f"Connecté  │  Dernière trame : t={t:.3f} s  │  F={fn:.4f} N  │  Total : {self._nb_total} mesures",
            self.COULEUR_OK
        )

    # ── Effacer / sauvegarder ───────────────────────────────────────────────
    def _effacer_donnees(self):
        rep = QMessageBox.question(
            self, "Effacer", "Effacer toutes les données affichées ?",
            QMessageBox.Yes | QMessageBox.No
        )
        if rep == QMessageBox.Yes:
            self.temps_data.clear()
            self.effort_data.clear()
            self._nb_total = 0
            self.table.setRowCount(0)
            self.courbe.setData([], [])
            self.ligne_max.setPos(0)
            self.lbl_valeur_actuelle.setText("— N")
            self.lbl_temps_actuel.setText("t = — s")
            self.lbl_valeur_max.setText("Max : — N")
            self.lbl_valeur_min.setText("Min : — N")
            self.lbl_nb_mesures.setText("Mesures : 0")

    def _sauvegarder_csv(self):
        if not self.temps_data:
            QMessageBox.information(self, "Aucune donnée", "Pas de données à sauvegarder.")
            return

        horodatage = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        chemin, _ = QFileDialog.getSaveFileName(
            self, "Sauvegarder les données",
            f"mesure_effort_{horodatage}.csv",
            "Fichiers CSV (*.csv)"
        )
        if not chemin:
            return

        try:
            with open(chemin, 'w', newline='', encoding='utf-8') as f:
                writer = csv.writer(f, delimiter=';')
                writer.writerow(["N°", "Temps (s)", "Effort (N)"])
                for i, (t, fn) in enumerate(zip(self.temps_data, self.effort_data), 1):
                    writer.writerow([i, f"{t:.3f}", f"{fn:.4f}"])
            QMessageBox.information(
                self, "Sauvegarde réussie",
                f"{len(self.temps_data)} mesures sauvegardées dans :\n{chemin}"
            )
        except OSError as e:
            QMessageBox.critical(self, "Erreur de sauvegarde", str(e))

    # ── Barre de statut ──────────────────────────────────────────────────────
    def _set_status(self, msg: str, couleur: str):
        self.status_bar.showMessage(msg)
        self.status_bar.setStyleSheet(f"QStatusBar {{ background-color: {self.COULEUR_SURFACE}; color: {couleur}; }}")

    # ── Fermeture propre ─────────────────────────────────────────────────────
    def closeEvent(self, event):
        self._deconnecter()
        event.accept()


# ─────────────────────────────────────────────────────────────────────────────
if __name__ == '__main__':
    app = QApplication(sys.argv)
    app.setStyle('Fusion')
    win = MainWindow()
    win.show()
    sys.exit(app.exec_())
