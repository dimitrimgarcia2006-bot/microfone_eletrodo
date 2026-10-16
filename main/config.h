/*
 * Parâmetros compartilhados entre os módulos do afinador
 *
 * Segmentação:
 *   HOP = N_FFT      -> quadros sem sobreposição
 *   HOP = N_FFT / 2  -> 50% de sobreposição
 */
#pragma once

#define SAMPLE_RATE_HZ    8000
#define N_FFT             2048               /* potência de 2; df = 3,9 Hz */
#define HOP               (N_FFT / 2)        /* N_FFT = sem sobreposição */
#define DF                ((float)SAMPLE_RATE_HZ / N_FFT)   /* resolução da FFT (Hz/bin) */

/* Faixa de notas: 2 oitavas, de C3 a C5, com folga de meio semitom */
#define NOTE_F_MIN        130.81f            /* C3 */
#define NOTE_F_MAX        523.25f            /* C5 */
#define HALF_SEMITONE     1.0293022f         /* 2^(1/24) */
#define F0_MIN            (NOTE_F_MIN / HALF_SEMITONE)
#define F0_MAX            (NOTE_F_MAX * HALF_SEMITONE)

_Static_assert((N_FFT & (N_FFT - 1)) == 0, "N_FFT precisa ser potencia de 2");
_Static_assert(HOP > 0 && HOP <= N_FFT, "HOP precisa estar entre 1 e N_FFT");
