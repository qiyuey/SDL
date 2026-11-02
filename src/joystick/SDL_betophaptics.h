/* Local SDL extension: Betop KP70C companion HID haptics. SPDX-License-Identifier: Zlib */
#ifndef SDL_betophaptics_h_
#define SDL_betophaptics_h_

#include "SDL_sysjoystick.h"

typedef struct SDL_BetopHapticsContext SDL_BetopHapticsContext;

extern bool SDL_BetopHapticsIsSupported(SDL_Joystick *joystick);
extern SDL_BetopHapticsContext *SDL_BetopHapticsOpen(SDL_Joystick *joystick);
extern bool SDL_BetopHapticsRumble(SDL_BetopHapticsContext *ctx,
                                 float left_low, float left_high,
                                 float right_low, float right_high, Uint32 duration_ms);
extern bool SDL_BetopHapticsSetEffects(SDL_BetopHapticsContext *ctx,
                                     const SDL_BetopGamepadEffects *effects, Uint32 duration_ms);
extern void SDL_BetopHapticsStop(SDL_BetopHapticsContext *ctx);
extern void SDL_BetopHapticsClose(SDL_BetopHapticsContext *ctx);

/* Pure packet builders are kept separate for transport-free regression tests. */
extern bool SDL_BetopHapticsEncodeGrip(Uint8 packet[64], float left_low, float left_high,
                                     float right_low, float right_high);
extern bool SDL_BetopHapticsEncodeEffects(Uint8 packets[4][64], const SDL_BetopGamepadEffects *effects);

#endif
