/* Standalone local codec check: link against SDL3 and run without a controller. */
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include "../src/joystick/hidapi/SDL_switch2_rumble.h"

static Uint64 unpack(const Uint8 b[5])
{
    Uint64 bits = 0;
    int i;
    for (i = 0; i < 5; ++i) bits |= (Uint64)b[i] << (i * 8);
    return bits;
}

int main(void)
{
    Uint8 packet[5];
    int code;
    assert(Switch2FrequencyCode(27.5f) == 141);
    assert(Switch2FrequencyCode(55.0f) == 237);
    assert(Switch2FrequencyCode(110.0f) == 333);
    assert(Switch2FrequencyCode(160.0f) == 385);
    assert(Switch2FrequencyCode(320.0f) == 481);
    assert(Switch2FrequencyCode(400.0f) == 512);
    assert(Switch2FrequencyCode(0) == 0);
    assert(Switch2FrequencyCode(100000) == 1023);
    assert(Switch2AmplitudeCodeWithGain(0, 2) == 0);
    assert(Switch2AmplitudeCodeWithGain(1, 1) == 1023);
    assert(Switch2AmplitudeCodeWithGain(1, 3) == 1023);
    assert(Switch2AmplitudeCodeWithGain(.5f, 1) == 512);
    assert(Switch2AmplitudeCodeWithGain(.25f, 1) == 256);
    assert(Switch2AmplitudeCodeWithGain(.25f, 2) == Switch2AmplitudeCodeWithGain(.5f, 1));
    assert(Switch2AmplitudeCodeWithGain(.25f, 0) == Switch2AmplitudeCodeWithGain(.25f, 1));
    assert(Switch2AmplitudeCodeWithGain(.25f, 4) == Switch2AmplitudeCodeWithGain(.25f, 1));
    for (code = 0; code <= 1023; ++code) {
        Uint64 v;
        float hz = (float)(110.0 * SDL_pow(2.0, (code - 333.0) / 96.0));
        assert(Switch2FrequencyCode(hz) == code);
        Switch2PackRumble((Uint16)code, (Uint16)(1023-code), (Uint16)(code^512), (Uint16)(code^341), packet);
        v = unpack(packet);
        assert((v & 1023) == (Uint64)code);
        assert(((v >> 10) & 1023) == (Uint64)(1023-code));
        assert(((v >> 20) & 1023) == (Uint64)(code^512));
        assert(((v >> 30) & 1023) == (Uint64)(code^341));
        /* Cross-check the old byte encoder, with its 16-bit amplitude input. */
        {
            Uint16 a = (Uint16)((1023-code) << 6), b = (Uint16)((code^341) << 6);
            Uint16 f = (Uint16)code, g = (Uint16)(code^512);
            Uint8 old[5] = {(Uint8)f, (Uint8)(((a >> 4) & 0xfc) | ((f >> 8) & 3)),
                (Uint8)((a >> 12) | (g << 4)), (Uint8)((b & 0xc0) | ((g >> 4) & 0x3f)), (Uint8)(b >> 8)};
            assert(SDL_memcmp(old, packet, 5) == 0);
        }
    }
    puts("PASS: patent examples, all 1024 codes, field isolation and legacy encoder equivalence");
    return 0;
}
