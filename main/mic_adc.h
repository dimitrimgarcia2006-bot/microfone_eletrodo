/*
 * Microfone eletreto no ADC contínuo (DMA)
 *
 * Ligação:
 *   3V3 -> resistor -> saída do microfone -> GPIO4 (ADC1_CH3)
 *   carcaça do microfone -> GND
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MIC_READ_SAMPLES  256                /* amostras por leitura (32 ms a 8 kHz) */

/* Configura o ADC contínuo a SAMPLE_RATE_HZ e inicia a conversão */
void mic_adc_start(void);

/* Bloqueia até chegarem amostras (no máximo 1 s) e copia até max_samples
 * para out. Devolve o número de amostras copiadas; 0 em caso de timeout. */
size_t mic_adc_read(uint16_t *out, size_t max_samples);
