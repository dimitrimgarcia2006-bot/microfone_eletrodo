/*
 * Envio dos quadros para o visualizador no PC (tools/visualizador.py)
 *
 * Usa a USB nativa (USB Serial/JTAG) do ESP32-S3. Os pacotes só são
 * enviados enquanto o PC manda o byte 0x02 pelo menos a cada 2 s; assim o
 * idf.py monitor não é inundado de dados binários.
 *
 * Pacote (little-endian), um por quadro:
 *   cabeçalho (28 bytes)
 *     uint8_t  magic[4]      A5 5A 'F' 'T'
 *     uint16_t versao        1
 *     uint16_t n_amostras    N_FFT
 *     uint16_t n_bins        N_FFT / 2 + 1
 *     uint16_t fs            SAMPLE_RATE_HZ
 *     uint32_t seq           número do quadro
 *     uint8_t  resultado     frame_result_t
 *     uint8_t  reservado
 *     int16_t  n             tecla do piano (só em FRAME_NOTE)
 *     float    f0            Hz (0 em FRAME_SILENCE)
 *     float    cents         (só em FRAME_NOTE)
 *   uint16_t amostras[N_FFT]       quadro bruto do ADC (contagens)
 *   float    mag[N_FFT / 2 + 1]    |X[k]| da FFT
 *   uint16_t soma                  soma dos bytes depois do magic (mod 65536)
 */
#pragma once

#include <stdint.h>
#include "config.h"
#include "note.h"
#include "pitch.h"

/* Instala o driver da USB Serial/JTAG (o printf passa a usar o driver) */
void viz_init(void);

/* Envia um quadro, se o visualizador estiver conectado. Não bloqueia:
 * se a fila de envio estiver cheia, o quadro é descartado. */
void viz_send_frame(uint32_t seq, const uint16_t frame[N_FFT], const float *mag,
                    frame_result_t res, const note_t *note);
