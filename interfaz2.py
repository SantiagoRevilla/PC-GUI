import sys
import time
import requests
import numpy as np
from collections import deque

# Librerías de Interfaz Gráfica (PyQt5)
from PyQt5.QtWidgets import QApplication, QMainWindow, QVBoxLayout, QWidget, QLabel, QHBoxLayout, QFrame
from PyQt5.QtCore import QThread, pyqtSignal, Qt
from PyQt5.QtGui import QFont

# Librerías de Gráfica (Matplotlib integrada en Qt)
from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg as FigureCanvas
from matplotlib.figure import Figure

# --- CONFIGURACIÓN ---
# ¡IMPORTANTE! Reemplaza esto con la IP que imprime tu ESP32 en el Monitor Serial
ESP_IP = "192.168.1.10"  
URL_ECG = f"http://{ESP_IP}/readECG"
URL_STATS = f"http://{ESP_IP}/readStats"

class DataWorker(QThread):
    """
    Este hilo se encarga de descargar los datos del ESP32 en segundo plano
    para no congelar la ventana principal.
    """
    sig_ecg = pyqtSignal(float)
    sig_stats = pyqtSignal(int, int)

    def __init__(self):
        super().__init__()
        self.running = True

    def run(self):
        contador_stats = 0
        while self.running:
            try:
                # 1. Leer ECG (Intentamos leer lo más rápido posible)
                # Nota: HTTP tiene latencia, para ECG profesional se usa TCP/UDP Raw,
                # pero esto funcionará con tu código actual de ESP32.
                r_ecg = requests.get(URL_ECG, timeout=0.5)
                if r_ecg.status_code == 200:
                    try:
                        valor = float(r_ecg.text)
                        self.sig_ecg.emit(valor)
                    except ValueError:
                        pass

                # 2. Leer HR y SpO2 (Solo cada 20 iteraciones aprox, es decir, cada ~1 segundo)
                if contador_stats > 20:
                    r_stats = requests.get(URL_STATS, timeout=0.5)
                    if r_stats.status_code == 200:
                        partes = r_stats.text.split(',')
                        if len(partes) == 2:
                            spo2 = int(partes[0])
                            hr = int(partes[1])
                            self.sig_stats.emit(spo2, hr)
                    contador_stats = 0
                
                contador_stats += 1
                
                # Pequeña pausa para no saturar la red excesivamente
                time.sleep(0.04) 

            except requests.exceptions.RequestException:
                # Si falla la conexión, ignoramos y reintentamos
                pass

    def stop(self):
        self.running = False
        self.wait()

class MonitorVital(QMainWindow):
    def __init__(self):
        super().__init__()
        
        # Configuración de la ventana
        self.setWindowTitle("Monitor de Signos Vitales - UCB")
        self.setGeometry(100, 100, 1000, 600)
        self.setStyleSheet("background-color: #121212; color: #00FF00;") # Estilo oscuro tipo hospital

        # Buffer de datos para el ECG (últimos 200 puntos)
        self.data_buffer = deque([0]*200, maxlen=200)

        self.initUI()
        self.init_worker()

    def initUI(self):
        # Widget Central
        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        
        # Layout Principal Horizontal (Izquierda: Gráfico, Derecha: Números)
        main_layout = QHBoxLayout(central_widget)

        # --- SECCIÓN IZQUIERDA: GRÁFICO ECG ---
        plot_layout = QVBoxLayout()
        
        # Crear Figura Matplotlib
        self.fig = Figure(figsize=(5, 4), dpi=100, facecolor='#121212')
        self.ax = self.fig.add_subplot(111)
        self.ax.set_facecolor('#000000') # Fondo negro de la gráfica
        
        # Configurar ejes
        self.ax.set_title('ECG Lead I', color='white')
        self.ax.tick_params(axis='x', colors='white')
        self.ax.tick_params(axis='y', colors='white')
        self.ax.set_ylim(0, 4096) # Rango del ADC de 12 bits (ajustar si filtra)
        self.ax.grid(True, color='#333333', linestyle='--')

        # Línea del gráfico
        self.line, = self.ax.plot([], [], color='#00FF00', linewidth=1.5)
        
        # Canvas
        self.canvas = FigureCanvas(self.fig)
        plot_layout.addWidget(self.canvas)
        
        # --- SECCIÓN DERECHA: PANELES DE DATOS ---
        stats_layout = QVBoxLayout()
        
        # Panel HR
        self.lbl_hr = self.crear_panel_dato("FRECUENCIA CARDIACA", "BPM", "#FF3333")
        stats_layout.addWidget(self.lbl_hr)
        
        # Panel SpO2
        self.lbl_spo2 = self.crear_panel_dato("SATURACIÓN O2", "%", "#3399FF")
        stats_layout.addWidget(self.lbl_spo2)
        
        stats_layout.addStretch()

        # Añadir sub-layouts al principal
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
        
        # Guardamos referencia al label del valor en el objeto frame para acceder luego
        frame.valor_label = lbl_valor 
        return frame

    def init_worker(self):
        # Iniciar el hilo de comunicación
        self.worker = DataWorker()
        self.worker.sig_ecg.connect(self.actualizar_grafica)
        self.worker.sig_stats.connect(self.actualizar_stats)
        self.worker.start()

    def actualizar_grafica(self, valor):
        # Añadir nuevo dato al buffer
        self.data_buffer.append(valor)
        
        # Actualizar datos de la línea
        self.line.set_data(np.arange(len(self.data_buffer)), list(self.data_buffer))
        self.ax.set_xlim(0, len(self.data_buffer))
        
        # Redibujar canvas (usando draw_idle para optimizar)
        self.canvas.draw_idle()

    def actualizar_stats(self, spo2, hr):
        # Actualizar textos
        self.lbl_spo2.valor_label.setText(str(spo2))
        self.lbl_hr.valor_label.setText(str(hr))

        # Alerta visual simple (cambiar a rojo si está mal)
        if spo2 < 90 and spo2 > 0:
            self.lbl_spo2.setStyleSheet("border: 2px solid red; background-color: #330000; margin: 10px;")
        else:
            self.lbl_spo2.setStyleSheet("border: 2px solid #3399FF; background-color: #1E1E1E; margin: 10px;")

    def closeEvent(self, event):
        # Detener el hilo al cerrar la ventana
        self.worker.stop()
        event.accept()

if __name__ == "__main__":
    app = QApplication(sys.argv)
    window = MonitorVital()
    window.show()
    sys.exit(app.exec_())