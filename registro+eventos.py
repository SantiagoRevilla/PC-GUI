import sys
import time
import socket
import datetime
import numpy as np
from collections import deque
from PyQt5.QtWidgets import (QApplication, QMainWindow, QVBoxLayout, QWidget, 
                             QLabel, QHBoxLayout, QFrame, QLineEdit, QPushButton, QMessageBox)
from PyQt5.QtCore import QThread, pyqtSignal, Qt
from PyQt5.QtGui import QFont
from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg as FigureCanvas
from matplotlib.figure import Figure

# --- CONFIGURACIÓN UDP ---
UDP_IP = "0.0.0.0"
UDP_PORT = 3333

class DataWorker(QThread):
    sig_ecg = pyqtSignal(float)
    sig_stats = pyqtSignal(int, int)

    def __init__(self):
        super().__init__()
        self.running = True
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.sock.bind((UDP_IP, UDP_PORT))
        except Exception as e:
            print(f"Error binding socket: {e}")
        self.sock.settimeout(0.5)

    def run(self):
        while self.running:
            try:
                data, addr = self.sock.recvfrom(2048)
                text = data.decode('utf-8')
                lines = text.splitlines()

                for line in lines:
                    line = line.strip()
                    if not line: continue

                    if line.startswith("S:"):
                        try:
                            partes = line[2:].split(',')
                            if len(partes) == 2:
                                self.sig_stats.emit(int(partes[0]), int(partes[1]))
                        except ValueError: pass
                    else:
                        try:
                            self.sig_ecg.emit(float(line))
                        except ValueError: pass
            except socket.timeout:
                continue
            except Exception as e:
                print(f"Error receiving: {e}")

    def stop(self):
        self.running = False
        self.wait()
        self.sock.close()

class MonitorVital(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Monitor Signos Vitales - LOGS & HISTORIAL")
        self.setGeometry(100, 100, 1000, 600)
        self.setStyleSheet("background-color: #121212; color: #00FF00;")
        
        self.data_buffer = deque([0]*200, maxlen=200)
        self.is_recording = False
        self.file_handle = None
        self.alarma_activa = False # Para no saturar el log de alarmas
        
        self.initUI()
        self.init_worker()

    def initUI(self):
        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        main_layout = QHBoxLayout(central_widget)

        # Gráfica
        plot_layout = QVBoxLayout()
        self.fig = Figure(figsize=(5, 4), dpi=100, facecolor='#121212')
        self.ax = self.fig.add_subplot(111)
        self.ax.set_facecolor('#000000')
        self.ax.set_title('ECG', color='white')
        self.ax.tick_params(axis='x', colors='white')
        self.ax.tick_params(axis='y', colors='white')
        self.ax.set_ylim(0, 4096)
        self.ax.grid(True, color='#333333', linestyle='--')
        self.line, = self.ax.plot([], [], color='#00FF00', linewidth=1.5)
        self.canvas = FigureCanvas(self.fig)
        plot_layout.addWidget(self.canvas)

        # Panel Derecho
        stats_layout = QVBoxLayout()
        self.lbl_hr = self.crear_panel_dato("FRECUENCIA CARDIACA", "BPM", "#FF3333")
        stats_layout.addWidget(self.lbl_hr)
        self.lbl_spo2 = self.crear_panel_dato("SATURACIÓN O2", "%", "#3399FF")
        stats_layout.addWidget(self.lbl_spo2)

        # Controles
        control_frame = QFrame()
        control_frame.setStyleSheet("border: 1px solid #333333; border-radius: 5px; margin: 10px; padding: 5px;")
        control_layout = QVBoxLayout(control_frame)

        self.input_nombre = QLineEdit()
        self.input_nombre.setPlaceholderText("Nombre del Paciente")
        self.input_nombre.setStyleSheet("background-color: #333333; color: white; padding: 5px; border: none;")
        control_layout.addWidget(self.input_nombre)

        self.input_edad = QLineEdit()
        self.input_edad.setPlaceholderText("Edad")
        self.input_edad.setStyleSheet("background-color: #333333; color: white; padding: 5px; border: none; margin-top: 5px;")
        control_layout.addWidget(self.input_edad)

        self.btn_record = QPushButton("INICIAR REGISTRO")
        self.btn_record.setStyleSheet("background-color: #004400; color: white; font-weight: bold; padding: 10px; border-radius: 5px;")
        self.btn_record.clicked.connect(self.toggle_recording)
        control_layout.addWidget(self.btn_record)
        
        self.lbl_estado = QLabel("Sistema Listo")
        self.lbl_estado.setStyleSheet("color: gray; font-size: 10px; border: none;")
        self.lbl_estado.setAlignment(Qt.AlignCenter)
        control_layout.addWidget(self.lbl_estado)

        stats_layout.addWidget(control_frame)
        stats_layout.addStretch()

        main_layout.addLayout(plot_layout, stretch=3)
        main_layout.addLayout(stats_layout, stretch=1)

    def crear_panel_dato(self, titulo, unidad, color_borde):
        frame = QFrame()
        frame.setStyleSheet(f"border: 2px solid {color_borde}; border-radius: 10px; background-color: #1E1E1E; margin: 10px;")
        layout = QVBoxLayout(frame)
        lbl_titulo = QLabel(titulo)
        lbl_titulo.setAlignment(Qt.AlignCenter)
        lbl_titulo.setStyleSheet("color: white; font-size: 14px; border: none;")
        lbl_valor = QLabel("--")
        lbl_valor.setAlignment(Qt.AlignCenter)
        lbl_valor.setFont(QFont("Arial", 50, QFont.Bold))
        lbl_valor.setStyleSheet(f"color: {color_borde}; border: none;")
        lbl_unidad = QLabel(unidad)
        lbl_unidad.setAlignment(Qt.AlignCenter)
        lbl_unidad.setStyleSheet("color: gray; font-size: 12px; border: none;")
        layout.addWidget(lbl_titulo)
        layout.addWidget(lbl_valor)
        layout.addWidget(lbl_unidad)
        frame.valor_label = lbl_valor
        return frame

    def init_worker(self):
        self.worker = DataWorker()
        self.worker.sig_ecg.connect(self.actualizar_grafica)
        self.worker.sig_stats.connect(self.actualizar_stats)
        self.worker.start()

    # --- NUEVO: FUNCIÓN UNIVERSAL DE LOGS ---
    def escribir_log(self, tipo, mensaje, valor=""):
        """Escribe una línea en el archivo si estamos grabando"""
        if self.is_recording and self.file_handle:
            t_actual = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
            linea = f"{t_actual},{tipo},{mensaje},{valor}\n"
            self.file_handle.write(linea)

    def toggle_recording(self):
        if not self.is_recording:
            # INICIO
            nombre = self.input_nombre.text().strip() or "Anonimo"
            edad = self.input_edad.text().strip() or "?"
            timestamp_str = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
            filename = f"Historial_{nombre}_{timestamp_str}.txt"
            
            try:
                self.file_handle = open(filename, "w")
                # Cabecera de Historial
                self.file_handle.write("=== LOG DE SISTEMA DE TELEMETRIA ===\n")
                self.file_handle.write(f"PACIENTE: {nombre} | EDAD: {edad}\n")
                self.file_handle.write(f"INICIO SESION: {datetime.datetime.now()}\n")
                self.file_handle.write("====================================\n")
                self.file_handle.write("TIMESTAMP,TIPO,DETALLE,VALOR\n")
                
                self.is_recording = True
                self.escribir_log("SISTEMA", "INICIO DE GRABACION", "") # Log de evento
                
                self.btn_record.setText("DETENER REGISTRO")
                self.btn_record.setStyleSheet("background-color: #AA0000; color: white; padding: 10px; border-radius: 5px;")
                self.lbl_estado.setText(f"Grabando: {filename}")
                self.lbl_estado.setStyleSheet("color: #FF5555; font-weight: bold;")
                self.input_nombre.setEnabled(False)
                self.input_edad.setEnabled(False)
                
            except Exception as e:
                self.lbl_estado.setText(f"Error Disco: {e}")
        else:
            # FIN
            self.escribir_log("SISTEMA", "FIN DE GRABACION", "") # Log de evento
            if self.file_handle:
                self.file_handle.close()
                self.file_handle = None
            
            self.is_recording = False
            self.btn_record.setText("INICIAR REGISTRO")
            self.btn_record.setStyleSheet("background-color: #004400; color: white; padding: 10px; border-radius: 5px;")
            self.lbl_estado.setText("Historial Guardado")
            self.lbl_estado.setStyleSheet("color: #00FF00;")
            self.input_nombre.setEnabled(True)
            self.input_edad.setEnabled(True)

    def actualizar_grafica(self, valor):
        self.data_buffer.append(valor)
        self.line.set_data(np.arange(len(self.data_buffer)), list(self.data_buffer))
        self.ax.set_xlim(0, len(self.data_buffer))
        self.canvas.draw_idle()
        
        # Log de Datos Crudos
        self.escribir_log("ECG", "Muestra", f"{valor:.2f}")

    def actualizar_stats(self, spo2, hr):
        self.lbl_spo2.valor_label.setText(str(spo2))
        self.lbl_hr.valor_label.setText(str(hr))
        
        # Lógica de Alarmas y Logs de Eventos
        if spo2 < 90 and spo2 > 0:
            self.lbl_spo2.setStyleSheet("border: 2px solid red; background-color: #330000; margin: 10px;")
            if not self.alarma_activa:
                self.escribir_log("ALARMA", "HIPOXIA DETECTADA", str(spo2))
                self.alarma_activa = True
        else:
            self.lbl_spo2.setStyleSheet("border: 2px solid #3399FF; background-color: #1E1E1E; margin: 10px;")
            if self.alarma_activa:
                 self.escribir_log("INFO", "NIVEL O2 NORMALIZADO", str(spo2))
                 self.alarma_activa = False
            
        # Log de Datos Vitales
        self.escribir_log("VITALES", "SPO2/HR", f"{spo2}/{hr}")

    def closeEvent(self, event):
        if self.file_handle:
            self.escribir_log("SISTEMA", "CIERRE DE APLICACION", "")
            self.file_handle.close()
        self.worker.stop()
        event.accept()

if __name__ == "__main__":
    app = QApplication(sys.argv)
    window = MonitorVital()
    window.show()
    sys.exit(app.exec_())