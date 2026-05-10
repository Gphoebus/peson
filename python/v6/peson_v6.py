#!/usr/bin/env python3
"""
Balance HX711 - Interface PyQt5
Double connexion : Bluetooth BLE + Port Série
Sauvegarde CSV automatique
Format données série: temps;effort (ex: 0.222;-0.0296)
Baudrate: 9600
"""

import sys
import os
import csv
import time
import asyncio
from datetime import datetime
from collections import deque
from pathlib import Path
import threading

from PyQt5 import uic
from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QMessageBox, QFileDialog, QVBoxLayout
)
from PyQt5.QtCore import Qt, QThread, pyqtSignal, QTimer

import serial
import serial.tools.list_ports

import matplotlib
matplotlib.use('Qt5Agg')
from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg as FigureCanvas
from matplotlib.figure import Figure

# Bluetooth BLE (optionnel)
try:
    from bleak import BleakScanner, BleakClient
    BLUETOOTH_AVAILABLE = True
except ImportError:
    BLUETOOTH_AVAILABLE = False
    print("⚠️  Bleak non installé. pip install bleak")

# UUIDs BLE
SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CHARACTERISTIC_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"


# ============================================================
# CANVAS MATPLOTLIB
# ============================================================
class MatplotlibCanvas(FigureCanvas):
    def __init__(self, parent=None):
        self.fig = Figure(figsize=(10, 3), dpi=100, facecolor='#1e1e2e')
        self.axes = self.fig.add_subplot(111)
        self.axes.set_facecolor('#313244')
        super().__init__(self.fig)
        self.setParent(parent)
        self.axes.set_title('Effort en temps réel', color='#cdd6f4', fontweight='bold')
        self.axes.set_xlabel('Temps (s)', color='#cdd6f4')
        self.axes.set_ylabel('Effort (N)', color='#cdd6f4')
        self.axes.tick_params(colors='#cdd6f4')
        self.axes.grid(True, alpha=0.3, color='#45475a', linestyle='--')
        for spine in self.axes.spines.values():
            spine.set_color('#45475a')
        self.fig.tight_layout()


# ============================================================
# THREAD SÉRIE - VERSION FONCTIONNELLE (9600 bauds)
# ============================================================
class SerialReaderThread(QThread):
    nouvelle_trame = pyqtSignal(float, float)
    erreur = pyqtSignal(str)
    connexion_change = pyqtSignal(bool, str)

    def __init__(self, port, baudrate=9600):
        super().__init__()
        self.port = port
        self.baudrate = baudrate
        self._running = True

    def run(self):
        try:
            print(f"[SERIE] Connexion à {self.port} @ {self.baudrate} bauds")
            with serial.Serial(self.port, self.baudrate, timeout=1) as ser:
                ser.reset_input_buffer()
                self.connexion_change.emit(True, f"Série: {self.port} @ {self.baudrate} bauds")
                print("[SERIE] Connecté, en attente de données...")
                
                while self._running:
                    if ser.in_waiting:
                        line = ser.readline().decode('utf-8', errors='ignore').strip()
                        print(f"[SERIE] Ligne reçue: '{line}'")
                        
                        if ';' in line:
                            try:
                                parts = line.split(';')
                                t = float(parts[0].strip().replace(',', '.'))
                                fn = float(parts[1].strip().replace(',', '.'))
                                print(f"[SERIE] Parsé: temps={t}, effort={fn}")
                                self.nouvelle_trame.emit(t, fn)
                            except (ValueError, IndexError) as e:
                                print(f"[SERIE] Erreur parsing: {e} - ligne: '{line}'")
                                continue
                    else:
                        self.msleep(10)
                        
        except serial.SerialException as e:
            error_msg = f"Erreur série: {e}"
            print(f"[SERIE] {error_msg}")
            self.erreur.emit(error_msg)
            self.connexion_change.emit(False, error_msg)
        except Exception as e:
            error_msg = f"Erreur: {e}"
            print(f"[SERIE] {error_msg}")
            self.erreur.emit(error_msg)
            self.connexion_change.emit(False, error_msg)

    def stop(self):
        self._running = False
        self.wait()
        self.connexion_change.emit(False, "Série déconnecté")


# ============================================================
# THREAD BLE
# ============================================================
class BLEWorker(QThread):
    data_received = pyqtSignal(str)
    connection_changed = pyqtSignal(bool, str)
    devices_found = pyqtSignal(list)
    error_occurred = pyqtSignal(str)

    def __init__(self):
        super().__init__()
        self.client = None
        self.running = False

    def scan(self):
        if not BLUETOOTH_AVAILABLE:
            return
        try:
            loop = asyncio.new_event_loop()
            asyncio.set_event_loop(loop)
            async def _scan():
                devices = await BleakScanner.discover(timeout=5.0)
                return [{'name': d.name or 'Inconnu', 'addr': d.address}
                       for d in devices if d.name and 'ESP32' in d.name.upper()]
            results = loop.run_until_complete(_scan())
            self.devices_found.emit(results)
            loop.close()
        except Exception as e:
            self.connection_changed.emit(False, f"Scan BLE: {e}")

    def connect_ble(self, address):
        if not BLUETOOTH_AVAILABLE:
            return
        self.running = True
        try:
            loop = asyncio.new_event_loop()
            asyncio.set_event_loop(loop)
            async def _connect():
                self.client = BleakClient(address)
                await self.client.connect()
                self.connection_changed.emit(True, f"BLE: {address}")
                def handler(sender, data):
                    try:
                        msg = data.decode('utf-8', errors='ignore').strip()
                        if msg:
                            print(f"[BLE] Reçu: {msg}")
                            self.data_received.emit(msg)
                    except Exception as e:
                        print(f"[BLE] Erreur décodage: {e}")
                await self.client.start_notify(CHARACTERISTIC_UUID, handler)
                while self.running and self.client.is_connected:
                    await asyncio.sleep(0.5)
            loop.run_until_complete(_connect())
            loop.close()
        except Exception as e:
            self.connection_changed.emit(False, f"BLE: {e}")

    def disconnect(self):
        self.running = False
        if self.client:
            try:
                loop = asyncio.new_event_loop()
                asyncio.set_event_loop(loop)
                loop.run_until_complete(self.client.disconnect())
                loop.close()
            except:
                pass
        self.connection_changed.emit(False, "BLE déconnecté")

    def send(self, cmd):
        if self.client and self.client.is_connected:
            try:
                loop = asyncio.new_event_loop()
                asyncio.set_event_loop(loop)
                loop.run_until_complete(
                    self.client.write_gatt_char(CHARACTERISTIC_UUID, f"{cmd}\n".encode())
                )
                loop.close()
            except:
                pass


# ============================================================
# APPLICATION PRINCIPALE
# ============================================================
class BalanceApp(QMainWindow):
    def __init__(self):
        super().__init__()

        # Charger l'interface UI
        ui_path = Path(__file__).parent / "balance_interface.ui"
        if not ui_path.exists():
            QMessageBox.critical(self, "Erreur", f"Fichier UI introuvable:\n{ui_path}")
            sys.exit(1)
        uic.loadUi(ui_path, self)

        # Workers
        self.serial_thread = None
        self.ble_worker = BLEWorker() if BLUETOOTH_AVAILABLE else None

        # Données pour le graphique
        self.temps_list = deque(maxlen=500)
        self.effort_list = deque(maxlen=500)
        self.all_data = []
        self.start_time = None
        
        # Statistiques
        self.stats = {
            'max': float('-inf'), 
            'min': float('inf'), 
            'sum': 0.0, 
            'count': 0,
            'last_temps': 0.0,
            'last_effort': 0.0
        }

        # CSV
        self.csv_file = None
        self.csv_writer = None
        self.csv_active = False

        # Configurer les spinboxes
        self.doubleSpinBox_cal.setRange(0.1, 50000)
        self.doubleSpinBox_cal.setValue(1000)
        self.doubleSpinBox_cal.setSuffix(" N")
        self.doubleSpinBox_factor.setRange(1, 100000)
        self.doubleSpinBox_factor.setValue(2280)

        # Configuration du LCD
        self.lcd_poids.setDigitCount(8)
        self.lcd_poids.setSegmentStyle(self.lcd_poids.Flat)
        self.lcd_poids.display(0.0)
        self.label_unite.setText("Newton (N)")

        # Graphique Matplotlib
        self.canvas = MatplotlibCanvas(self.widget_graph)
        layout = QVBoxLayout(self.widget_graph)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.canvas)

        # Timer graphique
        self.graph_timer = QTimer()
        self.graph_timer.timeout.connect(self.update_graph)
        self.graph_timer.start(200)

        # Timer CSV
        self.csv_timer = QTimer()
        self.csv_timer.timeout.connect(self.flush_csv)
        self.csv_timer.start(1000)

        # Connecter les signaux
        self.connect_signals()

        # Init UI
        self.label_status.setText("🟡 Prêt - En attente de connexion")
        self.scan_serial_ports()

        # Désactiver BLE si non disponible
        if not BLUETOOTH_AVAILABLE:
            self.groupBox_ble.setTitle("Bluetooth BLE (non installé)")
            self.pushButton_scan_ble.setEnabled(False)
            self.pushButton_connect_ble.setEnabled(False)
            self.pushButton_disconnect_ble.setEnabled(False)
            self.comboBox_ble_devices.addItem("Installez: pip install bleak")

        # Ajuster pour grand écran
        self.adjust_for_large_screen()

    def adjust_for_large_screen(self):
        """Ajuster l'interface pour les grands écrans"""
        screen = QApplication.primaryScreen()
        if screen:
            screen_geometry = screen.availableGeometry()
            screen_width = screen_geometry.width()
            
            if screen_width >= 1600:
                new_width = min(1400, int(screen_width * 0.85))
                new_height = min(950, int(screen_geometry.height() * 0.85))
                self.resize(new_width, new_height)
                self.lcd_poids.setMinimumHeight(150)
                self.widget_graph.setMinimumHeight(300)

    # ============================================================
    # CONNEXION DES SIGNAUX
    # ============================================================
    def connect_signals(self):
        # === SÉRIE ===
        self.pushButton_scan_serial.clicked.connect(self.scan_serial_ports)
        self.pushButton_connect_serial.clicked.connect(self.connect_serial)
        self.pushButton_disconnect_serial.clicked.connect(self.disconnect_serial)

        # === BLE ===
        if self.ble_worker:
            self.pushButton_scan_ble.clicked.connect(self.scan_ble)
            self.pushButton_connect_ble.clicked.connect(self.connect_ble)
            self.pushButton_disconnect_ble.clicked.connect(self.disconnect_ble)
            self.ble_worker.data_received.connect(self.process_ble_data)
            self.ble_worker.connection_changed.connect(self.on_connection_change)
            self.ble_worker.devices_found.connect(self.on_ble_devices_found)
            self.ble_worker.error_occurred.connect(self.show_error)

        # === COMMANDES ===
        self.pushButton_tare.clicked.connect(self.tare)
        self.pushButton_calibrer.clicked.connect(self.calibrate)
        self.pushButton_factor.clicked.connect(self.set_factor)
        self.pushButton_reset_stats.clicked.connect(self.reset_stats)

        # === CSV ===
        self.checkBox_csv.toggled.connect(self.toggle_csv)
        self.pushButton_export.clicked.connect(self.export_csv)

        # === MENU ===
        self.actionExporter.triggered.connect(self.export_csv)
        self.actionEffacer.triggered.connect(self.clear_data)
        self.actionQuitter.triggered.connect(self.close)
        self.actionApropos.triggered.connect(self.show_about)

    # ============================================================
    # CONNEXIONS
    # ============================================================
    def scan_serial_ports(self):
        self.comboBox_ports.clear()
        ports = serial.tools.list_ports.comports()
        for p in ports:
            desc = f"{p.device} - {p.description}"
            if p.manufacturer:
                desc += f" [{p.manufacturer}]"
            if 'CH340' in p.description or 'CP210' in p.description or 'ESP32' in p.description.upper():
                desc += " ⭐"
            self.comboBox_ports.addItem(desc, p.device)
        if self.comboBox_ports.count() == 0:
            self.comboBox_ports.addItem("Aucun port trouvé - Branchez l'ESP32", "")
            self.label_status.setText("🔴 Aucun port série détecté")

    def connect_serial(self):
        port = self.comboBox_ports.currentData()
        if port and port != "":
            # Arrêter le thread précédent si existant
            if self.serial_thread and self.serial_thread.isRunning():
                self.serial_thread.stop()
            
            # Créer et démarrer le thread série (baudrate 9600)
            self.serial_thread = SerialReaderThread(port, 9600)
            self.serial_thread.nouvelle_trame.connect(self.process_serial_data)
            self.serial_thread.erreur.connect(self.show_error)
            self.serial_thread.connexion_change.connect(self.on_connection_change)
            self.serial_thread.start()
            
            self.start_time = time.time()
            self.label_status.setText(f"🟡 Connexion à {port} @ 9600 bauds...")
        else:
            QMessageBox.warning(self, "Attention", "Sélectionnez un port valide")

    def disconnect_serial(self):
        if self.serial_thread:
            self.serial_thread.stop()
            self.serial_thread = None

    def scan_ble(self):
        if self.ble_worker:
            self.pushButton_scan_ble.setEnabled(False)
            self.pushButton_scan_ble.setText("Scan...")
            threading.Thread(target=self.ble_worker.scan, daemon=True).start()
            QTimer.singleShot(6000, lambda: (
                self.pushButton_scan_ble.setEnabled(True),
                self.pushButton_scan_ble.setText("Scan")
            ))

    def on_ble_devices_found(self, devices):
        self.comboBox_ble_devices.clear()
        for d in devices:
            self.comboBox_ble_devices.addItem(f"{d['name']} ({d['addr']})", d['addr'])
        if self.comboBox_ble_devices.count() == 0:
            self.comboBox_ble_devices.addItem("Aucun ESP32 trouvé", "")
        self.label_status.setText(f"🔍 {len(devices)} appareil(s) BLE trouvé(s)")

    def connect_ble(self):
        if self.ble_worker:
            addr = self.comboBox_ble_devices.currentData()
            if addr and addr != "":
                threading.Thread(target=self.ble_worker.connect_ble, args=(addr,), daemon=True).start()
                self.label_status.setText("🟡 Connexion BLE...")

    def disconnect_ble(self):
        if self.ble_worker:
            self.ble_worker.disconnect()

    def on_connection_change(self, connected, message):
        if connected:
            self.label_status.setText(f"🟢 {message}")
            self.label_status.setStyleSheet("color: #a6e3a1; font-weight: bold; padding: 5px; font-size: 14px;")

            if "Série" in message:
                self.pushButton_connect_serial.setEnabled(False)
                self.pushButton_disconnect_serial.setEnabled(True)
                self.start_time = time.time()
            elif "BLE" in message:
                self.pushButton_connect_ble.setEnabled(False)
                self.pushButton_disconnect_ble.setEnabled(True)
        else:
            if "Série" in message:
                self.pushButton_connect_serial.setEnabled(True)
                self.pushButton_disconnect_serial.setEnabled(False)
            elif "BLE" in message:
                self.pushButton_connect_ble.setEnabled(True)
                self.pushButton_disconnect_ble.setEnabled(False)

            self.label_status.setText(f"🔴 {message}")
            self.label_status.setStyleSheet("color: #f38ba8; font-weight: bold; padding: 5px; font-size: 14px;")

        self.statusbar.showMessage(message)

    def send_cmd(self, cmd):
        """Envoyer commande sur tous les canaux"""
        print(f"[CMD] Envoi: {cmd}")
        if self.serial_thread and self.serial_thread._running:
            # Le thread série utilise 'with', on ne peut pas envoyer directement
            # Il faudrait un mécanisme d'envoi séparé si nécessaire
            pass
        if self.ble_worker:
            self.ble_worker.send(cmd)

    # ============================================================
    # COMMANDES
    # ============================================================
    def tare(self):
        """Fonction de tare avec feedback visuel"""
        self.send_cmd("TARE")
        self.label_status.setText("🟡 Tare en cours...")
        
        # Animation visuelle du bouton
        original_style = self.pushButton_tare.styleSheet()
        self.pushButton_tare.setStyleSheet(
            "background-color: #94e2d5; color: #1e1e2e; font-size: 18px; min-height: 50px; border: 2px solid #a6e3a1;"
        )
        QTimer.singleShot(500, lambda: self.pushButton_tare.setStyleSheet(original_style))
        QTimer.singleShot(1000, lambda: self.label_status.setText("🟢 Prêt"))

    def calibrate(self):
        poids = self.doubleSpinBox_cal.value()
        if poids > 0:
            self.send_cmd(f"CAL {poids}")
            self.label_status.setText(f"🟡 Calibration {poids}N envoyée")

    def set_factor(self):
        factor = self.doubleSpinBox_factor.value()
        if factor > 0:
            self.send_cmd(f"FACTOR {factor}")
            self.label_status.setText(f"🟡 Facteur {factor} appliqué")

    def reset_stats(self):
        self.stats = {
            'max': float('-inf'), 
            'min': float('inf'), 
            'sum': 0.0, 
            'count': 0,
            'last_temps': 0.0,
            'last_effort': 0.0
        }
        self.label_max.setText("Max: 0.00 N")
        self.label_min.setText("Min: 0.00 N")
        self.label_avg.setText("Moy: 0.00 N")
        self.label_count.setText("Mesures: 0")

    # ============================================================
    # TRAITEMENT DES DONNÉES
    # ============================================================
    def process_serial_data(self, temps, effort):
        """
        Traite les données reçues du port série
        """
        print(f"[DATA SÉRIE] Temps: {temps:.4f}s, Effort: {effort:.4f}N")
        
        if self.start_time is None:
            self.start_time = time.time()
        
        # Utiliser le temps reçu comme temps relatif
        temps_relatif = temps
        
        # Mettre à jour l'affichage
        self.update_display(effort, temps_relatif)

    def process_ble_data(self, data):
        """
        Traite les données reçues du BLE (format texte)
        """
        print(f"[DATA BLE] Texte reçu: '{data}'")
        
        data = data.strip()
        if not data:
            return
        
        # Essayer le format temps;effort
        if ';' in data:
            try:
                parts = data.split(';')
                if len(parts) >= 2:
                    temps = float(parts[0].strip().replace(',', '.'))
                    effort = float(parts[1].strip().replace(',', '.'))
                    
                    if self.start_time is None:
                        self.start_time = time.time()
                    
                    self.update_display(effort, temps)
                    return
            except (ValueError, IndexError) as e:
                print(f"[BLE] Erreur parsing: {e}")
        
        # Essayer le format legacy "Poids: XX.XX"
        elif "Poids:" in data:
            try:
                parts = data.split(":")
                val_str = parts[1].strip().split()[0]
                effort = float(val_str.replace(',', '.'))
                
                if self.start_time is None:
                    self.start_time = time.time()
                
                temps_relatif = time.time() - self.start_time
                self.update_display(effort, temps_relatif)
            except (ValueError, IndexError) as e:
                print(f"[BLE] Erreur parsing legacy: {e}")

    # ============================================================
    # MISE À JOUR DE L'AFFICHAGE
    # ============================================================
    def update_display(self, effort, temps_relatif):
        """
        Met à jour le LCD, les statistiques, le graphique et le CSV
        """
        # LCD - Afficher l'effort
        self.lcd_poids.display(f"{effort:.3f}")
        
        # Status
        self.label_status.setText(f"🟢 {effort:.3f} N")
        
        # Mise à jour des statistiques
        self.stats['max'] = max(self.stats['max'], effort)
        self.stats['min'] = min(self.stats['min'], effort)
        self.stats['sum'] += effort
        self.stats['count'] += 1
        avg = self.stats['sum'] / self.stats['count'] if self.stats['count'] > 0 else 0
        
        self.stats['last_temps'] = temps_relatif
        self.stats['last_effort'] = effort
        
        # Mettre à jour les labels
        self.label_max.setText(f"Max: {self.stats['max']:.3f} N")
        self.label_min.setText(f"Min: {self.stats['min']:.3f} N")
        self.label_avg.setText(f"Moy: {avg:.3f} N")
        self.label_count.setText(f"Mesures: {self.stats['count']}")
        
        # Ajouter aux données pour le graphique
        timestamp = datetime.now()
        self.temps_list.append(temps_relatif)
        self.effort_list.append(effort)
        self.all_data.append((timestamp, temps_relatif, effort))
        
        # Sauvegarde CSV si activée
        if self.csv_active and self.csv_writer:
            try:
                self.csv_writer.writerow([
                    timestamp.isoformat(),
                    f"{temps_relatif:.4f}",
                    f"{effort:.4f}",
                    timestamp.strftime("%H:%M:%S.%f")[:-3]
                ])
            except Exception as e:
                print(f"[CSV] Erreur écriture: {e}")

    # ============================================================
    # GRAPHIQUE
    # ============================================================
    def update_graph(self):
        self.canvas.axes.clear()
        
        if len(self.effort_list) > 1:
            t = list(self.temps_list)
            e = list(self.effort_list)

            # Tracer l'effort
            self.canvas.axes.plot(t, e, color='#89b4fa', linewidth=1.5, alpha=0.9, label='Effort')
            self.canvas.axes.fill_between(t, e, alpha=0.15, color='#89b4fa')

            # Ligne de moyenne
            if self.stats['count'] > 0:
                avg = self.stats['sum'] / self.stats['count']
                self.canvas.axes.axhline(y=avg, color='#f38ba8', linestyle='--',
                                        alpha=0.7, linewidth=1.5,
                                        label=f'Moy: {avg:.3f}N')
                
                # Lignes max et min
                if self.stats['max'] != float('-inf'):
                    self.canvas.axes.axhline(y=self.stats['max'], color='#a6e3a1', 
                                           linestyle=':', alpha=0.5, linewidth=1,
                                           label=f'Max: {self.stats["max"]:.3f}N')
                if self.stats['min'] != float('inf'):
                    self.canvas.axes.axhline(y=self.stats['min'], color='#fab387', 
                                           linestyle=':', alpha=0.5, linewidth=1,
                                           label=f'Min: {self.stats["min"]:.3f}N')

            # Configuration du graphique
            self.canvas.axes.set_title(f'Effort en temps réel - {self.stats["count"]} mesures', 
                                      color='#cdd6f4', fontweight='bold', fontsize=12)
            self.canvas.axes.set_xlabel('Temps (s)', color='#cdd6f4', fontsize=10)
            self.canvas.axes.set_ylabel('Effort (N)', color='#cdd6f4', fontsize=10)
            self.canvas.axes.tick_params(colors='#cdd6f4', labelsize=9)
            self.canvas.axes.grid(True, alpha=0.2, color='#45475a', linestyle='--')
            
            # Légende
            self.canvas.axes.legend(loc='upper right', 
                                   facecolor='#313244',
                                   edgecolor='#45475a', 
                                   labelcolor='#cdd6f4',
                                   fontsize=9)
            
            self.canvas.fig.tight_layout()
        
        self.canvas.draw()

    # ============================================================
    # CSV
    # ============================================================
    def toggle_csv(self, enabled):
        if enabled:
            self.start_csv()
        else:
            self.stop_csv()

    def start_csv(self):
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        filename, _ = QFileDialog.getSaveFileName(
            self, "Créer fichier CSV", f"effort_{timestamp}.csv",
            "Fichiers CSV (*.csv);;Tous (*.*)"
        )
        if filename:
            try:
                self.csv_file = open(filename, 'w', newline='', encoding='utf-8')
                self.csv_writer = csv.writer(self.csv_file)
                self.csv_writer.writerow(['Timestamp', 'Temps (s)', 'Effort (N)', 'Heure'])
                self.csv_file.flush()
                self.csv_active = True
                self.label_csv_status.setText(f"✅ {os.path.basename(filename)}")
                self.label_csv_status.setStyleSheet("color: #a6e3a1; font-size: 13px;")
                self.statusbar.showMessage(f"CSV: {filename}")
            except Exception as e:
                QMessageBox.critical(self, "Erreur CSV", str(e))
                self.checkBox_csv.setChecked(False)
        else:
            self.checkBox_csv.setChecked(False)

    def stop_csv(self):
        self.csv_active = False
        if self.csv_file:
            self.csv_file.close()
            self.csv_file = None
            self.csv_writer = None
        self.label_csv_status.setText("CSV: Inactif")
        self.label_csv_status.setStyleSheet("color: #f38ba8; font-size: 13px;")

    def flush_csv(self):
        if self.csv_active and self.csv_file:
            try:
                self.csv_file.flush()
            except:
                pass

    def export_csv(self):
        if not self.all_data:
            QMessageBox.information(self, "Info", "Aucune donnée à exporter")
            return

        filename, _ = QFileDialog.getSaveFileName(
            self, "Exporter les données",
            f"effort_export_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv",
            "Fichiers CSV (*.csv);;Tous (*.*)"
        )
        if filename:
            try:
                with open(filename, 'w', newline='', encoding='utf-8') as f:
                    w = csv.writer(f)
                    w.writerow(['Timestamp', 'Temps (s)', 'Effort (N)', 'Heure'])
                    for ts, t, e in self.all_data:
                        w.writerow([ts.isoformat(), f"{t:.4f}", f"{e:.4f}", 
                                  ts.strftime("%H:%M:%S.%f")[:-3]])
                QMessageBox.information(self, "Export réussi",
                    f"✅ {len(self.all_data)} mesures exportées dans:\n{filename}")
            except Exception as e:
                QMessageBox.critical(self, "Erreur export", str(e))

    def clear_data(self):
        rep = QMessageBox.question(self, "Confirmer", "Effacer toutes les données ?",
                                   QMessageBox.Yes | QMessageBox.No)
        if rep == QMessageBox.Yes:
            self.all_data.clear()
            self.temps_list.clear()
            self.effort_list.clear()
            self.reset_stats()
            self.start_time = time.time()
            self.statusbar.showMessage("Données effacées")

    # ============================================================
    # DIVERS
    # ============================================================
    def show_error(self, message):
        self.statusbar.showMessage(f"ERREUR: {message}", 5000)
        print(f"[ERROR] {message}")

    def show_about(self):
        QMessageBox.about(self, "À propos",
            "<h2>Mesure d'Effort en Temps Réel</h2>"
            "<p><b>Port Série:</b> 9600 bauds - Format: temps;effort</p>"
            "<p><b>Bluetooth BLE:</b> Connexion sans fil</p>"
            "<p><b>Sauvegarde:</b> CSV automatique</p>"
            "<hr>"
            "<p>Matériel: ESP32 + Capteur de force</p>"
        )

    def closeEvent(self, event):
        self.stop_csv()
        if self.serial_thread:
            self.serial_thread.stop()
        if self.ble_worker:
            self.ble_worker.disconnect()
        event.accept()


# ============================================================
# MAIN
# ============================================================
def main():
    app = QApplication(sys.argv)
    app.setStyle('Fusion')
    app.setApplicationName("Mesure d'Effort")

    window = BalanceApp()
    window.show()

    sys.exit(app.exec_())


if __name__ == "__main__":
    main()