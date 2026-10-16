/*
 * Conversão de frequência em nota musical
 *   n = 49 + 12 log2(f0 / 440)   (n = tecla do piano, A4 = 49)
 */
#pragma once

typedef struct {
    float f0;           /* fundamental estimado (Hz); fora da faixa: pico dominante */
    float f_ref;        /* frequência da nota afinada (Hz) */
    float cents;        /* desvio de f0 em relação a f_ref */
    int   n;            /* tecla do piano (A4 = 49) */
    int   octave;
    const char *name;
    const char *solfege;
} note_t;

/* Preenche out com a nota mais próxima de f0 e o desvio em cents */
void note_from_freq(float f0, note_t *out);
