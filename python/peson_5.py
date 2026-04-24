#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Visualiseur Mesure d'Effort - Arduino HX711
Compatible EduPython / conda Python 3.7
Graphique : matplotlib (FigureCanvasQTAgg)
Trame attendue : "secondes;newtons\n"

 - Lycee de la Borde Basse

VERSION 5 — Compatible PyInstaller (--onefile)
Modifications :
  - Suppression de uic.loadUi() → interface chargee depuis ui_mainwindow.py
  - Ajout resource_path() pour les ressources embarquees
  - Import explicite du backend matplotlib (evite l'erreur ImportError dans l'exe)
  - Import serial.tools.list_ports explicite (contournement PyInstaller)
"""

import sys
import os
import csv

# ---------------------------------------------------------------------------
# Fonction resource_path : indispensable pour PyInstaller --onefile
# Permet de localiser les fichiers embarques dans le bundle temporaire _MEIPASS
# ---------------------------------------------------------------------------
def resource_path(relative_path):
    """Retourne le chemin absolu vers une ressource, compatible PyInstaller."""
    if hasattr(sys, '_MEIPASS'):
        return os.path.join(sys._MEIPASS, relative_path)
    return os.path.join(os.path.abspath("."), relative_path)


# ---------------------------------------------------------------------------
# Imports serie — import explicite de list_ports pour eviter l'omission
# PyInstaller ne detecte pas toujours les sous-modules de pyserial
# ---------------------------------------------------------------------------
import serial
import serial.tools.list_ports  # noqa: F401  — force l'inclusion dans l'exe

# ---------------------------------------------------------------------------
# Imports Qt
# ---------------------------------------------------------------------------
from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QMessageBox, QFileDialog,
    QTableWidgetItem, QVBoxLayout
)
from PyQt5.QtCore import QThread, pyqtSignal

# ---------------------------------------------------------------------------
# Interface generee par pyuic5 (remplace uic.loadUi)
# PyInstaller gere un fichier .py, pas un .ui dynamique
# ---------------------------------------------------------------------------
from ui_mainwindow import Ui_MainWindow

# ---------------------------------------------------------------------------
# Matplotlib — backend force AVANT tout autre import matplotlib
# PyInstaller a besoin que le backend soit declare explicitement
# ---------------------------------------------------------------------------
import matplotlib
matplotlib.use('Qt5Agg')  # doit etre avant l'import de FigureCanvas

from matplotlib.figure import Figure
from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg as FigureCanvas

# Imports supplementaires forces pour PyInstaller
# (evite "ModuleNotFoundError: No module named 'matplotlib.backends.backend_qt5agg'")
import matplotlib.backends.backend_qt5agg   # noqa: F401
import matplotlib.backends.backend_agg      # noqa: F401


# =============================================================================
# Thread de lecture serie (inchange)
# =============================================================================
class SerialReaderThread(QThread):
    nouvelle_trame = pyqtSignal(float, float)
    erreur         = pyqtSignal(str)

    def __init__(self, port, baudrate):
        super().__init__()
        self.port     = port
        self.baudrate = baudrate
        self._running = True

    def run(self):
        try:
            with serial.Serial(self.port, self.baudrate, timeout=1) as ser:
                ser.reset_input_buffer()
                while self._running:
                    if ser.in_waiting:
                        line = ser.readline().decode('utf-8', errors='ignore').strip()
                        if ';' in line:
                            try:
                                parts = line.split(';')
                                t  = float(parts[0].strip().replace(',', '.'))
                                fn = float(parts[1].strip().replace(',', '.'))
                                self.nouvelle_trame.emit(t, fn)
                            except (ValueError, IndexError):
                                continue
        except Exception as e:
            self.erreur.emit(str(e))

    def stop(self):
        self._running = False
        self.wait()


# =============================================================================
# Widget graphique matplotlib (inchange)
# =============================================================================
class GraphiqueMatplotlib(FigureCanvas):
    """Canvas matplotlib pret a etre insere dans un QGroupBox."""

    COULEUR_BG     = '#1e1e2e'
    COULEUR_AXES   = '#313244'
    COULEUR_TEXTE  = '#cdd6f4'
    COULEUR_COURBE = '#89b4fa'
    COULEUR_MAX    = '#fab387'

    def __init__(self):
        self.fig = Figure(facecolor=self.COULEUR_BG)
        super(GraphiqueMatplotlib, self).__init__(self.fig)

        self.ax = self.fig.add_subplot(111)
        self._configurer_axes()

        self.courbe,  = self.ax.plot([], [], color=self.COULEUR_COURBE,
                                     linewidth=2, marker='o', markersize=4,
                                     label='Effort (N)')
        self.ligne_max = self.ax.axhline(y=0, color=self.COULEUR_MAX,
                                          linestyle='--', linewidth=1,
                                          label='Max')
        leg = self.ax.legend(facecolor=self.COULEUR_AXES, edgecolor='#45475a',
                             fontsize=9)
        for text in leg.get_texts():
            text.set_color(self.COULEUR_TEXTE)
        self.fig.tight_layout(pad=1.5)

    def _configurer_axes(self):
        self.ax.set_facecolor(self.COULEUR_AXES)
        self.ax.tick_params(colors=self.COULEUR_TEXTE, labelsize=9)
        self.ax.set_xlabel('Temps (s)', color=self.COULEUR_TEXTE, fontsize=10)
        self.ax.set_ylabel('Effort (N)', color=self.COULEUR_TEXTE, fontsize=10)
        self.ax.set_title('Effort = f(temps)', color=self.COULEUR_TEXTE, fontsize=11)
        for spine in self.ax.spines.values():
            spine.set_edgecolor('#45475a')
        self.ax.grid(True, color='#45475a', linestyle='--', linewidth=0.5, alpha=0.7)

    def mettre_a_jour(self, temps, efforts):
        if not temps:
            return
        self.courbe.set_data(temps, efforts)
        fmax = max(efforts)
        self.ligne_max.set_ydata([fmax, fmax])
        marge_x = max((temps[-1] - temps[0]) * 0.05, 0.5) if len(temps) > 1 else 1.0
        self.ax.set_xlim(temps[0] - marge_x * 0.1, temps[-1] + marge_x)
        fmin = min(efforts)
        plage = max(abs(fmax - fmin), 0.01)
        self.ax.set_ylim(fmin - plage * 0.15, fmax + plage * 0.25)
        self.draw()

    def effacer(self):
        self.courbe.set_data([], [])
        self.ligne_max.set_ydata([0, 0])
        self.ax.set_xlim(0, 10)
        self.ax.set_ylim(-0.1, 1)
        self.draw()


# =============================================================================
# Fenetre principale — utilise Ui_MainWindow (plus de uic.loadUi)
# =============================================================================
class MainWindow(QMainWindow):

    def __init__(self):
        super().__init__()

        # ---------------------------------------------------------------
        # MODIFICATION CLE : setupUi() depuis la classe Python generee
        # par pyuic5, au lieu de uic.loadUi("fichier.ui")
        # PyInstaller ne peut pas embarquer les .ui dynamiques
        # ---------------------------------------------------------------
        self.ui = Ui_MainWindow()
        self.ui.setupUi(self)

        # Raccourcis directs vers les widgets (confort de code)
        self.combo_port        = self.ui.combo_port
        self.combo_baud        = self.ui.combo_baud
        self.btn_connect       = self.ui.btn_connect
        self.btn_disconnect    = self.ui.btn_disconnect
        self.btn_clear         = self.ui.btn_clear
        self.btn_save          = self.ui.btn_save
        self.lbl_valeur_actuelle = self.ui.lbl_valeur_actuelle
        self.lbl_temps_actuel  = self.ui.lbl_temps_actuel
        self.lbl_valeur_max    = self.ui.lbl_valeur_max
        self.lbl_valeur_min    = self.ui.lbl_valeur_min
        self.lbl_nb_mesures    = self.ui.lbl_nb_mesures
        self.table             = self.ui.table
        self.groupGraph        = self.ui.groupGraph
        self.statusbar         = self.statusBar()

        # Donnees
        self.temps_data  = []
        self.effort_data = []
        self._thread     = None

        # Configuration tableau
        self.table.setColumnCount(3)
        self.table.setHorizontalHeaderLabels(["N", "Temps (s)", "Effort (N)"])
        self.table.setRowCount(0)
        self.table.horizontalHeader().setStretchLastSection(True)
        self.table.setAlternatingRowColors(True)

        # Graphique matplotlib
        self._init_graphique()

        # Connexions boutons
        self.btn_connect.clicked.connect(self._connecter)
        self.btn_disconnect.clicked.connect(self._deconnecter)
        self.btn_clear.clicked.connect(self._effacer_donnees)
        self.btn_save.clicked.connect(self._sauvegarder_csv)

        # Remplissage liste des ports
        self._rafraichir_ports()

    # -------------------------------------------------------------------------
    def _init_graphique(self):
        self.canvas = GraphiqueMatplotlib()
        layout = self.groupGraph.layout()
        layout.addWidget(self.canvas)
        layout.setContentsMargins(4, 4, 4, 4)

    # -------------------------------------------------------------------------
    def _rafraichir_ports(self):
        self.combo_port.clear()
        ports = [p.device for p in serial.tools.list_ports.comports()]
        if ports:
            self.combo_port.addItems(ports)
        else:
            self.combo_port.addItem("(aucun port detecte)")

    # -------------------------------------------------------------------------
    def _connecter(self):
        port = self.combo_port.currentText()
        if not port or port.startswith("("):
            QMessageBox.warning(self, "Port invalide", "Selectionnez un port serie valide.")
            return
        baud = int(self.combo_baud.currentText())
        self._thread = SerialReaderThread(port, baud)
        self._thread.nouvelle_trame.connect(self._recevoir_trame)
        self._thread.erreur.connect(self._erreur_serie)
        self._thread.start()
        self.btn_connect.setEnabled(False)
        self.btn_disconnect.setEnabled(True)
        self.statusbar.showMessage("Connecte sur {} @ {} baud".format(port, baud))

    def _deconnecter(self):
        if self._thread:
            self._thread.stop()
            self._thread = None
        self.btn_connect.setEnabled(True)
        self.btn_disconnect.setEnabled(False)
        self.statusbar.showMessage("Deconnecte")

    def _erreur_serie(self, msg):
        self._deconnecter()
        QMessageBox.critical(self, "Erreur serie", msg)

    # -------------------------------------------------------------------------
    def _recevoir_trame(self, t, fn):
        self.temps_data.append(t)
        self.effort_data.append(fn)

        self.lbl_valeur_actuelle.setText("{:.4f} N".format(fn))
        self.lbl_temps_actuel.setText("t = {:.3f} s".format(t))

        fmax = max(self.effort_data)
        fmin = min(self.effort_data)
        self.lbl_valeur_max.setText("Max : {:.4f} N".format(fmax))
        self.lbl_valeur_min.setText("Min : {:.4f} N".format(fmin))
        self.lbl_nb_mesures.setText("Mesures : {}".format(len(self.effort_data)))

        row = self.table.rowCount()
        self.table.insertRow(row)
        self.table.setItem(row, 0, QTableWidgetItem(str(row + 1)))
        self.table.setItem(row, 1, QTableWidgetItem("{:.3f}".format(t)))
        self.table.setItem(row, 2, QTableWidgetItem("{:.4f}".format(fn)))
        self.table.scrollToBottom()

        self.canvas.mettre_a_jour(self.temps_data, self.effort_data)

        self.statusbar.showMessage(
            "Derniere trame : t={:.3f} s | F={:.4f} N | Total : {} mesures".format(
                t, fn, len(self.effort_data)
            )
        )

    # -------------------------------------------------------------------------
    def _effacer_donnees(self):
        rep = QMessageBox.question(
            self, "Effacer",
            "Effacer toutes les donnees ?",
            QMessageBox.Yes | QMessageBox.No
        )
        if rep == QMessageBox.Yes:
            self.temps_data  = []
            self.effort_data = []
            self.table.setRowCount(0)
            self.canvas.effacer()
            self.lbl_valeur_actuelle.setText("0.0000 N")
            self.lbl_temps_actuel.setText("t = 0.000 s")
            self.lbl_valeur_max.setText("Max : 0 N")
            self.lbl_valeur_min.setText("Min : 0 N")
            self.lbl_nb_mesures.setText("Mesures : 0")

    def _sauvegarder_csv(self):
        if not self.temps_data:
            QMessageBox.information(self, "Vide", "Pas de donnees a sauvegarder.")
            return
        path, _ = QFileDialog.getSaveFileName(
            self, "Sauvegarder les donnees", "mesure_effort.csv", "CSV (*.csv)"
        )
        if not path:
            return
        try:
            with open(path, 'w', newline='', encoding='utf-8') as f:
                writer = csv.writer(f, delimiter=';')
                writer.writerow(['N', 'Temps (s)', 'Effort (N)'])
                for i, (t, fn) in enumerate(zip(self.temps_data, self.effort_data), 1):
                    writer.writerow([i, "{:.3f}".format(t), "{:.4f}".format(fn)])
            QMessageBox.information(
                self, "Sauvegarde OK",
                "{} mesures sauvegardees dans :\n{}".format(len(self.temps_data), path)
            )
        except Exception as e:
            QMessageBox.critical(self, "Erreur", str(e))

    # -------------------------------------------------------------------------
    def closeEvent(self, event):
        self._deconnecter()
        event.accept()


# =============================================================================
if __name__ == '__main__':
    app = QApplication(sys.argv)
    win = MainWindow()
    win.show()
    sys.exit(app.exec_())
