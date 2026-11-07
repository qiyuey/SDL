/* SPDX-License-Identifier: Zlib
 * Read-only input-rate observation. No rumble calls or nonzero effects.
 * Device initialization/sensor feature commands use the existing SDL driver.
 */
#include <SDL3/SDL.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

static SDL_JoystickID selected;
static SDL_AtomicInt gyro, accel, axes, buttons;

static bool SDLCALL watch(void *unused, SDL_Event *event)
{
    (void)unused;
    if (event->type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE && event->gsensor.which == selected) {
        if (event->gsensor.sensor == SDL_SENSOR_GYRO) SDL_AddAtomicInt(&gyro, 1);
        if (event->gsensor.sensor == SDL_SENSOR_ACCEL) SDL_AddAtomicInt(&accel, 1);
    } else if (event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && event->gaxis.which == selected) SDL_AddAtomicInt(&axes, 1);
    else if ((event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event->type == SDL_EVENT_GAMEPAD_BUTTON_UP) &&
             event->gbutton.which == selected) SDL_AddAtomicInt(&buttons, 1);
    return true;
}

static void sample(const char *phase, unsigned ms)
{
    SDL_SetAtomicInt(&gyro, 0); SDL_SetAtomicInt(&accel, 0);
    SDL_SetAtomicInt(&axes, 0); SDL_SetAtomicInt(&buttons, 0);
    Uint64 begin = SDL_GetTicksNS(), until = begin + SDL_MS_TO_NS(ms);
    printf("PHASE %s ns=%llu\n", phase, (unsigned long long)begin);
    while (SDL_GetTicksNS() < until) {
        SDL_UpdateJoysticks();
        SDL_Event event;
        while (SDL_PollEvent(&event)) { }
        SDL_DelayPrecise(500000);
    }
    printf("SAMPLE phase=%s begin_ns=%llu end_ns=%llu gyro=%u accel=%u axes=%u buttons=%u\n",
           phase, (unsigned long long)begin, (unsigned long long)SDL_GetTicksNS(), SDL_GetAtomicInt(&gyro), SDL_GetAtomicInt(&accel), SDL_GetAtomicInt(&axes), SDL_GetAtomicInt(&buttons));
}

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "--capture") && strcmp(argv[1], "--capture-09"))) {
        puts("Usage: testswitch2input --capture|--capture-09 (057e:2069 input only; no rumble)");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_SWITCH2_INPUT_TRACE", "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_SWITCH2_INPUT_TRACE_NATIVE_REPORT", !strcmp(argv[1], "--capture-09") ? "1" : "0");
    SDL_SetLogPriority(SDL_LOG_CATEGORY_INPUT, SDL_LOG_PRIORITY_INFO);
    if (!SDL_Init(SDL_INIT_GAMEPAD)) return 1;
    int count;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count; ++i) {
        if (SDL_GetGamepadVendorForID(ids[i]) == 0x057e && SDL_GetGamepadProductForID(ids[i]) == 0x2069) {
            selected = ids[i]; break;
        }
    }
    SDL_free(ids);
    if (!selected) { puts("SKIP no 057e:2069"); SDL_Quit(); return 77; }
    SDL_Gamepad *pad = SDL_OpenGamepad(selected);
    if (!pad) { puts(SDL_GetError()); SDL_Quit(); return 1; }
    FILETIME ft;
    Uint64 before = SDL_GetTicksNS();
    GetSystemTimePreciseAsFileTime(&ft);
    Uint64 after = SDL_GetTicksNS();
    printf("ANCHOR ns=%llu filetime_100ns=%llu uncertainty_ns=%llu\n",
           (unsigned long long)((before + after) / 2),
           ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime,
           (unsigned long long)(after - before));
    SDL_AddEventWatch(watch, NULL);
    sample("warmup", 1200);
    sample("sensors-off", 3000);
    bool gyro_ok = SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
    bool accel_ok = SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
    printf("SENSORS gyro=%d accel=%d\n", gyro_ok, accel_ok);
    sample("sensor-warmup", 500);
    sample("sensors-on", 6000);
    SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, false);
    SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, false);
    SDL_RemoveEventWatch(watch, NULL);
    SDL_CloseGamepad(pad);
    SDL_Quit();
    puts("RESULT input-only capture complete");
    return gyro_ok && accel_ok ? 0 : 1;
}
