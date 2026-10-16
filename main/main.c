/*
 * Afinador eletrônico (Tema I)
 * Microfone eletreto -> ADC contínuo -> segmentação -> Hann -> FFT -> nota
 * ESP32-S3, ESP-IDF v5.1 ou superior, componente espressif/esp-dsp
 *
 * Módulos:
 *   config.h   parâmetros compartilhados (fs, N_FFT, HOP, faixa de notas)
 *   mic_adc.c  leitura do microfone pelo ADC contínuo (DMA)
 *   pitch.c    janela + FFT + busca do fundamental f0
 *   note.c     conversão de f0 em nota e cents
 *   viz.c      envio dos quadros e do espectro para tools/visualizador.py
 *   main.c     laço principal: junta amostras em quadros de N_FFT com
 *              passo HOP, chama o detector e imprime o resultado
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "config.h"
#include "mic_adc.h"
#include "pitch.h"
#include "note.h"
#include "viz.h"

#define PRINT_EVERY       2                  /* imprime 1 a cada 2 quadros */

/* Buffers globais (grandes demais para a pilha do app_main) */
static uint16_t samples[MIC_READ_SAMPLES];
static uint16_t frame[N_FFT];

static void print_result(frame_result_t res, const note_t *note)
{
    switch (res) {
    case FRAME_SILENCE:
        printf("-- silencio\n");
        break;
    case FRAME_OUT_OF_RANGE:
        printf("-- fora da faixa (pico em %.1f Hz)\n", note->f0);
        break;
    case FRAME_NOTE:
        printf("Nota: %s%d (%s)  f0 = %.1f Hz  ref = %.2f Hz  %+ld cents\n",
               note->name, note->octave, note->solfege,
               note->f0, note->f_ref, lroundf(note->cents));
        break;
    }
}

void app_main(void)
{
    pitch_init();
    viz_init();
    mic_adc_start();

    printf("fs = %d Hz, N = %d, df = %.2f Hz, quadro = %.1f ms, hop = %d (%.0f%% sobreposicao)\n",
           SAMPLE_RATE_HZ, N_FFT, DF,
           1000.0f * N_FFT / SAMPLE_RATE_HZ, HOP, 100.0f * (N_FFT - HOP) / N_FFT);
    printf("Faixa de notas: C3 (%.2f Hz) a C5 (%.2f Hz)\n", NOTE_F_MIN, NOTE_F_MAX);

    int fill = 0;
    uint32_t frame_count = 0;
    note_t note;

    while (1) {
        size_t n = mic_adc_read(samples, MIC_READ_SAMPLES);

        for (size_t i = 0; i < n; i++) {
            frame[fill++] = samples[i];

            if (fill == N_FFT) {
                frame_result_t res = pitch_process(frame, &note);
                viz_send_frame(++frame_count, frame, pitch_spectrum(), res, &note);
                if (frame_count % PRINT_EVERY == 0) {
                    print_result(res, &note);
                }
                /* Mantém as últimas N_FFT - HOP amostras para o próximo quadro */
                memmove(frame, frame + HOP, (N_FFT - HOP) * sizeof(frame[0]));
                fill = N_FFT - HOP;
            }
        }
    }
}
