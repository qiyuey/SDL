/* Local KP70C integration probe. Listing never sends a haptic output report. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_hidapi.h>
#include <stdio.h>

static bool Stop(SDL_Gamepad *pad)
{
    SDL_BetopGamepadEffects effects = {0};
    bool a = SDL_SetGamepadBetopEffects(pad, &effects, 0);
    bool b = SDL_RumbleGamepadFrequenciesEx(pad, 0, 160, 0, 320, 0, 160, 0, 320, 0);
    return a && b;
}

static bool TriggerTest(SDL_Gamepad *pad, int mode, int selected_side)
{
    bool held[2] = {false, false};
    const bool prearmed = mode == 3;
    Uint64 end = SDL_GetTicks() + (selected_side < 0 ? 20000 : 12000);
    if (!Stop(pad)) return false;
    printf("Trigger test: mode %d, side %s. Slowly press, hold, then release; BACK / VIEW stops.\n",
           mode, selected_side < 0 ? "both" : selected_side ? "RIGHT / RT" : "LEFT / LT");
    fflush(stdout);
    if (prearmed) {
        puts("Breakthrough stays armed for this stage; travel only records input and does not gate output.");
        fflush(stdout);
    }
    while (SDL_GetTicks() < end) {
        SDL_BetopGamepadEffects effects = {0};
        SDL_PumpEvents();
        if (!SDL_GamepadConnected(pad)) return false;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK)) return Stop(pad);
        for (int side = 0; side < 2; ++side) {
            if (selected_side >= 0 && side != selected_side) continue;
            Sint16 value = SDL_GetGamepadAxis(pad, side ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
            bool pressed = value > 3277;
            if (pressed != held[side]) {
                const char *state = prearmed ? (pressed ? "pressed / effect remains armed" : "released / effect remains armed") :
                                              (pressed ? "effect requested" : "released / stop requested");
                printf("%s: %s (travel %.0f%%)\n", side ? "RT" : "LT", state, value * 100.0 / 32767);
                fflush(stdout);
                held[side] = pressed;
            }
            if (pressed || prearmed) {
                effects.triggers[side].mode = (Uint8)mode;
                effects.triggers[side].parameters[0] = 10;
                effects.triggers[side].parameters[1] = 25;
                if (mode == 2) {
                    effects.triggers[side].parameters[2] = 25;
                    effects.triggers[side].parameters[3] = 20;
                } else if (mode == 3) {
                    effects.triggers[side].parameters[2] = 20;
                }
            }
        }
        // Actuator diagnostic only: pressing a trigger does not identify a bow action.
        if (!SDL_SetGamepadBetopEffects(pad, &effects, (prearmed || held[0] || held[1]) ? 100 : 0)) return false;
        SDL_Delay(25);
    }
    return Stop(pad);
}

static bool ShoulderTest(SDL_Gamepad *pad, int side, bool direct)
{
    if (!Stop(pad)) return false;
    for (int pulse = 0; pulse < 3; ++pulse) {
        SDL_BetopGamepadEffects effects = {0};
        effects.shoulders[side].preset = direct ? 4 : 1;
        effects.shoulders[side].strength = direct ? 255 : 0;
        printf("%s shoulder: pulse %d/3 (%s)\n", side ? "RIGHT / RB" : "LEFT / LB", pulse + 1,
               direct ? "100 ms, observed vendor direct-drive ON" : "300 ms, low crisp preset");
        fflush(stdout);
        if (!SDL_SetGamepadBetopEffects(pad, &effects, direct ? 100 : 300)) return false;
        for (int tick = 0; tick < 80; ++tick) {
            SDL_PumpEvents();
            if (!SDL_GamepadConnected(pad)) return false;
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK)) {
                puts("Stopped by BACK / VIEW button.");
                return Stop(pad);
            }
            SDL_Delay(25);
        }
        if (!Stop(pad)) return false;
    }
    puts("Stage finished; stop requested.");
    fflush(stdout);
    return true;
}

static bool Sequence(SDL_Gamepad *pad)
{
    static const char *names[] = {"left grip", "right grip", "left shoulder", "right shoulder", "left trigger", "right trigger"};
    if (!Stop(pad)) {
        return false;
    }
    for (int step = 0; step < 6; ++step) {
        SDL_BetopGamepadEffects effects = {0};
        bool ok;
        printf("Pulse %d/6: %s (250 ms); then watchdog stop without event pumping\n", step + 1, names[step]);
        fflush(stdout);
        if (step < 2) {
            ok = SDL_RumbleGamepadFrequenciesEx(pad, step == 0 ? .25f : 0, 160, 0, 320,
                                                step == 1 ? .25f : 0, 160, 0, 320, 250);
        } else {
            int side = step & 1;
            if (step < 4) {
                effects.shoulders[side].preset = 1;
            } else {
                effects.triggers[side].mode = 2;
                effects.triggers[side].parameters[0] = 10;
                effects.triggers[side].parameters[1] = 20;
                effects.triggers[side].parameters[2] = 10;
                effects.triggers[side].parameters[3] = 5;
            }
            ok = SDL_SetGamepadBetopEffects(pad, &effects, 250);
        }
        if (!ok) {
            return false;
        }
        SDL_Delay(1000);
        // A second submission also reports any asynchronous transport failure.
        if (!Stop(pad)) {
            return false;
        }
        SDL_PumpEvents();
    }
    printf("All submissions accepted; confirm the six physical effects and automatic stops by touch.\n");
    return true;
}

int main(int argc, char **argv)
{
    SDL_JoystickID *ids;
    struct SDL_hid_device_info *devices, *device;
    int count = 0;
    int candidates = 0, result = 0;
    SDL_JoystickID selected = 0;
    bool sequence = argc == 2 && SDL_strcmp(argv[1], "--sequence") == 0;
    bool trigger_test = argc == 2 && SDL_strcmp(argv[1], "--trigger-test") == 0;
    int trigger_mode = 1, trigger_side = -1;
    if (argc == 2) {
        static const char *modes[] = {"--trigger-vibration-left", "--trigger-vibration-right", "--trigger-breakthrough-left", "--trigger-breakthrough-right"};
        for (int i = 0; i < 4; ++i) {
            if (SDL_strcmp(argv[1], modes[i]) == 0) {
                trigger_test = true;
                trigger_mode = i < 2 ? 2 : 3;
                trigger_side = i & 1;
            }
        }
    }
    bool shoulder_left = argc == 2 && SDL_strcmp(argv[1], "--shoulder-left") == 0;
    bool shoulder_right = argc == 2 && SDL_strcmp(argv[1], "--shoulder-right") == 0;
    bool shoulder_direct = argc == 2 && (SDL_strcmp(argv[1], "--shoulder-direct-left") == 0 || SDL_strcmp(argv[1], "--shoulder-direct-right") == 0);
    if (shoulder_direct) {
        shoulder_left = SDL_strcmp(argv[1], "--shoulder-direct-left") == 0;
        shoulder_right = !shoulder_left;
    }
    if (argc > 2 || (argc == 2 && !sequence && !trigger_test && !shoulder_left && !shoulder_right && SDL_strcmp(argv[1], "--list") != 0)) {
        fprintf(stderr, "Usage: testbetophaptics [--list | --sequence | --trigger-test | --shoulder-left | --shoulder-right | --shoulder-direct-left | --shoulder-direct-right]\n");
        return 2;
    }
    // The foreground window belongs to the console host, not SDL.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "%s\n", SDL_GetError());
        return 1;
    }
    for (int i = 0; i < 20; ++i) {
        SDL_PumpEvents();
        SDL_Delay(25);
    }
    ids = SDL_GetGamepads(&count);
    printf("SDL %d: gamepads=%d error=%s\n", SDL_GetVersion(), count, SDL_GetError());
    for (int i = 0; i < count; ++i) {
        SDL_Gamepad *pad = SDL_OpenGamepad(ids[i]);
        if (pad) {
            printf("gamepad %u: %s vendor=%04x product=%04x path=%s\n", ids[i],
                   SDL_GetGamepadName(pad), SDL_GetGamepadVendor(pad),
                   SDL_GetGamepadProduct(pad), SDL_GetGamepadPath(pad));
            if (SDL_GetGamepadVendor(pad) == 0x20bc &&
                (SDL_GetGamepadProduct(pad) == 0x5173 || SDL_GetGamepadProduct(pad) == 0x5174)) {
                ++candidates;
                selected = ids[i];
            }
            SDL_CloseGamepad(pad);
        }
    }
    SDL_free(ids);
    devices = SDL_hid_enumerate(0x20bc, 0);
    printf("Betop HID enumeration: %s (%s)\n", devices ? "found" : "empty", SDL_GetError());
    for (device = devices; device; device = device->next) {
        printf("hid %04x:%04x usage=%04x:%04x interface=%d path=%s\n",
               device->vendor_id, device->product_id, device->usage_page,
               device->usage, device->interface_number, device->path);
    }
    SDL_hid_free_enumeration(devices);
    if (sequence || trigger_test || shoulder_left || shoulder_right) {
        SDL_Gamepad *pad = candidates == 1 ? SDL_OpenGamepad(selected) : NULL;
        if (!pad) {
            fprintf(stderr, "Need exactly one connected KP70C gamepad (found %d).\n", candidates);
            result = 2;
        } else {
            if (!(trigger_test ? TriggerTest(pad, trigger_mode, trigger_side) : (shoulder_left || shoulder_right) ? ShoulderTest(pad, shoulder_right ? 1 : 0, shoulder_direct) : Sequence(pad))) {
                fprintf(stderr, "Haptics failed: %s\n", SDL_GetError());
                result = 1;
            }
            Stop(pad);
            SDL_Delay(50);
            SDL_CloseGamepad(pad);
        }
    }
    SDL_Quit();
    return result;
}
