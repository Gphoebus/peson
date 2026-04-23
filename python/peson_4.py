#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Visualiseur Mesure d'Effort - Arduino HX711
Compatible EduPython / conda Python 3.7
Graphique : matplotlib (FigureCanvasQTAgg)
Trame attendue : "secondes;newtons\n"

M. Parent - Lycee de la Borde Basse
"""

import sys
import csv
import serial
import serial.tools.list_ports

from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QMessageBox, QFileDialog, QTableWidgetItem, QVBoxLayout
)
from PyQt5.QtCore import QThread, pyqtSignal
from PyQt5 import uic

# --- Matplotlib embarque dans Qt (disponible dans EduPython/conda) -----------
import matplotlib
matplotlib.use('Qt5Agg')                           # backend Qt5
from matplotlib.figure import Figure
from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg as FigureCanvas

# =============================================================================
# Thread de lecture serie (inchange, compatible Python 3.7)
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
# Widget graphique matplotlib
# =============================================================================
class GraphiqueMatplotlib(FigureCanvas):
    """Canvas matplotlib pret a etre insere dans un QGroupBox."""

    COULEUR_BG     = '#1e1e2e'
    COULEUR_AXES   = '#313244'
    COULEUR_TEXTE  = '#cdd6f4'
    COULEUR_COURBE = '#89b4fa'   # bleu
    COULEUR_MAX    = '#fab387'   # orange

    def __init__(self):
        self.fig = Figure(facecolor=self.COULEUR_BG)
        super(GraphiqueMatplotlib, self).__init__(self.fig)

        self.ax = self.fig.add_subplot(111)
        self._configurer_axes()

        # Objets graphiques
        self.courbe,  = self.ax.plot([], [], color=self.COULEUR_COURBE,
                                     linewidth=2, marker='o', markersize=4,
                                     label='Effort (N)')
        self.ligne_max = self.ax.axhline(y=0, color=self.COULEUR_MAX,
                                          linestyle='--', linewidth=1,
                                          label='Max')
        self.ax.legend(facecolor=self.COULEUR_AXES, edgecolor='#45475a',
                       labelcolor=self.COULEUR_TEXTE, fontsize=9)

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
        """Met a jour la courbe et la ligne de maximum."""
        if not temps:
            return

        self.courbe.set_data(temps, efforts)

        fmax = max(efforts)
        self.ligne_max.set_ydata([fmax, fmax])

        # Recalcul des limites
        marge_x = max((temps[-1] - temps[0]) * 0.05, 0.5) if len(temps) > 1 else 1.0
        self.ax.set_xlim(temps[0] - marge_x * 0.1, temps[-1] + marge_x)

        fmin = min(efforts)
        plage = max(abs(fmax - fmin), 0.01)
        self.ax.set_ylim(fmin - plage * 0.15, fmax + plage * 0.25)

        self.draw()   # rafraichissement immediat

    def effacer(self):
        self.courbe.set_data([], [])
        self.ligne_max.set_ydata([0, 0])
        self.ax.set_xlim(0, 10)
        self.ax.set_ylim(-0.1, 1)
        self.draw()


# =============================================================================
# Fenetre principale
# =============================================================================
class MainWindow(QMainWindow):

    def __init__(self):
        super().__init__()

        # Chargement du fichier .ui (doit etre dans le meme dossier)
        try:
            uic.loadUi("ui_mainwindow.ui", self)
        except Exception as e:
            QMessageBox.critical(None, "Erreur UI", "Impossible de charger ui_mainwindow.ui :\n" + str(e))
            sys.exit(1)

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

        # Graphique matplotlib insere dans groupGraph
        self._init_graphique()

        # Connexions boutons
        self.btn_connect.clicked.connect(self._connecter)
        self.btn_disconnect.clicked.connect(self._deconnecter)
        self.btn_clear.clicked.connect(self._effacer_donnees)
        self.btn_save.clicked.connect(self._sauvegarder_csv)

        # Remplissage liste des ports
        self._rafraichir_ports()

    # -------------------------------------------------------------------------
    # Graphique
    # -------------------------------------------------------------------------
    def _init_graphique(self):
        """Cree le canvas matplotlib et l'insere dans le layout_graph du .ui."""
        self.canvas = GraphiqueMatplotlib()

        # layout_graph est defini dans le .ui avec un QVBoxLayout vide
        # On l'utilise directement sans en recreer un nouveau
        layout = self.groupGraph.layout()   # recupere le QVBoxLayout existant
        layout.addWidget(self.canvas)
        layout.setContentsMargins(4, 4, 4, 4)

    # -------------------------------------------------------------------------
    # Ports serie
    # -------------------------------------------------------------------------
    def _rafraichir_ports(self):
        self.combo_port.clear()
        ports = [p.device for p in serial.tools.list_ports.comports()]
        if ports:
            self.combo_port.addItems(ports)
        else:
            self.combo_port.addItem("(aucun port detecte)")

    # -------------------------------------------------------------------------
    # Connexion / deconnexion
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
    # Reception d'une trame
    # -------------------------------------------------------------------------
    def _recevoir_trame(self, t, fn):
        self.temps_data.append(t)
        self.effort_data.append(fn)

        # -- Labels live --
        self.lbl_valeur_actuelle.setText("{:.4f} N".format(fn))
        self.lbl_temps_actuel.setText("t = {:.3f} s".format(t))

        fmax = max(self.effort_data)
        fmin = min(self.effort_data)
        self.lbl_valeur_max.setText("Max : {:.4f} N".format(fmax))
        self.lbl_valeur_min.setText("Min : {:.4f} N".format(fmin))
        self.lbl_nb_mesures.setText("Mesures : {}".format(len(self.effort_data)))

        # -- Tableau --
        row = self.table.rowCount()
        self.table.insertRow(row)
        self.table.setItem(row, 0, QTableWidgetItem(str(row + 1)))
        self.table.setItem(row, 1, QTableWidgetItem("{:.3f}".format(t)))
        self.table.setItem(row, 2, QTableWidgetItem("{:.4f}".format(fn)))
        self.table.scrollToBottom()

        # -- Graphique matplotlib --
        self.canvas.mettre_a_jour(self.temps_data, self.effort_data)

        # -- Barre de statut --
        self.statusbar.showMessage(
            "Derniere trame : t={:.3f} s | F={:.4f} N | Total : {} mesures".format(
                t, fn, len(self.effort_data)
            )
        )

    # -------------------------------------------------------------------------
    # Effacer / sauvegarder
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
    # Fermeture propre
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
