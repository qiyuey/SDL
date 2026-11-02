/* Betop report and lease regression checks. Never opens or writes a device.
 * Build as a standalone program linked with SDL3, or add to the SDL test build.
 * SPDX-License-Identifier: Zlib
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#define SDL_BETOPHAPTICS_PROTOCOL_TEST 1
#include "../src/joystick/SDL_betophaptics.c"

static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "line %d failed: %s (%s)\n", __LINE__, #condition, SDL_GetError()); return 1; \
} } while (0)

static int test_grips(void)
{
    Uint8 packet[64], expected[64] = {0x02, 0x16}, before[64];
    static const float invalid[] = { NAN, INFINITY, -INFINITY, -0.01f, 1.01f };
    int side, bad;
    CHECK(SDL_BetopHapticsEncodeGrip(packet, 0, 0, 0, 0));
    CHECK(memcmp(packet, expected, sizeof(packet)) == 0);
    CHECK(SDL_BetopHapticsEncodeGrip(packet, 0.2f, 0.75f, 0.5f, 0.3f));
    expected[2] = 75; expected[3] = 50;
    CHECK(memcmp(packet, expected, sizeof(packet)) == 0);
    /* Side routing must not become the usual left-low/right-high motor mapping. */
    CHECK(SDL_BetopHapticsEncodeGrip(packet, 0, 1, 0, 0));
    expected[2] = 100; expected[3] = 0;
    CHECK(memcmp(packet, expected, sizeof(packet)) == 0);
    CHECK(SDL_BetopHapticsEncodeGrip(packet, 0, 0, 1, 0));
    expected[2] = 0; expected[3] = 100;
    CHECK(memcmp(packet, expected, sizeof(packet)) == 0);
    for (side = 0; side < 4; ++side) {
        for (bad = 0; bad < (int)SDL_arraysize(invalid); ++bad) {
            float amplitudes[4] = {0};
            amplitudes[side] = invalid[bad];
            memset(packet, 0xa5, sizeof(packet));
            memcpy(before, packet, sizeof(packet));
            CHECK(!SDL_BetopHapticsEncodeGrip(packet, amplitudes[0], amplitudes[1], amplitudes[2], amplitudes[3]));
            CHECK(memcmp(packet, before, sizeof(packet)) == 0);
        }
    }
    return 0;
}

static void expected_stops(Uint8 packets[4][64])
{
    memset(packets, 0, 4 * 64);
    packets[0][0] = packets[1][0] = packets[2][0] = packets[3][0] = 2;
    packets[0][1] = packets[1][1] = 0x56;
    packets[1][2] = 1;
    packets[2][1] = packets[3][1] = 0x76;
    packets[2][2] = packets[3][2] = 1;
    packets[2][3] = 4; packets[3][3] = 5;
}

static int test_effects(void)
{
    SDL_BetopGamepadEffects effects;
    Uint8 packets[4][64], expected[4][64], before[4][64];
    static const Uint8 meaningful[5] = {0, 2, 4, 3, 4};
    static const Uint8 shoulder_reports[9][6] = {
        {35,35,1,60,0,0}, {35,35,1,105,0,0}, {35,35,1,150,0,0},
        {25,30,3,25,15,15}, {30,30,3,40,25,20}, {30,40,3,55,30,30},
        {30,30,3,100,100,100}, {30,30,3,200,100,100}, {30,30,3,255,155,155}
    };
    int side, mode, i, fixture;
    memset(&effects, 0, sizeof(effects));
    expected_stops(expected);
    CHECK(SDL_BetopHapticsEncodeEffects(packets, &effects));
    CHECK(memcmp(packets, expected, sizeof(packets)) == 0);
    for (side = 0; side < 2; ++side) {
        for (mode = 1; mode <= 4; ++mode) {
            memset(&effects, 0, sizeof(effects));
            effects.triggers[side].mode = (Uint8)mode;
            expected_stops(expected);
            expected[side][3] = (Uint8)mode;
            for (i = 0; i < meaningful[mode]; ++i) {
                effects.triggers[side].parameters[i] = (Uint8)(10 * (i + 1));
                expected[side][4 + i] = (Uint8)(10 * (i + 1));
            }
            CHECK(SDL_BetopHapticsEncodeEffects(packets, &effects));
            CHECK(memcmp(packets, expected, sizeof(packets)) == 0);
        }
        for (fixture = 0; fixture < 9; ++fixture) {
            memset(&effects, 0, sizeof(effects));
            effects.shoulders[side].preset = (Uint8)(fixture / 3 + 1);
            effects.shoulders[side].strength = (Uint8)(fixture % 3);
            expected_stops(expected);
            expected[side + 2][3] = (Uint8)side;
            memcpy(expected[side + 2] + 4, shoulder_reports[fixture], 6);
            CHECK(SDL_BetopHapticsEncodeEffects(packets, &effects));
            CHECK(memcmp(packets, expected, sizeof(packets)) == 0);
        }
    }
    /* An error on the second actuator must not leave a partially built batch. */
    for (side = 0; side < 2; ++side) {
        for (mode = 0; mode <= 4; ++mode) {
            for (i = 0; i < 6; ++i) {
                memset(&effects, 0, sizeof(effects));
                effects.triggers[side].mode = (Uint8)mode;
                effects.triggers[side].parameters[i] = i < meaningful[mode] ? 101 : 1;
                memset(packets, 0xa5, sizeof(packets));
                memcpy(before, packets, sizeof(packets));
                CHECK(!SDL_BetopHapticsEncodeEffects(packets, &effects));
                CHECK(memcmp(packets, before, sizeof(packets)) == 0);
            }
        }
        for (i = 0; i < 3; ++i) {
            memset(&effects, 0, sizeof(effects));
            if (i == 0) effects.triggers[side].mode = 5;
            if (i == 1) effects.shoulders[side].preset = 5;
            if (i == 2) effects.shoulders[side].strength = 3;
            memset(packets, 0xa5, sizeof(packets));
            memcpy(before, packets, sizeof(packets));
            CHECK(!SDL_BetopHapticsEncodeEffects(packets, &effects));
            CHECK(memcmp(packets, before, sizeof(packets)) == 0);
        }
    }
    for (side = 0; side < 2; ++side) {
        memset(&effects, 0, sizeof(effects));
        effects.shoulders[side].preset = 4;
        effects.shoulders[side].strength = 255;
        expected_stops(expected);
        expected[side + 2][4] = 255;
        CHECK(SDL_BetopHapticsEncodeEffects(packets, &effects));
        CHECK(memcmp(packets, expected, sizeof(packets)) == 0);
        effects.shoulders[side].strength = 0;
        expected_stops(expected);
        CHECK(SDL_BetopHapticsEncodeEffects(packets, &effects));
        CHECK(memcmp(packets, expected, sizeof(packets)) == 0);
        for (i = 1; i < 255; ++i) {
            effects.shoulders[side].strength = (Uint8)i;
            memcpy(before, packets, sizeof(packets));
            CHECK(!SDL_BetopHapticsEncodeEffects(packets, &effects));
            CHECK(memcmp(packets, before, sizeof(packets)) == 0);
        }
    }
    memset(packets, 0xa5, sizeof(packets));
    memcpy(before, packets, sizeof(packets));
    CHECK(!SDL_BetopHapticsEncodeEffects(packets, NULL));
    CHECK(memcmp(packets, before, sizeof(packets)) == 0);
    return 0;
}

static int test_leases(void)
{
    Uint8 previous[64] = {2, 0x56, 0, 3, 10, 25, 20};
    Uint8 next[64];
    memcpy(next, previous, sizeof(next));
    CHECK(!BetopEffectNeedsSubmission(previous, next, 2099, 100));
    CHECK(BetopEffectNeedsSubmission(previous, next, 2100, 100));
    CHECK(!BetopEffectNeedsSubmission(previous, next, 2101, 2100));
    next[3] = 0;
    CHECK(BetopEffectNeedsSubmission(previous, next, 101, 100));
    memcpy(previous, next, sizeof(next));
    CHECK(!BetopEffectNeedsSubmission(previous, next, 10000, 100));
    next[1] = 0x76; next[2] = 1; next[3] = 4; next[4] = 255;
    memcpy(previous, next, sizeof(next));
    CHECK(BetopEffectNeedsSubmission(previous, next, 2100, 100));
    next[4] = 0;
    CHECK(BetopEffectNeedsSubmission(previous, next, 101, 100));
    memcpy(previous, next, sizeof(next));
    CHECK(!BetopEffectNeedsSubmission(previous, next, 2100, 100));
    next[3] = 0; next[4] = 35;
    memcpy(previous, next, sizeof(next));
    CHECK(!BetopEffectNeedsSubmission(previous, next, 2100, 100));
    Uint64 deadline = 123;
    CHECK(BetopLeaseDeadline(50, false, 0, true, &deadline) && deadline == 0);
    CHECK(BetopLeaseDeadline(50, true, 1, true, &deadline) && deadline == 51);
    CHECK(BetopLeaseDeadline(50, true, 1000, true, &deadline) && deadline == 1050);
    CHECK(!BetopLeaseDeadline(50, true, 0, true, &deadline) && deadline == 1050);
    CHECK(!BetopLeaseDeadline(50, true, 1001, true, &deadline) && deadline == 1050);
    CHECK(BetopLeaseDeadline(50, true, 0, false, &deadline) && deadline == 1050);
    CHECK(BetopLeaseDeadline(50, true, SDL_MAX_UINT32, false, &deadline) && deadline == 1050);
    CHECK(BetopLeaseDeadline(100, true, 100, false, &deadline) && deadline == 200);
    CHECK(BetopLeaseDeadline(150, true, 100, false, &deadline) && deadline == 250);
    CHECK(BetopLeaseDeadline(151, false, 0, false, &deadline) && deadline == 0);
    CHECK(BetopLeaseDeadline(SDL_MAX_UINT64 - 2, true, 10, true, &deadline) && deadline == SDL_MAX_UINT64);
    return 0;
}

int main(void)
{
    if (test_grips() || test_effects() || test_leases()) return 1;
    printf("Betop protocol: %u checks passed; grip/trigger/shoulder routing, neutral reports, invalid inputs, atomic encoding and finite leases. No device I/O.\n", checks);
    return 0;
}
