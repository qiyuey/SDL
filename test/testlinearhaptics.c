/*
 * Distinguishable two-channel Switch HD Rumble test.
 *
 * The test deliberately uses one public API: SDL_RumbleGamepadFrequenciesEx().
 * Keep the controller in hand: each mode isolates a motor or a frequency band.
 */
#include <SDL3/SDL.h>

#include <stdio.h>

typedef struct RumbleTestState
{
    SDL_Gamepad *gamepad;
    SDL_Renderer *renderer;
    Uint64 stop_at;
    int mode;
    int step;
} RumbleTestState;

static const char *mode_names[] = {
    "Stopped", "Left low", "Right low", "Left high", "Right high",
    "Both channels", "Alternating", "Frequency sweep", "Sequence"
};

static void DrawDigit(SDL_Renderer *renderer, int digit, float x, float y, float scale)
{
    static const Uint8 glyphs[10][15] = {
        {1,1,1, 1,0,1, 1,0,1, 1,0,1, 1,1,1},
        {0,1,0, 1,1,0, 0,1,0, 0,1,0, 1,1,1},
        {1,1,1, 0,0,1, 1,1,1, 1,0,0, 1,1,1},
        {1,1,1, 0,0,1, 1,1,1, 0,0,1, 1,1,1},
        {1,0,1, 1,0,1, 1,1,1, 0,0,1, 0,0,1},
        {1,1,1, 1,0,0, 1,1,1, 0,0,1, 1,1,1},
        {1,1,1, 1,0,0, 1,1,1, 1,0,1, 1,1,1},
        {1,1,1, 0,0,1, 0,1,0, 0,1,0, 0,1,0},
        {1,1,1, 1,0,1, 1,1,1, 1,0,1, 1,1,1},
        {1,1,1, 1,0,1, 1,1,1, 0,0,1, 1,1,1}
    };
    for (int row = 0; row < 5; ++row) {
        for (int col = 0; col < 3; ++col) {
            if (glyphs[digit][row * 3 + col]) {
                SDL_FRect r = {x + col * scale, y + row * scale, scale - 1, scale - 1};
                SDL_RenderFillRect(renderer, &r);
            }
        }
    }
}

static void DrawWave(SDL_Renderer *renderer, const RumbleTestState *state, float x, float y, float w, float h, int right)
{
    float amp_low = 0, amp_high = 0, freq_low = 160, freq_high = 320;
    if (state->mode == 1 && !right) amp_low = 1;
    if (state->mode == 2 && right) amp_low = 1;
    if (state->mode == 3 && !right) amp_high = 1;
    if (state->mode == 4 && right) amp_high = 1;
    if (state->mode == 5 || state->mode == 8) amp_low = amp_high = 0.8f;
    if (state->mode == 6) amp_low = ((state->step & 1) == (right ? 1 : 0)) ? 1.0f : 0.0f;
    if (state->mode == 7) {
        static const float sweep[] = {80, 120, 160, 240, 320, 480};
        const int i = state->step > 5 ? 5 : state->step;
        freq_low = sweep[i]; freq_high = sweep[i] * 2.0f;
        amp_low = amp_high = 0.7f;
    }
    SDL_SetRenderDrawColor(renderer, 75, 75, 85, 255);
    SDL_RenderLine(renderer, (int)x, (int)(y + h * 0.5f), (int)(x + w), (int)(y + h * 0.5f));
    for (int band = 0; band < 2; ++band) {
        const float amp = band ? amp_high : amp_low;
        const float freq = band ? freq_high : freq_low;
        const Uint8 r = band ? 255 : 70;
        const Uint8 g = band ? 100 : 210;
        const Uint8 b = band ? 70 : 255;
        SDL_SetRenderDrawColor(renderer, r, g, b, 255);
        int last_y = (int)(y + h * 0.5f);
        for (int px = 0; px < (int)w; ++px) {
            const float t = (float)px / w;
            const float cycles = freq / 80.0f;
            const float value = amp * SDL_sinf((t * cycles + (float)(SDL_GetTicks() % 1000) / 1000.0f) * 2.0f * SDL_PI_F);
            const int py = (int)(y + h * 0.5f - value * h * 0.38f);
            SDL_RenderLine(renderer, (int)x + px - 1, last_y, (int)x + px, py);
            last_y = py;
        }
    }
}

static void DrawUI(const RumbleTestState *state)
{
    SDL_Renderer *renderer = state->renderer;
    SDL_SetRenderDrawColor(renderer, 18, 18, 24, 255);
    SDL_RenderClear(renderer);
    for (int i = 1; i <= 8; ++i) {
        const float x = 18.0f + (i - 1) * 86.0f;
        const bool active = state->mode == i || (state->mode == 8 && i == state->step);
        SDL_SetRenderDrawColor(renderer, active ? 230 : 60, active ? 150 : 65, active ? 60 : 80, 255);
        SDL_FRect button = {x, 18, 70, 52};
        SDL_RenderFillRect(renderer, &button);
        SDL_SetRenderDrawColor(renderer, 240, 240, 240, 255);
        DrawDigit(renderer, i, x + 25, 29, 8);
    }
    SDL_SetRenderDrawColor(renderer, 230, 230, 230, 255);
    SDL_FRect left_label = {20, 95, 12, 12};
    SDL_RenderFillRect(renderer, &left_label);
    SDL_SetRenderDrawColor(renderer, 80, 210, 255, 255);
    SDL_FRect right_label = {20, 225, 12, 12};
    SDL_RenderFillRect(renderer, &right_label);
    DrawWave(renderer, state, 45, 88, 650, 105, 0);
    DrawWave(renderer, state, 45, 218, 650, 105, 1);
    SDL_RenderPresent(renderer);
}

static void StopRumble(RumbleTestState *state)
{
    if (state->gamepad) {
        SDL_RumbleGamepadFrequenciesEx(state->gamepad,
                                       0.0f, 160.0f, 0.0f, 320.0f,
                                       0.0f, 160.0f, 0.0f, 320.0f, 0);
    }
    state->stop_at = 0;
    state->mode = 0;
}

static void SendRumble(RumbleTestState *state,
                       float ll_amp, float ll_freq, float lh_amp, float lh_freq,
                       float rl_amp, float rl_freq, float rh_amp, float rh_freq,
                       Uint32 duration)
{
    if (!state->gamepad) {
        return;
    }
    if (!SDL_RumbleGamepadFrequenciesEx(state->gamepad,
                                        ll_amp, ll_freq, lh_amp, lh_freq,
                                        rl_amp, rl_freq, rh_amp, rh_freq,
                                        duration)) {
        SDL_Log("Rumble failed: %s", SDL_GetError());
    }
    state->stop_at = SDL_GetTicks() + duration;
}

static void StartMode(RumbleTestState *state, int mode)
{
    StopRumble(state);
    state->mode = mode;
    state->step = 0;
    switch (mode) {
    case 1: /* left motor, low band */
        SendRumble(state, 1.0f, 160.0f, 0.0f, 320.0f, 0.0f, 160.0f, 0.0f, 320.0f, 900);
        break;
    case 2: /* right motor, low band */
        SendRumble(state, 0.0f, 160.0f, 0.0f, 320.0f, 1.0f, 160.0f, 0.0f, 320.0f, 900);
        break;
    case 3: /* left motor, high band */
        SendRumble(state, 0.0f, 160.0f, 1.0f, 320.0f, 0.0f, 160.0f, 0.0f, 320.0f, 900);
        break;
    case 4: /* right motor, high band */
        SendRumble(state, 0.0f, 160.0f, 0.0f, 320.0f, 0.0f, 160.0f, 1.0f, 320.0f, 900);
        break;
    case 5: /* both motors, same waveform */
        SendRumble(state, 0.8f, 160.0f, 0.8f, 320.0f, 0.8f, 160.0f, 0.8f, 320.0f, 900);
        break;
    case 6: /* alternating left/right pulses */
        SendRumble(state, 1.0f, 160.0f, 0.0f, 320.0f, 0.0f, 160.0f, 0.0f, 320.0f, 220);
        break;
    case 7: /* frequency sweep on both motors */
        SendRumble(state, 0.7f, 80.0f, 0.7f, 160.0f, 0.7f, 80.0f, 0.7f, 160.0f, 250);
        break;
    case 8: /* automatic identification sequence */
        StartMode(state, 1);
        state->mode = 8;
        break;
    default:
        StopRumble(state);
        break;
    }
}

static void TickMode(RumbleTestState *state)
{
    const Uint64 now = SDL_GetTicks();
    if (!state->mode || now < state->stop_at) {
        return;
    }
    if (state->mode == 6) {
        ++state->step;
        if (state->step >= 8) {
            StopRumble(state);
        } else if (state->step & 1) {
            SendRumble(state, 0.0f, 160.0f, 0.0f, 320.0f, 1.0f, 160.0f, 0.0f, 320.0f, 220);
        } else {
            SendRumble(state, 1.0f, 160.0f, 0.0f, 320.0f, 0.0f, 160.0f, 0.0f, 320.0f, 220);
        }
    } else if (state->mode == 7) {
        static const float low[] = {80.0f, 120.0f, 160.0f, 240.0f, 320.0f, 480.0f};
        if (state->step >= 5) {
            StopRumble(state);
        } else {
            ++state->step;
            SendRumble(state, 0.7f, low[state->step], 0.7f, low[state->step] * 2.0f,
                       0.7f, low[state->step], 0.7f, low[state->step] * 2.0f, 250);
        }
    } else if (state->mode == 8) {
        ++state->step;
        if (state->step > 7) {
            StopRumble(state);
        } else {
            StartMode(state, state->step);
            state->mode = 8;
        }
    } else {
        StopRumble(state);
    }
}

int main(int argc, char **argv)
{
    RumbleTestState state = {0};
    SDL_Window *window = NULL;
    int count = 0;
    SDL_JoystickID *ids;
    SDL_Event event;

    if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    ids = SDL_GetGamepads(&count);
    if (count < 1 || !ids) {
        SDL_Log("No gamepad found");
        SDL_Quit();
        return 2;
    }
    state.gamepad = SDL_OpenGamepad(ids[0]);
    SDL_Log("Testing: %s", state.gamepad ? SDL_GetGamepadName(state.gamepad) : "(open failed)");
    SDL_free(ids);
    if (!state.gamepad) {
        SDL_Quit();
        return 3;
    }

    window = SDL_CreateWindow("SDL3 Dual-Channel Linear Rumble", 720, 420, 0);
    if (!window) {
        SDL_Log("Window creation failed: %s", SDL_GetError());
        SDL_CloseGamepad(state.gamepad);
        SDL_Quit();
        return 4;
    }
    state.renderer = SDL_CreateRenderer(window, NULL);
    if (!state.renderer) {
        SDL_Log("Renderer creation failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_CloseGamepad(state.gamepad);
        SDL_Quit();
        return 5;
    }

    SDL_Log("1 left-low  2 right-low  3 left-high  4 right-high  5 both  6 alternate  7 sweep  8 sequence  Space stop  Esc quit");
    if (argc > 2 && SDL_strcmp(argv[1], "--mode") == 0) {
        const int mode = SDL_atoi(argv[2]);
        if (mode >= 1 && mode <= 8) {
            StartMode(&state, mode);
        }
    }
    for (;;) {
        if (SDL_WaitEventTimeout(&event, 10)) {
            if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
                break;
            }
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT &&
                event.button.y >= 18 && event.button.y < 70) {
                const int mode = (int)(event.button.x - 18) / 86 + 1;
                if (mode >= 1 && mode <= 8 && event.button.x >= 18 + (mode - 1) * 86 &&
                    event.button.x < 18 + (mode - 1) * 86 + 70) {
                    StartMode(&state, mode);
                }
            }
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                if (event.key.key >= SDLK_1 && event.key.key <= SDLK_8) {
                    StartMode(&state, (int)(event.key.key - SDLK_0));
                } else if (event.key.key == SDLK_SPACE) {
                    StopRumble(&state);
                }
            }
        }
        TickMode(&state);
        DrawUI(&state);
    }
    StopRumble(&state);
    SDL_DestroyRenderer(state.renderer);
    SDL_DestroyWindow(window);
    SDL_CloseGamepad(state.gamepad);
    SDL_Quit();
    return 0;
}
