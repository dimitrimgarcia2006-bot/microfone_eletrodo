/*
 * Afinador eletrônico (Tema I)
 * Microfone eletreto -> ADC contínuo -> segmentação -> Hann -> FFT -> nota
 * ESP32-S3, ESP-IDF v5.1 ou superior, componente espressif/esp-dsp
 *
 * Ligação:
 *   3V3 -> resistor -> saída do microfone -> GPIO4 (ADC1_CH3)
 *   carcaça do microfone -> GND
 *
 * A cada quadro de N_FFT amostras:
 *   1. remove a média (nível DC de polarização)
 *   2. aplica a janela de Hann  w[n] = 0,5 - 0,5 cos(2 pi n / (N-1))
 *   3. calcula a FFT radix-2 da esp-dsp
 *   4. acha o pico dominante e testa os sub-harmônicos (kp/4, kp/3, kp/2)
 *      para chegar ao fundamental f0, dentro da faixa C3..C5 (2 oitavas)
 *   5. refina f0 por interpolação parabólica e converte em nota:
 *      n = 49 + 12 log2(f0 / 440)   (n = tecla do piano, A4 = 49)
 *
 * Segmentação:
 *   HOP = N_FFT      -> quadros sem sobreposição
 *   HOP = N_FFT / 2  -> 50% de sobreposição
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "soc/soc_caps.h"
#include "esp_adc/adc_continuous.h"
#include "esp_dsp.h"

/* ---------------- Parâmetros ---------------- */
#define MIC_ADC_CHANNEL   ADC_CHANNEL_3      /* GPIO4 no ESP32-S3 */
#define MIC_ADC_ATTEN     ADC_ATTEN_DB_12

#define SAMPLE_RATE_HZ    8000
#define N_FFT             2048               /* potência de 2; df = 3,9 Hz */
#define HOP               (N_FFT / 2)        /* N_FFT = sem sobreposição */

/* Faixa de notas: 2 oitavas, de C3 a C5, com folga de meio semitom */
#define NOTE_F_MIN        130.81f            /* C3 */
#define NOTE_F_MAX        523.25f            /* C5 */
#define HALF_SEMITONE     1.0293022f         /* 2^(1/24) */
#define F0_MIN            (NOTE_F_MIN / HALF_SEMITONE)
#define F0_MAX            (NOTE_F_MAX * HALF_SEMITONE)

#define N_HARM_MAX        4                  /* pico dominante pode ser até o 4º harmônico */
#define SUBHARM_REL_DB    20.0f              /* fundamental até 20 dB abaixo do pico dominante */
#define MIN_AMP_CNT       4.0f               /* amplitude mínima do pico (contagens) */
#define PRINT_EVERY       2                  /* imprime 1 a cada 2 quadros */

#define READ_SAMPLES      256
#define READ_BYTES        (READ_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES)

_Static_assert((N_FFT & (N_FFT - 1)) == 0, "N_FFT precisa ser potencia de 2");
_Static_assert(HOP > 0 && HOP <= N_FFT, "HOP precisa estar entre 1 e N_FFT");

static const char *TAG = "afinador";

/* Buffers globais (grandes demais para a pilha do app_main) */
static uint16_t frame[N_FFT];
static float    win[N_FFT];
static float    y_cf[2 * N_FFT] __attribute__((aligned(16)));  /* complexo intercalado: re, im */
static float    mag[N_FFT / 2 + 1];
static uint8_t  adc_buf[READ_BYTES];

/* ---------------- Notas ---------------- */
typedef enum {
    FRAME_SILENCE,
    FRAME_OUT_OF_RANGE,
    FRAME_NOTE,
} frame_result_t;

typedef struct {
    float f0;           /* fundamental estimado (Hz); fora da faixa: pico dominante */
    float f_ref;        /* frequência da nota afinada (Hz) */
    float cents;        /* desvio de f0 em relação a f_ref */
    int   n;            /* tecla do piano (A4 = 49) */
    int   octave;
    const char *name;
    const char *solfege;
} note_t;

static const char *const NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};
static const char *const SOLFEGE[12] = {
    "Do", "Do#", "Re", "Re#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si"
};

/* ---------------- ADC contínuo ---------------- */
static adc_continuous_handle_t adc_init(void)
{
    adc_continuous_handle_t handle = NULL;

    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = 8 * READ_BYTES,   /* folga de ~256 ms */
        .conv_frame_size    = READ_BYTES,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &handle));

    adc_digi_pattern_config_t pattern = {
        .atten     = MIC_ADC_ATTEN,
        .channel   = MIC_ADC_CHANNEL,
        .unit      = ADC_UNIT_1,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    adc_continuous_config_t cfg = {
        .pattern_num    = 1,
        .adc_pattern    = &pattern,
        .sample_freq_hz = SAMPLE_RATE_HZ,
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };
    ESP_ERROR_CHECK(adc_continuous_config(handle, &cfg));
    return handle;
}

/* ---------------- Janela e FFT (esp-dsp) ---------------- */
static void dsp_init(void)
{
    dsps_wind_hann_f32(win, N_FFT);
    ESP_ERROR_CHECK(dsps_fft2r_init_fc32(NULL, N_FFT));
}

/* Interpolação parabólica sobre o log da magnitude (bom para Hann) */
static float refine_freq(int k)
{
    const float eps = 1e-6f;
    float a = logf(mag[k - 1] + eps);
    float b = logf(mag[k]     + eps);
    float c = logf(mag[k + 1] + eps);
    float den = a - 2.0f * b + c;
    float p = (fabsf(den) > eps) ? 0.5f * (a - c) / den : 0.0f;
    return (k + p) * (float)SAMPLE_RATE_HZ / N_FFT;
}

/* Fórmula inversa de f(n) = 440 * 2^((n - 49) / 12) */
static void identify_note(float f0, note_t *out)
{
    float n_real = 49.0f + 12.0f * log2f(f0 / 440.0f);
    int   n      = (int)lroundf(n_real);
    int   idx    = (n + 8) % 12;            /* n = 40 -> C4 */

    out->f0      = f0;
    out->n       = n;
    out->octave  = (n + 8) / 12;
    out->name    = NOTE_NAMES[idx];
    out->solfege = SOLFEGE[idx];
    out->f_ref   = 440.0f * powf(2.0f, (n - 49) / 12.0f);
    out->cents   = 100.0f * (n_real - n);
}

/* ---------------- Processamento de um quadro ---------------- */
static frame_result_t process_frame(note_t *out)
{
    const float df = (float)SAMPLE_RATE_HZ / N_FFT;

    /* 1. Remove a média */
    float mean = 0.0f;
    for (int n = 0; n < N_FFT; n++) {
        mean += frame[n];
    }
    mean /= N_FFT;

    /* 2. Janela de Hann na parte real; parte imaginária zerada */
    for (int n = 0; n < N_FFT; n++) {
        y_cf[2 * n]     = ((float)frame[n] - mean) * win[n];
        y_cf[2 * n + 1] = 0.0f;
    }

    /* 3. FFT e magnitude (só metade, o espectro de sinal real é simétrico) */
    dsps_fft2r_fc32(y_cf, N_FFT);
    dsps_bit_rev_fc32(y_cf, N_FFT);
    for (int k = 0; k <= N_FFT / 2; k++) {
        float r = y_cf[2 * k];
        float i = y_cf[2 * k + 1];
        mag[k] = sqrtf(r * r + i * i);
    }

    /* 4. Pico dominante: pode ser o fundamental ou um dos harmônicos */
    int kmin = (int)ceilf(F0_MIN / df);
    int kmax = (int)(N_HARM_MAX * F0_MAX / df);
    if (kmax > N_FFT / 2 - 1) kmax = N_FFT / 2 - 1;

    int kp = kmin;
    for (int k = kmin + 1; k <= kmax; k++) {
        if (mag[k] > mag[kp]) kp = k;
    }

    /* Senoide de amplitude A com Hann: |X[k]| ~ A * N / 4  =>  A ~ 4|X|/N */
    if (4.0f * mag[kp] / N_FFT < MIN_AMP_CNT) {
        return FRAME_SILENCE;
    }

    /* 5. Fundamental: testa kp/d do mais grave (d = N_HARM_MAX) ao mais agudo
     *    (d = 1). Para d >= 2 exige um máximo local perto de kp/d com no máximo
     *    SUBHARM_REL_DB abaixo do dominante. O primeiro aceito é o f0. */
    const float rel_min = mag[kp] * powf(10.0f, -SUBHARM_REL_DB / 20.0f);
    int k0 = -1;
    for (int d = N_HARM_MAX; d >= 1; d--) {
        float kc = (float)kp / d;
        if (kc * df < F0_MIN || kc * df > F0_MAX) {
            continue;
        }
        if (d == 1) {
            k0 = kp;
            break;
        }
        /* Maior bin dentro do lóbulo principal da Hann (+-2 bins) em torno de kp/d */
        int kc_i = (int)lroundf(kc);
        int kb = kc_i - 2;
        for (int k = kc_i - 1; k <= kc_i + 2; k++) {
            if (mag[k] > mag[kb]) kb = k;
        }
        if (mag[kb] >= rel_min && mag[kb] > mag[kb - 1] && mag[kb] >= mag[kb + 1]) {
            k0 = kb;
            break;
        }
    }

    if (k0 < 0) {
        out->f0 = refine_freq(kp);
        return FRAME_OUT_OF_RANGE;
    }

    /* 6. Refina f0 e converte em nota */
    float f0 = refine_freq(k0);
    if (f0 < F0_MIN || f0 > F0_MAX) {
        out->f0 = f0;
        return FRAME_OUT_OF_RANGE;
    }
    identify_note(f0, out);
    return FRAME_NOTE;
}

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

/* ---------------- Principal ---------------- */
void app_main(void)
{
    dsp_init();
    adc_continuous_handle_t adc = adc_init();
    ESP_ERROR_CHECK(adc_continuous_start(adc));

    printf("fs = %d Hz, N = %d, df = %.2f Hz, quadro = %.1f ms, hop = %d (%.0f%% sobreposicao)\n",
           SAMPLE_RATE_HZ, N_FFT, (float)SAMPLE_RATE_HZ / N_FFT,
           1000.0f * N_FFT / SAMPLE_RATE_HZ, HOP, 100.0f * (N_FFT - HOP) / N_FFT);
    printf("Faixa de notas: C3 (%.2f Hz) a C5 (%.2f Hz)\n", NOTE_F_MIN, NOTE_F_MAX);

    int fill = 0;
    uint32_t frame_count = 0;
    note_t note;

    while (1) {
        uint32_t n_bytes = 0;
        esp_err_t ret = adc_continuous_read(adc, adc_buf, READ_BYTES, &n_bytes, 1000);
        if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Timeout lendo o ADC");
            continue;
        }
        ESP_ERROR_CHECK(ret);

        for (uint32_t i = 0; i < n_bytes; i += SOC_ADC_DIGI_RESULT_BYTES) {
            adc_digi_output_data_t *p = (adc_digi_output_data_t *)&adc_buf[i];
            if (p->type2.channel != MIC_ADC_CHANNEL) {
                continue;
            }
            frame[fill++] = (uint16_t)p->type2.data;

            if (fill == N_FFT) {
                frame_result_t res = process_frame(&note);
                if (++frame_count % PRINT_EVERY == 0) {
                    print_result(res, &note);
                }
                /* Mantém as últimas N_FFT - HOP amostras para o próximo quadro */
                memmove(frame, frame + HOP, (N_FFT - HOP) * sizeof(frame[0]));
                fill = N_FFT - HOP;
            }
        }
    }
}
