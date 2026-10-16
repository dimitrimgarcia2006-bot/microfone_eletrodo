#include "viz.h"

#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#define VIZ_VERSION       1
#define VIZ_KEEPALIVE     0x02               /* byte que o PC manda para manter o envio ligado */
#define VIZ_TIMEOUT_MS    2000
#define N_BINS            (N_FFT / 2 + 1)

typedef struct __attribute__((packed)) {
    uint8_t  magic[4];
    uint16_t version;
    uint16_t n_samples;
    uint16_t n_bins;
    uint16_t sample_rate;
    uint32_t seq;
    uint8_t  result;
    uint8_t  reserved;
    int16_t  note_n;
    float    f0;
    float    cents;
} viz_header_t;

_Static_assert(sizeof(viz_header_t) == 28, "cabecalho precisa ter 28 bytes (igual ao visualizador.py)");

#define PACKET_BYTES      (sizeof(viz_header_t) + N_FFT * sizeof(uint16_t) \
                           + N_BINS * sizeof(float) + sizeof(uint16_t))

static const uint8_t MAGIC[4] = { 0xA5, 0x5A, 'F', 'T' };

static uint8_t    packet[PACKET_BYTES];
static bool       active;
static TickType_t last_keepalive;

void viz_init(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 3 * PACKET_BYTES,  /* até 3 quadros na fila */
        .rx_buffer_size = 256,               /* o driver exige mais que 64 (FIFO da USB) */
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));

    /* O printf do console secundário (USB) passa a usar o mesmo driver.
     * Sem isso, texto e pacotes disputariam a FIFO da USB e se misturariam. */
    usb_serial_jtag_vfs_use_driver();
}

/* Lê o que chegou do PC e diz se o visualizador continua conectado */
static bool check_keepalive(void)
{
    uint8_t rx[16];
    int n;
    while ((n = usb_serial_jtag_read_bytes(rx, sizeof(rx), 0)) > 0) {
        if (memchr(rx, VIZ_KEEPALIVE, n)) {
            last_keepalive = xTaskGetTickCount();
            active = true;
        }
    }
    if (active && xTaskGetTickCount() - last_keepalive > pdMS_TO_TICKS(VIZ_TIMEOUT_MS)) {
        active = false;
    }
    return active;
}

void viz_send_frame(uint32_t seq, const uint16_t frame[N_FFT], const float *mag,
                    frame_result_t res, const note_t *note)
{
    if (!check_keepalive()) {
        return;
    }

    viz_header_t h = {
        .version     = VIZ_VERSION,
        .n_samples   = N_FFT,
        .n_bins      = N_BINS,
        .sample_rate = SAMPLE_RATE_HZ,
        .seq         = seq,
        .result      = (uint8_t)res,
    };
    memcpy(h.magic, MAGIC, sizeof(MAGIC));
    if (res != FRAME_SILENCE) {
        h.f0 = note->f0;
    }
    if (res == FRAME_NOTE) {
        h.note_n = (int16_t)note->n;
        h.cents  = note->cents;
    }

    /* Monta o pacote inteiro num buffer só: o driver envia tudo ou nada */
    uint8_t *p = packet;
    memcpy(p, &h, sizeof(h));                   p += sizeof(h);
    memcpy(p, frame, N_FFT * sizeof(uint16_t)); p += N_FFT * sizeof(uint16_t);
    memcpy(p, mag, N_BINS * sizeof(float));     p += N_BINS * sizeof(float);

    uint16_t sum = 0;
    for (const uint8_t *q = packet + sizeof(MAGIC); q < p; q++) {
        sum += *q;
    }
    memcpy(p, &sum, sizeof(sum));

    /* Sem espera: se o PC não estiver lendo rápido, o quadro é descartado */
    usb_serial_jtag_write_bytes(packet, PACKET_BYTES, 0);
}
