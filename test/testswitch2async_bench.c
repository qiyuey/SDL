/* SPDX-License-Identifier: Zlib
 * Opt-in, low-amplitude public-API queue-depth comparison on USB 057e:2069.
 * All times are SDL monotonic nanoseconds; completion is not actuator onset.
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Gamepad *pad;
static unsigned call_id, failures;

static void wait_until(Uint64 due)
{
    for (;;) {
        Uint64 now = SDL_GetTicksNS();
        if (now >= due) return;
        if (due - now > 800000) SDL_DelayPrecise(due - now - 500000);
        else SDL_CPUPauseInstruction();
    }
}

static void service(unsigned ms)
{
    Uint64 due = SDL_GetTicksNS() + SDL_MS_TO_NS(ms);
    do { SDL_UpdateJoysticks(); SDL_Delay(1); } while (SDL_GetTicksNS() < due);
}

static void publish(const char *phase, unsigned trial, unsigned width, float value, Uint64 due)
{
    Uint64 begin = SDL_GetTicksNS();
    bool ok = SDL_RumbleGamepadFrequenciesEx(pad, value, 160, value ? .015f : 0, 320,
                                             value ? .015f : 0, 180, value, 360, 250);
    Uint64 end = SDL_GetTicksNS();
    if (!ok) ++failures;
    printf("PUB id=%u phase=%s trial=%u width=%u active=%d due_ns=%llu begin_ns=%llu end_ns=%llu ok=%d\n",
           call_id++, phase, trial, width, value != 0, (unsigned long long)due,
           (unsigned long long)begin, (unsigned long long)end, ok);
}

int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "--play") || atoi(argv[2]) < 1 || atoi(argv[2]) > 4) {
        puts("Usage: testswitch2async_bench --play SLOTS (1..4; low-amplitude real USB output)");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_SWITCH2_RUMBLE_SLOTS", argv[2]);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_SWITCH2_EXPERIMENTAL_HD", "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_RUMBLE_TRACE", "1");
    SDL_SetLogPriority(SDL_LOG_CATEGORY_INPUT, SDL_LOG_PRIORITY_INFO);
    if (!SDL_Init(SDL_INIT_GAMEPAD)) return 1;
    int count;
    SDL_JoystickID *ids = SDL_GetGamepads(&count), id = 0;
    for (int i = 0; i < count; ++i) {
        if (SDL_GetGamepadVendorForID(ids[i]) == 0x057e && SDL_GetGamepadProductForID(ids[i]) == 0x2069) {
            id = ids[i]; break;
        }
    }
    SDL_free(ids);
    if (!id) { puts("SKIP no 057e:2069"); SDL_Quit(); return 77; }
    pad = SDL_OpenGamepad(id);
    if (!pad) { puts(SDL_GetError()); SDL_Quit(); return 1; }
    SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
    SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
    service(300);
    for (unsigned period = 4; period <= 5; ++period) {
        const char *phase = period == 4 ? "curve4" : "curve5";
        Uint64 begin = SDL_GetTicksNS();
        for (unsigned i = 0; i < 3000 / period; ++i) {
            Uint64 due = begin + (Uint64)i * SDL_MS_TO_NS(period);
            wait_until(due);
            publish(phase, i, period, .02f + .03f * (i % 25) / 24.f, due);
            SDL_UpdateJoysticks();
        }
        // Let the last changing state complete before measuring the stop.
        service(20);
        publish(phase, 9999, period, 0, 0);
        service(30);
    }
    const unsigned widths[] = { 1, 2, 4, 5, 8, 12 };
    for (unsigned i = 0; i < SDL_arraysize(widths); ++i) {
        for (unsigned trial = 0; trial < 6; ++trial) {
            Uint64 begin = SDL_GetTicksNS();
            publish("pulse", trial, widths[i], .04f, begin);
            wait_until(begin + SDL_MS_TO_NS(widths[i]));
            publish("pulse", trial, widths[i], 0, begin + SDL_MS_TO_NS(widths[i]));
            service(25);
        }
    }
    for (unsigned trial = 0; trial < 12; ++trial) {
        for (unsigned i = 0; i < 4; ++i) {
            publish("burst", trial, i, .025f + i * .005f, 0);
            wait_until(SDL_GetTicksNS() + 200000);
        }
        publish("burst", trial, 4, 0, 0);
        service(30);
    }
    printf("PHASE close ns=%llu\n", (unsigned long long)SDL_GetTicksNS());
    SDL_CloseGamepad(pad);
    SDL_Quit();
    printf("RESULT slots=%s failures=%u calls=%u\n", argv[2], failures, call_id);
    return failures ? 1 : 0;
}
