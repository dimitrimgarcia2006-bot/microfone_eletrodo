#include "pitch.h"

#include <math.h>
#include "esp_err.h"
#include "esp_dsp.h"

#define N_HARM_MAX        4                  /* pico dominante pode ser até o 4º harmônico */
#define SUBHARM_REL_DB    20.0f              /* fundamental até 20 dB abaixo do pico dominante */
#define MIN_AMP_CNT       4.0f               /* amplitude mínima do pico (contagens) */

/* Buffers globais (grandes demais para a pilha) */
static float win[N_FFT];
static float y_cf[2 * N_FFT] __attribute__((aligned(16)));  /* complexo intercalado: re, im */
static float mag[N_FFT / 2 + 1];

void pitch_init(void)
{
    dsps_wind_hann_f32(win, N_FFT);
    ESP_ERROR_CHECK(dsps_fft2r_init_fc32(NULL, N_FFT));
}

/* 1 e 2. Remove a média e aplica a Hann na parte real; parte imaginária zerada */
static void remove_dc_and_window(const uint16_t *frame)
{
    float mean = 0.0f;
    for (int n = 0; n < N_FFT; n++) {
        mean += frame[n];
    }
    mean /= N_FFT;

    for (int n = 0; n < N_FFT; n++) {
        y_cf[2 * n]     = ((float)frame[n] - mean) * win[n];
        y_cf[2 * n + 1] = 0.0f;
    }
}

/* 3. FFT e magnitude (só metade, o espectro de sinal real é simétrico) */
static void compute_magnitude(void)
{
    dsps_fft2r_fc32(y_cf, N_FFT);
    dsps_bit_rev_fc32(y_cf, N_FFT);
    for (int k = 0; k <= N_FFT / 2; k++) {
        float r = y_cf[2 * k];
        float i = y_cf[2 * k + 1];
        mag[k] = sqrtf(r * r + i * i);
    }
}

/* 4. Pico dominante: pode ser o fundamental ou um dos harmônicos */
static int find_dominant_peak(void)
{
    int kmin = (int)ceilf(F0_MIN / DF);
    int kmax = (int)(N_HARM_MAX * F0_MAX / DF);
    if (kmax > N_FFT / 2 - 1) kmax = N_FFT / 2 - 1;

    int kp = kmin;
    for (int k = kmin + 1; k <= kmax; k++) {
        if (mag[k] > mag[kp]) kp = k;
    }
    return kp;
}

/* 5. Fundamental: testa kp/d do mais grave (d = N_HARM_MAX) ao mais agudo
 *    (d = 1). Para d >= 2 exige um máximo local perto de kp/d com no máximo
 *    SUBHARM_REL_DB abaixo do dominante. O primeiro aceito é o f0.
 *    Devolve o bin do fundamental, ou -1 se nenhum candidato cai na faixa. */
static int find_fundamental(int kp)
{
    const float rel_min = mag[kp] * powf(10.0f, -SUBHARM_REL_DB / 20.0f);

    for (int d = N_HARM_MAX; d >= 1; d--) {
        float kc = (float)kp / d;
        if (kc * DF < F0_MIN || kc * DF > F0_MAX) {
            continue;
        }
        if (d == 1) {
            return kp;
        }
        /* Maior bin dentro do lóbulo principal da Hann (+-2 bins) em torno de kp/d */
        int kc_i = (int)lroundf(kc);
        int kb = kc_i - 2;
        for (int k = kc_i - 1; k <= kc_i + 2; k++) {
            if (mag[k] > mag[kb]) kb = k;
        }
        if (mag[kb] >= rel_min && mag[kb] > mag[kb - 1] && mag[kb] >= mag[kb + 1]) {
            return kb;
        }
    }
    return -1;
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
    return (k + p) * DF;
}

frame_result_t pitch_process(const uint16_t frame[N_FFT], note_t *out)
{
    remove_dc_and_window(frame);
    compute_magnitude();

    int kp = find_dominant_peak();

    /* Senoide de amplitude A com Hann: |X[k]| ~ A * N / 4  =>  A ~ 4|X|/N */
    if (4.0f * mag[kp] / N_FFT < MIN_AMP_CNT) {
        return FRAME_SILENCE;
    }

    int k0 = find_fundamental(kp);
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
    note_from_freq(f0, out);
    return FRAME_NOTE;
}

const float *pitch_spectrum(void)
{
    return mag;
}
