/* Public haptics API regression checks using only virtual gamepads.
 * No physical gamepad is opened and no HID output is sent.
 * SPDX-License-Identifier: Zlib
 */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct Capture
{
    unsigned calls;
    Uint16 low, high;
    bool fail;
} Capture;

static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "line %d failed: %s (%s)\n", __LINE__, #condition, SDL_GetError()); return 1; \
} } while (0)

static bool SDLCALL capture_rumble(void *userdata, Uint16 low, Uint16 high)
{
    Capture *capture = userdata;
    ++capture->calls;
    if (capture->fail) {
        return SDL_SetError("Synthetic transport failure");
    }
    capture->low = low;
    capture->high = high;
    return true;
}

static SDL_Gamepad *attach_gamepad(Capture *capture, Uint16 vendor, Uint16 product, SDL_JoystickID *id)
{
    SDL_VirtualJoystickDesc description;
    SDL_INIT_INTERFACE(&description);
    description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    description.vendor_id = vendor;
    description.product_id = product;
    description.naxes = SDL_GAMEPAD_AXIS_COUNT;
    description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    description.name = "Betop API virtual regression controller";
    description.userdata = capture;
    description.Rumble = capture_rumble;
    *id = SDL_AttachVirtualJoystick(&description);
    return *id ? SDL_OpenGamepad(*id) : NULL;
}

static bool frequencies(SDL_Gamepad *pad, float ll, float lh, float rl, float rh, Uint32 duration)
{
    return SDL_RumbleGamepadFrequenciesEx(pad, ll, 160, lh, 320, rl, 160, rh, 320, duration);
}

static int test_fallback(void)
{
    SDL_JoystickID id;
    Capture capture = {0};
    SDL_Gamepad *pad = attach_gamepad(&capture, 0x1209, 0x0001, &id);
    unsigned calls;
    CHECK(pad != NULL);
    CHECK(frequencies(pad, .25f, .75f, .5f, .125f, 1000));
    CHECK(capture.low == 32767 && capture.high == 49151);
    calls = capture.calls;
    CHECK(SDL_RumbleGamepad(pad, 0, 0, 0));
    CHECK(capture.calls == calls + 1 && capture.low == 0 && capture.high == 0);

    CHECK(frequencies(pad, 0, 1, 0, 0, 20));
    CHECK(capture.low == 0 && capture.high == 65535);
    SDL_Delay(40);
    SDL_UpdateJoysticks();
    CHECK(capture.low == 0 && capture.high == 0);

    CHECK(frequencies(pad, 1, 0, 0, 0, 10));
    CHECK(frequencies(pad, 1, 0, 0, 0, 500));
    SDL_Delay(30);
    SDL_UpdateJoysticks();
    CHECK(capture.low == 65535 && capture.high == 0);
    SDL_Delay(500);
    SDL_UpdateJoysticks();
    CHECK(capture.low == 0 && capture.high == 0);

    /* A previous ordinary timer must not cancel a longer frequency lease. */
    CHECK(SDL_RumbleGamepad(pad, 100, 200, 10));
    CHECK(frequencies(pad, 0, 0, .25f, .5f, 500));
    SDL_Delay(30);
    SDL_UpdateJoysticks();
    CHECK(capture.low == 16383 && capture.high == 32767);
    CHECK(frequencies(pad, 0, 0, 0, 0, 0));
    CHECK(capture.low == 0 && capture.high == 0);

    /* Failed delivery must not poison the success cache. */
    capture.fail = true;
    CHECK(!frequencies(pad, 1, 1, 0, 0, 1000));
    CHECK(capture.low == 0 && capture.high == 0);
    capture.fail = false;
    calls = capture.calls;
    CHECK(SDL_RumbleGamepad(pad, 65535, 65535, 1000));
    CHECK(capture.calls == calls + 1 && capture.low == 65535 && capture.high == 65535);
    CHECK(frequencies(pad, .25f, .5f, 0, 0, 1000));
    calls = capture.calls;
    SDL_CloseGamepad(pad);
    CHECK(capture.calls == calls + 1 && capture.low == 0 && capture.high == 0);
    CHECK(SDL_DetachVirtualJoystick(id));
    return 0;
}

static int test_validation(void)
{
    static const float invalid_amplitude[] = { NAN, INFINITY, -INFINITY, -0.1f, 1.1f };
    static const float invalid_frequency[] = { NAN, INFINITY, -INFINITY, -1.0f };
    SDL_BetopGamepadEffects effects;
    SDL_JoystickID id;
    Capture capture = {0};
    SDL_Gamepad *pad;
    unsigned calls;
    int band, i;

    SDL_zero(effects);
    CHECK(!SDL_SetGamepadBetopEffects(NULL, &effects, 0));
    CHECK(!frequencies(NULL, 0, 0, 0, 0, 0));
    /* Even a virtual pad advertising Betop IDs must never bind real hardware. */
    pad = attach_gamepad(&capture, 0x20bc, 0x5174, &id);
    CHECK(pad != NULL);
    CHECK(SDL_IsJoystickVirtual(id));
    calls = capture.calls;
    CHECK(!SDL_SetGamepadBetopEffects(pad, NULL, 0));
    CHECK(capture.calls == calls);
    effects.triggers[1].mode = 5;
    CHECK(!SDL_SetGamepadBetopEffects(pad, &effects, 100));
    CHECK(capture.calls == calls);
    SDL_zero(effects);
    effects.triggers[0].mode = 1;
    effects.triggers[0].parameters[0] = 10;
    effects.triggers[0].parameters[1] = 20;
    CHECK(!SDL_SetGamepadBetopEffects(pad, &effects, 0));
    CHECK(strstr(SDL_GetError(), "lease") || strstr(SDL_GetError(), "duration"));
    CHECK(capture.calls == calls);
    CHECK(!SDL_SetGamepadBetopEffects(pad, &effects, 1001));
    CHECK(strstr(SDL_GetError(), "lease") || strstr(SDL_GetError(), "duration"));
    CHECK(capture.calls == calls);
    effects.triggers[0].parameters[5] = 1;
    CHECK(!SDL_SetGamepadBetopEffects(pad, &effects, 100));
    CHECK(capture.calls == calls);
    SDL_zero(effects);
    effects.shoulders[0].preset = 5;
    CHECK(!SDL_SetGamepadBetopEffects(pad, &effects, 100));
    CHECK(capture.calls == calls);
    SDL_zero(effects);
    effects.shoulders[1].preset = 1;
    effects.shoulders[1].strength = 3;
    CHECK(!SDL_SetGamepadBetopEffects(pad, &effects, 100));
    CHECK(capture.calls == calls);
    SDL_zero(effects);
    effects.shoulders[0].preset = 4;
    effects.shoulders[0].strength = 255;
    CHECK(!SDL_SetGamepadBetopEffects(pad, &effects, 0));
    CHECK(strstr(SDL_GetError(), "lease") != NULL);
    CHECK(!SDL_SetGamepadBetopEffects(pad, &effects, 1001));
    CHECK(strstr(SDL_GetError(), "lease") != NULL);
    effects.shoulders[0].strength = 0;
    CHECK(!SDL_SetGamepadBetopEffects(pad, &effects, 0));
    CHECK(strstr(SDL_GetError(), "not supported") != NULL);
    CHECK(capture.calls == calls);
    SDL_zero(effects);
    CHECK(!SDL_SetGamepadBetopEffects(pad, &effects, 0));
    CHECK(strstr(SDL_GetError(), "not supported") != NULL);
    CHECK(capture.calls == calls);
    CHECK(!SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), "SDL.gamepad.betop.haptics", false));

    for (band = 0; band < 4; ++band) {
        for (i = 0; i < (int)SDL_arraysize(invalid_amplitude); ++i) {
            float amplitudes[4] = {0};
            amplitudes[band] = invalid_amplitude[i];
            CHECK(!frequencies(pad, amplitudes[0], amplitudes[1], amplitudes[2], amplitudes[3], 100));
            CHECK(capture.calls == calls);
        }
        for (i = 0; i < (int)SDL_arraysize(invalid_frequency); ++i) {
            float hz[4] = {160, 320, 160, 320};
            hz[band] = invalid_frequency[i];
            CHECK(!SDL_RumbleGamepadFrequenciesEx(pad, .1f, hz[0], .2f, hz[1], .3f, hz[2], .4f, hz[3], 100));
            CHECK(capture.calls == calls);
        }
    }
    CHECK(frequencies(pad, .5f, 0, 0, .25f, 100));
    CHECK(capture.low == 32767 && capture.high == 16383);
    CHECK(SDL_RumbleGamepad(pad, 0, 0, 0));
    CHECK(SDL_DetachVirtualJoystick(id));
    calls = capture.calls;
    CHECK(!frequencies(pad, 1, 1, 1, 1, 100));
    CHECK(!SDL_SetGamepadBetopEffects(pad, &effects, 0));
    CHECK(capture.calls == calls);
    SDL_CloseGamepad(pad);
    return 0;
}

int main(void)
{
    int result;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    result = test_fallback() || test_validation();
    SDL_Quit();
    if (!result) printf("Betop API: %u virtual-device checks passed; no physical device output.\n", checks);
    return result;
}
