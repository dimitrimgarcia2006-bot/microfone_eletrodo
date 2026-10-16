"""
Visualizador em tempo real do afinador (ESP32-S3)

Recebe pela USB nativa os pacotes enviados por main/viz.c e mostra:
  - o quadro de N_FFT amostras do microfone em função do tempo
  - o espectro calculado pela FFT do ESP, com o f0 encontrado marcado

Uso, na raiz do repositório (feche o idf.py monitor antes, a porta só
pode ser aberta por um programa de cada vez):
  .venv\\Scripts\\python tools\\visualizador.py
  .venv\\Scripts\\python tools\\visualizador.py --port COM7

O formato dos pacotes está documentado em main/viz.h.
"""

import argparse
import struct
import sys
import threading
import time
from dataclasses import dataclass

import numpy as np
import pyqtgraph as pg
import serial
import serial.tools.list_ports
from pyqtgraph.Qt import QtCore

# ---------------- Protocolo (igual a main/viz.h) ----------------
MAGIC = b"\xA5\x5AFT"
HEADER = struct.Struct("<4sHHHHIBBhff")     # 28 bytes
VERSION = 1
KEEPALIVE = b"\x02"
KEEPALIVE_S = 0.5                           # o firmware para de enviar após 2 s sem keepalive

FRAME_SILENCE, FRAME_OUT_OF_RANGE, FRAME_NOTE = 0, 1, 2

# ---------------- Parâmetros do firmware usados nos gráficos ----------------
MIN_AMP_CNT = 4.0                           # pitch.c: limiar de silêncio (contagens)
HALF_SEMITONE = 2 ** (1 / 24)
F0_MIN = 130.81 / HALF_SEMITONE             # config.h: C3 menos meio semitom
F0_MAX = 523.25 * HALF_SEMITONE             # config.h: C5 mais meio semitom

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
SOLFEGE = ["Do", "Do#", "Re", "Re#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si"]

ESPRESSIF_VID = 0x303A                      # USB Serial/JTAG do ESP32-S3


@dataclass
class Frame:
    seq: int
    fs: int
    result: int
    note_n: int
    f0: float
    cents: float
    samples: np.ndarray                     # quadro bruto do ADC (contagens)
    mag: np.ndarray                         # |X[k]|, k = 0..N/2


# ---------------- Leitura da serial ----------------
class Reader(threading.Thread):
    """Lê a porta, separa o texto dos pacotes e guarda o quadro mais recente"""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self.status = "conectando..."
        self.n_ok = self.n_lost = self.n_bad = 0
        self._lock = threading.Lock()
        self._latest = None
        self._last_seq = None
        self._buf = bytearray()
        self._text = bytearray()
        self._stop = threading.Event()

    def stop(self):
        self._stop.set()

    def take(self):
        """Devolve o quadro chegado desde a última chamada, ou None"""
        with self._lock:
            frame, self._latest = self._latest, None
        return frame

    def run(self):
        # Reconecta sozinho: ao gravar ou reiniciar o ESP, a porta some por um instante
        while not self._stop.is_set():
            try:
                with self._open() as ser:
                    self.status = f"conectado em {self.port}"
                    self._last_seq = None
                    self._loop(ser)
            except serial.SerialException:
                self.status = f"sem conexão com {self.port}, tentando de novo..."
                time.sleep(1.0)

    def _open(self):
        ser = serial.Serial()
        ser.port = self.port
        ser.timeout = 0.05
        # DTR/RTS em 0: certas combinações reiniciam o ESP32-S3 pela USB
        ser.dtr = False
        ser.rts = False
        ser.open()
        return ser

    def _loop(self, ser):
        last_keepalive = 0.0
        while not self._stop.is_set():
            now = time.monotonic()
            if now - last_keepalive >= KEEPALIVE_S:
                ser.write(KEEPALIVE)
                last_keepalive = now
            data = ser.read(ser.in_waiting or 1)
            if data:
                self._buf += data
                self._parse()

    def _parse(self):
        buf = self._buf
        while True:
            i = buf.find(MAGIC)
            if i < 0:
                # Guarda o fim do buffer: o magic pode ter chegado pela metade
                keep = len(MAGIC) - 1
                self._feed_text(buf[:-keep])
                del buf[:-keep]
                return
            if i > 0:
                self._feed_text(buf[:i])
                del buf[:i]
            if len(buf) < HEADER.size:
                return

            hdr = HEADER.unpack_from(buf)
            version, n_samples, n_bins = hdr[1], hdr[2], hdr[3]
            if version != VERSION or n_samples == 0 or n_bins != n_samples // 2 + 1:
                del buf[:1]                 # não era um pacote; procura o próximo magic
                continue

            size = HEADER.size + 2 * n_samples + 4 * n_bins + 2
            if len(buf) < size:
                return
            pkt = bytes(buf[:size])
            del buf[:size]
            self._decode(hdr, pkt)

    def _decode(self, hdr, pkt):
        _, _, n_samples, n_bins, fs, seq, result, _, note_n, f0, cents = hdr

        body = np.frombuffer(pkt, np.uint8, len(pkt) - len(MAGIC) - 2, offset=len(MAGIC))
        (checksum,) = struct.unpack_from("<H", pkt, len(pkt) - 2)
        if int(body.sum()) & 0xFFFF != checksum:
            self.n_bad += 1
            return

        samples = np.frombuffer(pkt, "<u2", n_samples, offset=HEADER.size)
        mag = np.frombuffer(pkt, "<f4", n_bins, offset=HEADER.size + 2 * n_samples)

        if self._last_seq is not None and seq > self._last_seq + 1:
            self.n_lost += seq - self._last_seq - 1
        self._last_seq = seq
        self.n_ok += 1

        with self._lock:
            self._latest = Frame(seq, fs, result, note_n, f0, cents, samples, mag)

    def _feed_text(self, data):
        """Imprime no terminal o texto do firmware (printf, logs)"""
        self._text += data
        *lines, rest = self._text.split(b"\n")
        for line in lines:
            print(line.decode("utf-8", "replace").rstrip("\r"), flush=True)
        self._text = bytearray(rest[-1000:])


# ---------------- Janela ----------------
class Window:
    def __init__(self, reader):
        self.reader = reader
        self.n = None

        self.win = pg.GraphicsLayoutWidget(title="Afinador ESP32-S3 - visualizador")
        self.win.resize(1100, 800)

        self.note_label = self.win.addLabel("aguardando dados...", row=0, col=0, size="20pt")

        # Sinal no tempo
        self.p_time = self.win.addPlot(row=1, col=0)
        self.p_time.setLabel("bottom", "tempo (ms)")
        self.p_time.setLabel("left", "amplitude (contagens)")
        self.p_time.showGrid(x=True, y=True, alpha=0.3)
        self.p_time.addLegend(offset=(10, 10))
        self.c_raw = self.p_time.plot(pen=pg.mkPen("#4a90d9", width=1), name="sinal sem DC")
        self.c_win = self.p_time.plot(pen=pg.mkPen("#e8a33d", width=1), name="sinal × Hann (entrada da FFT)")

        # Espectro
        self.p_freq = self.win.addPlot(row=2, col=0)
        self.p_freq.setTitle("Espectro (FFT do ESP); faixa C3..C5 em verde")
        self.p_freq.setLabel("bottom", "frequência (Hz)")
        self.p_freq.setLabel("left", "amplitude (dB re 1 contagem)")
        self.p_freq.showGrid(x=True, y=True, alpha=0.3)
        self.p_freq.addItem(pg.LinearRegionItem((F0_MIN, F0_MAX), movable=False,
                                                brush=pg.mkBrush(80, 180, 80, 40),
                                                pen=pg.mkPen(None)))
        self.p_freq.addItem(pg.InfiniteLine(20 * np.log10(MIN_AMP_CNT), angle=0,
                                            pen=pg.mkPen("#888", style=QtCore.Qt.DotLine),
                                            label="limiar de silêncio",
                                            labelOpts={"position": 0.95, "color": "#888"}))
        self.c_spec = self.p_freq.plot(pen=pg.mkPen("#4a90d9", width=1))
        self.f0_line = pg.InfiniteLine(angle=90, pen=pg.mkPen("#e44", style=QtCore.Qt.DashLine),
                                       label="f0", labelOpts={"position": 0.95, "color": "#e44"})
        self.p_freq.addItem(self.f0_line)
        self.f0_line.hide()
        self.p_freq.setXRange(0, 2500)
        self.p_freq.setYRange(-30, 70)

        self.status = self.win.addLabel("", row=3, col=0, size="9pt")

        self.timer = QtCore.QTimer()
        self.timer.timeout.connect(self.update)
        self.timer.start(30)

    def update(self):
        r = self.reader
        self.status.setText(f"{r.status} · quadros: {r.n_ok} · perdidos: {r.n_lost} · com erro: {r.n_bad}")

        f = r.take()
        if f is None:
            return

        n = len(f.samples)
        if n != self.n:
            self.n = n
            self.t_ms = 1000.0 * np.arange(n) / f.fs
            self.freqs = np.arange(n // 2 + 1) * f.fs / n
            self.hann = np.hanning(n)       # mesma janela de dsps_wind_hann_f32

        # Tempo: o mesmo pré-processamento do pitch.c (tira a média, aplica a Hann)
        x = f.samples.astype(np.float32)
        dc = x.mean()
        x -= dc
        self.c_raw.setData(self.t_ms, x)
        self.c_win.setData(self.t_ms, x * self.hann)
        self.p_time.setTitle(f"Microfone: quadro {f.seq}, {n} amostras ({self.t_ms[-1]:.0f} ms) · "
                             f"DC = {dc:.0f} · pico a pico = {int(np.ptp(f.samples))} contagens")

        # Frequência: |X| -> amplitude da senoide (A = 4|X|/N, ver pitch.c) -> dB
        amp = 4.0 * f.mag / n
        self.c_spec.setData(self.freqs, 20 * np.log10(np.maximum(amp, 1e-3)))

        if f.result == FRAME_NOTE:
            idx = (f.note_n + 8) % 12
            octave = (f.note_n + 8) // 12
            c = abs(f.cents)
            color = "#3c3" if c < 5 else "#ec3" if c < 15 else "#e44"
            self.note_label.setText(f"{NOTE_NAMES[idx]}{octave} ({SOLFEGE[idx]}) · "
                                    f"f0 = {f.f0:.1f} Hz · {f.cents:+.0f} cents", color=color)
            self.f0_line.setPos(f.f0)
            self.f0_line.show()
        elif f.result == FRAME_OUT_OF_RANGE:
            self.note_label.setText(f"fora da faixa (pico em {f.f0:.1f} Hz)", color="#aaa")
            self.f0_line.setPos(f.f0)
            self.f0_line.show()
        else:
            self.note_label.setText("silêncio", color="#aaa")
            self.f0_line.hide()


def find_port():
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if p.vid == ESPRESSIF_VID:
            return p.device
    names = ", ".join(p.device for p in ports) or "nenhuma"
    sys.exit(f"Não achei a USB nativa do ESP32-S3 (VID 303A). Portas disponíveis: {names}. Use --port.")


def main():
    ap = argparse.ArgumentParser(description="Visualizador em tempo real do afinador ESP32-S3")
    ap.add_argument("--port", help="porta serial (padrão: procura a USB nativa do ESP32-S3)")
    args = ap.parse_args()

    port = args.port or find_port()
    print(f"Usando {port}. Feche a janela para sair.")

    app = pg.mkQApp("Afinador")
    reader = Reader(port)
    reader.start()
    window = Window(reader)
    window.win.show()
    app.exec()
    reader.stop()
    reader.join(timeout=1.0)


if __name__ == "__main__":
    main()
