/*
 * Detector de frequência fundamental (f0) por FFT
 *
 * A cada quadro de N_FFT amostras:
 *   1. remove a média (nível DC de polarização)
 *   2. aplica a janela de Hann  w[n] = 0,5 - 0,5 cos(2 pi n / (N-1))
 *   3. calcula a FFT radix-2 da esp-dsp
 *   4. acha o pico dominante e testa os sub-harmônicos (kp/4, kp/3, kp/2)
 *      para chegar ao fundamental f0, dentro da faixa C3..C5 (2 oitavas)
 *   5. refina f0 por interpolação parabólica e converte em nota
 */
#pragma once

#include <stdint.h>
#include "config.h"
#include "note.h"

typedef enum {
    FRAME_SILENCE,
    FRAME_OUT_OF_RANGE,
    FRAME_NOTE,
} frame_result_t;

/* Calcula a janela de Hann e as tabelas da FFT */
void pitch_init(void);

/* Processa um quadro de N_FFT amostras do ADC.
 * FRAME_NOTE:         out preenchido por completo
 * FRAME_OUT_OF_RANGE: só out->f0 (frequência encontrada fora de C3..C5)
 * FRAME_SILENCE:      out não é alterado */
frame_result_t pitch_process(const uint16_t frame[N_FFT], note_t *out);

/* Espectro |X[k]|, k = 0..N_FFT/2, do último quadro processado.
 * O bin k corresponde a k * DF Hz. */
const float *pitch_spectrum(void);
