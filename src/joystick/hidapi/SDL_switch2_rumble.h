/* Local Switch 2 Pro frequency experiment. See docs/README-switch2-hd-rumble-research.md. */
#ifndef SDL_switch2_rumble_h_
#define SDL_switch2_rumble_h_

/* US20260000974A1, expressions A/C. Choose by distance in Hz, not by
 * truncating the logarithm. This is a candidate hardware mapping. */
static Uint16 Switch2FrequencyCode(float hz)
{
    double x, lower_hz, upper_hz;
    int lower;
    if (!(hz > 0.0f)) {
        return 0;
    }
    x = 96.0 * SDL_log((double)hz / 110.0) / SDL_log(2.0) + 333.0;
    if (x <= 0.0) {
        return 0;
    }
    if (x >= 1023.0) {
        return 1023;
    }
    lower = (int)SDL_floor(x);
    lower_hz = 110.0 * SDL_pow(2.0, (lower - 333.0) / 96.0);
    upper_hz = 110.0 * SDL_pow(2.0, (lower + 1 - 333.0) / 96.0);
    return (Uint16)(hz - lower_hz <= upper_hz - hz ? lower : lower + 1);
}

/* Use the full 10-bit amplitude field without an additional gain. This is
 * the patent's candidate linear mapping, not a measured force. */
static Uint16 Switch2AmplitudeCode(float amplitude)
{
    /* The application has already applied its source-specific strength. */
    return (Uint16)SDL_floorf(SDL_clamp(amplitude, 0.0f, 1.0f) * 1023.0f + 0.5f);
}

/* Exactly the bit positions used by SDL's existing EncodeHDRumble:
 * first frequency [0:9], amplitude [10:19], second frequency [20:29],
 * amplitude [30:39]. Bit 9/29 are retained, not stripped as tone flags. */
static void Switch2PackRumble(Uint16 first_frequency, Uint16 first_amplitude,
                             Uint16 second_frequency, Uint16 second_amplitude, Uint8 out[5])
{
    Uint64 bits = ((Uint64)first_frequency & 1023) |
                  (((Uint64)first_amplitude & 1023) << 10) |
                  (((Uint64)second_frequency & 1023) << 20) |
                  (((Uint64)second_amplitude & 1023) << 30);
    int i;
    for (i = 0; i < 5; ++i) {
        out[i] = (Uint8)(bits >> (8 * i));
    }
}
#endif
