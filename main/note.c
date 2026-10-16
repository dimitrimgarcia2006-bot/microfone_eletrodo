#include "note.h"

#include <math.h>

static const char *const NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};
static const char *const SOLFEGE[12] = {
    "Do", "Do#", "Re", "Re#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si"
};

/* Fórmula inversa de f(n) = 440 * 2^((n - 49) / 12) */
void note_from_freq(float f0, note_t *out)
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
