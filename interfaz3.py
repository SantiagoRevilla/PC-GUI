import sys
import time
import socket
import numpy as np
from collections import deque
from PyQt5.QtWidgets import QApplication, QMainWindow, QVBoxLayout, QWidget, QLabel, QHBoxLayout, QFrame
from PyQt5.QtCore import QThread, pyqtSignal, Qt
from PyQt5.QtGui import QFont
from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg as FigureCanvas
from matplotlib.figure import Figure

# --- CONFIGURACIÓN UDP ---
UDP_IP = "0.0.0.0"  # Escuchar en todas las interfaces de red
UDP_PORT = 3333     # Debe coincidir con el puerto de tu ESP32

class DataWorker(QThread):
    sig_ecg = pyqtSignal(float)
    sig_stats = pyqtSignal(int, int)

    def __init__(self):
        super().__init__()
        self.running = True
        # Socket UDP
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((UDP_IP, UDP_PORT))
        self.sock.settimeout(0.5)  # para no bloquear indefinidamente

    def run(self):
        while self.running:
            try:
                data, addr = self.sock.recvfrom(2048)
                text = data.decode('utf-8')

            # 🔑 UDP puede traer muchas líneas juntas
                lines = text.splitlines()

                for line in lines:
                    line = line.strip()
                    if not line:
                        continue

                # 📌 SpO2 y HR
                    if line.startswith("S:"):
                        try:
                            partes = line[2:].split(',')
                            if len(partes) == 2:
                                spo2 = int(partes[0])
                                hr = int(partes[1])
                                self.sig_stats.emit(spo2, hr)
                        except ValueError:
                            pass

                # 📌 ECG
                    else:
                        try:
                            valor_ecg = float(line)
                            self.sig_ecg.emit(valor_ecg)
                        except ValueError:
                            pass

            except socket.timeout:
                continue


    def stop(self):
        self.running = False
        self.wait()
        self.sock.close()

# --- Resto del código de PyQt para interfaz y gráficas ---
class MonitorVital(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Monitor de Signos Vitales - UDP")
        self.setGeometry(100, 100, 1000, 600)
        self.setStyleSheet("background-color: #121212; color: #00FF00;")
        self.data_buffer = deque([0]*200, maxlen=200)
        self.initUI()
        self.init_worker()

    def initUI(self):
        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        main_layout = QHBoxLayout(central_widget)

        # Gráfica ECG
        plot_layout = QVBoxLayout()
        self.fig = Figure(figsize=(5, 4), dpi=100, facecolor='#121212')
        self.ax = self.fig.add_subplot(111)
        self.ax.set_facecolor('#000000')
        self.ax.set_title('ECG Lead I', color='white')
        self.ax.tick_params(axis='x', colors='white')
        self.ax.tick_params(axis='y', colors='white')
        self.ax.set_ylim(0, 4096)
        self.ax.grid(True, color='#333333', linestyle='--')
        self.line, = self.ax.plot([], [], color='#00FF00', linewidth=1.5)
        self.canvas = FigureCanvas(self.fig)
        plot_layout.addWidget(self.canvas)

        # Panel de estadísticas
        stats_layout = QVBoxLayout()
        self.lbl_hr = self.crear_panel_dato("FRECUENCIA CARDIACA", "BPM", "#FF3333")
        stats_layout.addWidget(self.lbl_hr)
        self.lbl_spo2 = self.crear_panel_dato("SATURACIÓN O2", "%", "#3399FF")
        stats_layout.addWidget(self.lbl_spo2)
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

    def actualizar_grafica(self, valor):
        self.data_buffer.append(valor)
        self.line.set_data(np.arange(len(self.data_buffer)), list(self.data_buffer))
        self.ax.set_xlim(0, len(self.data_buffer))
        self.canvas.draw_idle()

    def actualizar_stats(self, spo2, hr):
        self.lbl_spo2.valor_label.setText(str(spo2))
        self.lbl_hr.valor_label.setText(str(hr))
        if spo2 < 90 and spo2 > 0:
            self.lbl_spo2.setStyleSheet("border: 2px solid red; background-color: #330000; margin: 10px;")
        else:
            self.lbl_spo2.setStyleSheet("border: 2px solid #3399FF; background-color: #1E1E1E; margin: 10px;")

    def closeEvent(self, event):
        self.worker.stop()
        event.accept()

if __name__ == "__main__":
    app = QApplication(sys.argv)
    window = MonitorVital()
    window.show()
    sys.exit(app.exec_())