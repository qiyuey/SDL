/*
 * Local SDL extension: Betop KP70C companion HID haptics.
 * SPDX-License-Identifier: Zlib
 *
 * Independently implemented from observed report layouts, not vendor source.
 * The normal joystick still supplies input. This companion opens only the
 * vendor interface in the SAME Windows device container. It never changes
 * persistent configuration, firmware, LEDs, or controller mode.
 */
#ifdef SDL_BETOPHAPTICS_PROTOCOL_TEST
#include <SDL3/SDL.h>
#else
#include "SDL_internal.h"
#include "SDL_betophaptics.h"
#endif

#define BETOP_LEASE_MAX_MS 1000

/* The vendor queue refreshes unchanged reports every two seconds. A host
   lease renewal alone does not send anything to the controller. */
static bool BetopEffectNeedsSubmission(const Uint8 previous[64], const Uint8 next[64], Uint64 now, Uint64 queued_at)
{
    bool sustained = (next[1] == 0x56 && next[3] != 0) ||
                     (next[1] == 0x76 && (next[3] == 4 || next[3] == 5) && next[4] != 0);
    return SDL_memcmp(previous, next, 64) != 0 ||
           (sustained && now >= queued_at && now - queued_at >= 2000);
}

#if (defined(SDL_PLATFORM_WIN32) && defined(SDL_JOYSTICK_HIDAPI)) || defined(SDL_BETOPHAPTICS_PROTOCOL_TEST)
static bool BetopLeaseDeadline(Uint64 now, bool active, Uint32 duration_ms, bool strict, Uint64 *deadline)
{
    Uint32 lease;
    if (!active) {
        *deadline = 0;
        return true;
    }
    if (strict && (!duration_ms || duration_ms > BETOP_LEASE_MAX_MS)) {
        return SDL_SetError("Active Betop effects require a lease from 1 to 1000 ms");
    }
    lease = duration_ms ? SDL_min(duration_ms, BETOP_LEASE_MAX_MS) : BETOP_LEASE_MAX_MS;
    *deadline = now > SDL_MAX_UINT64 - lease ? SDL_MAX_UINT64 : now + lease;
    return true;
}
#endif

bool SDL_BetopHapticsEncodeGrip(Uint8 packet[64], float left_low, float left_high,
                              float right_low, float right_high)
{
    if (!(left_low >= 0 && left_low <= 1) || !(left_high >= 0 && left_high <= 1) ||
        !(right_low >= 0 && right_low <= 1) || !(right_high >= 0 && right_high <= 1)) {
        return SDL_SetError("Betop rumble amplitudes must be finite and between 0 and 1");
    }
    SDL_memset(packet, 0, 64);
    packet[0] = 0x02;
    packet[1] = 0x16;
    /* Preserve physical left/right envelopes. This device command has no Hz
       fields; it cannot reproduce Switch dual-band frequencies losslessly. */
    packet[2] = (Uint8)SDL_roundf(SDL_max(left_low, left_high) * 100.0f);
    packet[3] = (Uint8)SDL_roundf(SDL_max(right_low, right_high) * 100.0f);
    return true;
}

bool SDL_BetopHapticsEncodeEffects(Uint8 packets[4][64], const SDL_BetopGamepadEffects *effects)
{
    static const Uint8 shoulder_steps[3][3][6] = {
        { {35,35,1,60,0,0}, {35,35,1,105,0,0}, {35,35,1,150,0,0} },
        { {25,30,3,25,15,15}, {30,30,3,40,25,20}, {30,40,3,55,30,30} },
        { {30,30,3,100,100,100}, {30,30,3,200,100,100}, {30,30,3,255,155,155} }
    };
    /* Number of meaningful bytes after mode. The remaining report bytes are
       reserved and must be zero, including the vendor's optional extra fields. */
    static const Uint8 trigger_count[5] = { 0, 2, 4, 3, 4 };
    int side, i;

    if (!effects) {
        return SDL_InvalidParamError("effects");
    }
    for (side = 0; side < 2; ++side) {
        const SDL_BetopTriggerEffect *trigger = &effects->triggers[side];
        const SDL_BetopShoulderEffect *shoulder = &effects->shoulders[side];
        if (trigger->mode > 4 || shoulder->preset > 4 ||
            (shoulder->preset == 4 ? (shoulder->strength != 0 && shoulder->strength != 255) : shoulder->strength > 2)) {
            return SDL_SetError("Invalid Betop trigger mode or shoulder preset/strength");
        }
        for (i = 0; i < 6; ++i) {
            if (trigger->parameters[i] > 100 ||
                (i >= trigger_count[trigger->mode] && trigger->parameters[i] != 0)) {
                return SDL_SetError("Invalid or reserved Betop trigger parameter");
            }
        }
    }
    SDL_memset(packets, 0, 4 * 64);
    for (side = 0; side < 2; ++side) {
        Uint8 *trigger_packet = packets[side];
        Uint8 *shoulder_packet = packets[2 + side];
        const SDL_BetopTriggerEffect *trigger = &effects->triggers[side];
        const SDL_BetopShoulderEffect *shoulder = &effects->shoulders[side];
        trigger_packet[0] = 0x02;
        trigger_packet[1] = 0x56;
        trigger_packet[2] = (Uint8)side;
        trigger_packet[3] = trigger->mode;
        SDL_memcpy(trigger_packet + 4, trigger->parameters, 6);
        shoulder_packet[0] = 0x02;
        shoulder_packet[1] = 0x76;
        shoulder_packet[2] = 0x01;
        shoulder_packet[3] = (Uint8)((shoulder->preset && shoulder->preset != 4) ? side : side + 4);
        if (shoulder->preset == 4) {
            /* Vendor direct-drive builder and Witcher 3 call site use 255/0.
               Do not assume the intermediate byte values are linear amplitude. */
            shoulder_packet[4] = shoulder->strength;
        } else if (shoulder->preset) {
            SDL_memcpy(shoulder_packet + 4, shoulder_steps[shoulder->preset - 1][shoulder->strength], 6);
        }
    }
    return true;
}

#if defined(SDL_PLATFORM_WIN32) && defined(SDL_JOYSTICK_HIDAPI) && !defined(SDL_BETOPHAPTICS_PROTOCOL_TEST)

#include "../core/windows/SDL_windows.h"
#include "../core/windows/SDL_xinput.h"
#include <cfgmgr32.h>
#include <devpropdef.h>

#define BETOP_REPORT_COUNT 5

struct SDL_BetopHapticsContext
{
    SDL_hid_device *device;
    SDL_Mutex *lock;
    SDL_Condition *wake;
    SDL_Thread *thread;
    HANDLE owner;
    bool quit;
    bool failed;
    bool used;
    bool dirty[BETOP_REPORT_COUNT];
    Uint8 reports[BETOP_REPORT_COUNT][64];
    Uint64 grip_deadline;
    Uint64 effect_deadline;
    Uint64 effect_queued_at[4];
};

typedef CONFIGRET (WINAPI *BetopInterfaceProperty)(LPCWSTR, const DEVPROPKEY *, DEVPROPTYPE *, PBYTE, PULONG, ULONG);
typedef CONFIGRET (WINAPI *BetopLocateNode)(PDEVINST, DEVINSTID_W, ULONG);
typedef CONFIGRET (WINAPI *BetopNodeProperty)(DEVINST, const DEVPROPKEY *, DEVPROPTYPE *, PBYTE, PULONG, ULONG);

static bool BetopContainerId(const char *path, GUID *container)
{
    static const DEVPROPKEY instance_key = { {0x78c34fc8,0x104a,0x4aca,{0x9e,0xa4,0x52,0x4d,0x52,0x99,0x6e,0x57}},256 };
    static const DEVPROPKEY container_key = { {0x8c7ed206,0x3f8a,0x4827,{0xb3,0xab,0xae,0x9e,0x1f,0xae,0xfc,0x6c}},2 };
    static const GUID empty = {0};
    HMODULE module;
    BetopInterfaceProperty interface_property;
    BetopLocateNode locate;
    BetopNodeProperty node_property;
    WCHAR *wide_path, instance[1024];
    DEVINST node;
    DEVPROPTYPE type;
    ULONG size;
    bool result = false;

    if (!path || !*path) {
        return false;
    }
    module = LoadLibraryW(L"cfgmgr32.dll");
    if (!module) {
        return false;
    }
    interface_property = (BetopInterfaceProperty)(void *)GetProcAddress(module, "CM_Get_Device_Interface_PropertyW");
    locate = (BetopLocateNode)(void *)GetProcAddress(module, "CM_Locate_DevNodeW");
    node_property = (BetopNodeProperty)(void *)GetProcAddress(module, "CM_Get_DevNode_PropertyW");
    wide_path = WIN_UTF8ToStringW(path);
    size = sizeof(instance);
    if (wide_path && interface_property && locate && node_property &&
        interface_property(wide_path, &instance_key, &type, (PBYTE)instance, &size, 0) == CR_SUCCESS &&
        type == DEVPROP_TYPE_STRING && size >= sizeof(WCHAR) && size <= sizeof(instance)) {
        instance[SDL_arraysize(instance) - 1] = L'\0';
        if (locate(&node, instance, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
            size = sizeof(*container);
            result = node_property(node, &container_key, &type, (PBYTE)container, &size, 0) == CR_SUCCESS &&
                     type == DEVPROP_TYPE_GUID && size == sizeof(*container) &&
                     SDL_memcmp(container, &empty, sizeof(empty)) != 0;
        }
    }
    SDL_free(wide_path);
    FreeLibrary(module);
    return result;
}

/* XInput itself exposes only a slot, not a PnP interface path. Do not guess
   a slot's physical controller when another identical controller is present.
   SDL has already loaded these functions for the active XInput input driver. */
static bool BetopUniqueXInputSlot(const char *path, Uint16 vendor, Uint16 product)
{
    int selected, index, matches = 0, matched_index = -1;
    if (!path || SDL_strlen(path) != 8 || SDL_strncmp(path, "XInput#", 7) != 0 ||
        path[7] < '0' || path[7] >= '0' + XUSER_MAX_COUNT ||
        !XINPUTGETSTATE || !XINPUTGETCAPABILITIESEX) {
        return false;
    }
    selected = path[7] - '0';
    for (index = 0; index < XUSER_MAX_COUNT; ++index) {
        XINPUT_STATE state;
        SDL_XINPUT_CAPABILITIES_EX capabilities;
        SDL_zero(state);
        SDL_zero(capabilities);
        if (XINPUTGETSTATE((DWORD)index, &state) == ERROR_SUCCESS &&
            XINPUTGETCAPABILITIESEX(1, (DWORD)index, 0, &capabilities) == ERROR_SUCCESS &&
            capabilities.VendorId == vendor && capabilities.ProductId == product) {
            ++matches;
            matched_index = index;
        }
    }
    return matches == 1 && matched_index == selected;
}

bool SDL_BetopHapticsIsSupported(SDL_Joystick *joystick)
{
    Uint16 vendor, product;
    if (!joystick || joystick->is_virtual) {
        return false;
    }
    SDL_GetJoystickGUIDInfo(joystick->guid, &vendor, &product, NULL, NULL);
    return vendor == 0x20bc && (product == 0x5173 || product == 0x5174);
}

static void BetopClearGrip(SDL_BetopHapticsContext *ctx)
{
    SDL_BetopHapticsEncodeGrip(ctx->reports[0], 0, 0, 0, 0);
    ctx->dirty[0] = true;
    ctx->grip_deadline = 0;
}

static void BetopClearEffects(SDL_BetopHapticsContext *ctx)
{
    SDL_BetopGamepadEffects stopped;
    int i;
    SDL_zero(stopped);
    SDL_BetopHapticsEncodeEffects(&ctx->reports[1], &stopped);
    for (i = 1; i < BETOP_REPORT_COUNT; ++i) {
        ctx->dirty[i] = true;
    }
    ctx->effect_deadline = 0;
}

static int SDLCALL BetopWorker(void *userdata)
{
    SDL_BetopHapticsContext *ctx = userdata;
    for (;;) {
        Uint8 reports[BETOP_REPORT_COUNT][64];
        bool dirty[BETOP_REPORT_COUNT], quit, any = false;
        Uint64 now;
        int i;
        SDL_LockMutex(ctx->lock);
        now = SDL_GetTicks();
        if (ctx->grip_deadline && now >= ctx->grip_deadline) {
            BetopClearGrip(ctx);
        }
        if (ctx->effect_deadline && now >= ctx->effect_deadline) {
            BetopClearEffects(ctx);
        }
        quit = ctx->quit;
        for (i = 0; i < BETOP_REPORT_COUNT; ++i) {
            dirty[i] = ctx->dirty[i];
            any |= dirty[i];
            ctx->dirty[i] = false;
        }
        SDL_memcpy(reports, ctx->reports, sizeof(reports));
        if (!any && !quit) {
            SDL_WaitConditionTimeout(ctx->wake, ctx->lock, 10);
        }
        SDL_UnlockMutex(ctx->lock);
        for (i = 0; i < BETOP_REPORT_COUNT; ++i) {
            if (dirty[i] && SDL_hid_write(ctx->device, reports[i], sizeof(reports[i])) != sizeof(reports[i])) {
                int stop_index;
                SDL_LockMutex(ctx->lock);
                ctx->failed = true;
                BetopClearGrip(ctx);
                BetopClearEffects(ctx);
                SDL_memcpy(reports, ctx->reports, sizeof(reports));
                for (stop_index = 0; stop_index < BETOP_REPORT_COUNT; ++stop_index) {
                    ctx->dirty[stop_index] = false;
                }
                SDL_UnlockMutex(ctx->lock);
                /* Do not transmit any remaining active snapshots after a failure.
                   Try every neutral report once, even if one cannot be delivered;
                   this also applies when failure happens during shutdown. */
                for (stop_index = 0; stop_index < BETOP_REPORT_COUNT; ++stop_index) {
                    SDL_hid_write(ctx->device, reports[stop_index], sizeof(reports[stop_index]));
                }
                break;
            }
        }
        if (quit) {
            return 0;
        }
    }
}

SDL_BetopHapticsContext *SDL_BetopHapticsOpen(SDL_Joystick *joystick)
{
    SDL_BetopHapticsContext *ctx = NULL;
    SDL_hid_device_info *devices, *entry;
    GUID target, candidate;
    Uint16 vendor, product;
    char *path = NULL;
    int matches = 0;
    bool synthetic_path;
    bool container_failed = false;
    WCHAR owner_name[100];

    if (!SDL_BetopHapticsIsSupported(joystick)) {
        SDL_Unsupported();
        return NULL;
    }
    SDL_GetJoystickGUIDInfo(joystick->guid, &vendor, &product, NULL, NULL);
    synthetic_path = joystick->path && SDL_strncmp(joystick->path, "XInput#", 7) == 0;
    if (synthetic_path) {
        if (!BetopUniqueXInputSlot(joystick->path, vendor, product)) {
            SDL_SetError("Betop XInput slot is disconnected, unidentifiable or ambiguous");
            return NULL;
        }
    } else if (!BetopContainerId(joystick->path, &target)) {
        SDL_SetError("Cannot identify Betop gamepad physical device container");
        return NULL;
    }
    if (SDL_hid_init() < 0) {
        return NULL;
    }
    devices = SDL_hid_enumerate(vendor, product);
    for (entry = devices; entry; entry = entry->next) {
        if ((entry->usage_page == 0xff00 || entry->usage_page == 0x00ff || entry->usage_page == 0xff80) &&
            (entry->usage == 1 || entry->usage == 3)) {
            bool identified = BetopContainerId(entry->path, &candidate);
            if (synthetic_path) {
                /* Count every candidate, even one whose container lookup failed:
                   an unresolved second device must not make the first look unique. */
                ++matches;
                container_failed |= !identified;
            } else if (identified && SDL_memcmp(&target, &candidate, sizeof(target)) == 0) {
                ++matches;
            } else {
                continue;
            }
            if (!path && identified) {
                path = SDL_strdup(entry->path);
                if (synthetic_path) {
                    target = candidate;
                }
            }
        }
    }
    SDL_hid_free_enumeration(devices);
    if (matches != 1 || !path || container_failed) {
        SDL_SetError("Betop companion HID interface is missing or ambiguous (%d matches)", matches);
        goto failed;
    }
    if (synthetic_path && !BetopUniqueXInputSlot(joystick->path, vendor, product)) {
        SDL_SetError("Betop XInput identity changed during companion discovery");
        goto failed;
    }
    ctx = SDL_calloc(1, sizeof(*ctx));
    if (!ctx) {
        goto failed;
    }
    SDL_swprintf(owner_name, SDL_arraysize(owner_name),
                 L"Local\\SDL-Betop-%08lx-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x",
                 target.Data1, target.Data2, target.Data3,
                 target.Data4[0], target.Data4[1], target.Data4[2], target.Data4[3],
                 target.Data4[4], target.Data4[5], target.Data4[6], target.Data4[7]);
    /* Handle lifetime, not thread ownership: this also permits closing a
       joystick on a different thread. Other SDL processes cooperate with this
       claim; vendor applications do not, so callers must avoid concurrent use. */
    ctx->owner = CreateEventW(NULL, TRUE, FALSE, owner_name);
    if (!ctx->owner || GetLastError() == ERROR_ALREADY_EXISTS) {
        SDL_SetError("Betop haptics is already owned by another SDL gamepad/process");
        goto failed;
    }
    ctx->device = SDL_hid_open_path(path);
    ctx->lock = SDL_CreateMutex();
    ctx->wake = SDL_CreateCondition();
    if (!ctx->device || !ctx->lock || !ctx->wake) {
        goto failed;
    }
    ctx->thread = SDL_CreateThread(BetopWorker, "Betop haptics", ctx);
    if (!ctx->thread) {
        goto failed;
    }
    SDL_free(path);
    return ctx;

failed:
    SDL_free(path);
    if (ctx) {
        if (ctx->device) SDL_hid_close(ctx->device);
        if (ctx->owner) CloseHandle(ctx->owner);
        SDL_DestroyCondition(ctx->wake);
        SDL_DestroyMutex(ctx->lock);
        SDL_free(ctx);
    }
    SDL_hid_exit();
    return NULL;
}

bool SDL_BetopHapticsRumble(SDL_BetopHapticsContext *ctx,
                          float left_low, float left_high, float right_low, float right_high, Uint32 duration_ms)
{
    Uint8 packet[64];
    bool active;
    if (!ctx) return SDL_Unsupported();
    if (!SDL_BetopHapticsEncodeGrip(packet, left_low, left_high, right_low, right_high)) return false;
    active = packet[2] || packet[3];
    SDL_LockMutex(ctx->lock);
    if (ctx->failed || ctx->quit) {
        SDL_UnlockMutex(ctx->lock);
        return SDL_SetError("Betop haptics disconnected or output failed");
    }
    /* Every submission refreshes the finite host lease, even duration_ms=0.
       The worker expires it even when the application event pump stalls. */
    ctx->used = true;
    ctx->dirty[0] = true;
    SDL_memcpy(ctx->reports[0], packet, sizeof(packet));
    BetopLeaseDeadline(SDL_GetTicks(), active, duration_ms, false, &ctx->grip_deadline);
    SDL_SignalCondition(ctx->wake);
    SDL_UnlockMutex(ctx->lock);
    return true;
}

bool SDL_BetopHapticsSetEffects(SDL_BetopHapticsContext *ctx, const SDL_BetopGamepadEffects *effects, Uint32 duration_ms)
{
    Uint8 packets[4][64];
    bool active;
    Uint64 deadline;
    Uint64 now = SDL_GetTicks();
    int i;
    if (!ctx) return SDL_Unsupported();
    if (!SDL_BetopHapticsEncodeEffects(packets, effects)) return false;
    active = effects->triggers[0].mode || effects->triggers[1].mode ||
             (effects->shoulders[0].preset == 4 ? effects->shoulders[0].strength : effects->shoulders[0].preset) ||
             (effects->shoulders[1].preset == 4 ? effects->shoulders[1].strength : effects->shoulders[1].preset);
    if (!BetopLeaseDeadline(now, active, duration_ms, true, &deadline)) return false;
    SDL_LockMutex(ctx->lock);
    if (ctx->failed || ctx->quit) {
        SDL_UnlockMutex(ctx->lock);
        return SDL_SetError("Betop haptics disconnected or output failed");
    }
    ctx->used = true;
    for (i = 0; i < 4; ++i) {
        /* Keep sustained commands alive at the observed vendor cadence.
           Do not periodically retrigger unvalidated one-shot presets. */
        if (BetopEffectNeedsSubmission(ctx->reports[1 + i], packets[i], now, ctx->effect_queued_at[i])) {
            SDL_memcpy(ctx->reports[1 + i], packets[i], 64);
            ctx->dirty[1 + i] = true;
            ctx->effect_queued_at[i] = now;
        }
    }
    ctx->effect_deadline = deadline;
    SDL_SignalCondition(ctx->wake);
    SDL_UnlockMutex(ctx->lock);
    return true;
}

void SDL_BetopHapticsStop(SDL_BetopHapticsContext *ctx)
{
    if (!ctx) return;
    SDL_LockMutex(ctx->lock);
    ctx->used = true;
    BetopClearGrip(ctx);
    BetopClearEffects(ctx);
    SDL_SignalCondition(ctx->wake);
    SDL_UnlockMutex(ctx->lock);
}

void SDL_BetopHapticsClose(SDL_BetopHapticsContext *ctx)
{
    if (!ctx) return;
    SDL_LockMutex(ctx->lock);
    if (ctx->used) {
        BetopClearGrip(ctx);
        BetopClearEffects(ctx);
    }
    ctx->quit = true;
    SDL_SignalCondition(ctx->wake);
    SDL_UnlockMutex(ctx->lock);
    SDL_WaitThread(ctx->thread, NULL);
    SDL_hid_close(ctx->device);
    CloseHandle(ctx->owner);
    SDL_DestroyCondition(ctx->wake);
    SDL_DestroyMutex(ctx->lock);
    SDL_free(ctx);
    SDL_hid_exit();
}

#elif !defined(SDL_BETOPHAPTICS_PROTOCOL_TEST)

bool SDL_BetopHapticsIsSupported(SDL_Joystick *joystick) { (void)joystick; return false; }
SDL_BetopHapticsContext *SDL_BetopHapticsOpen(SDL_Joystick *joystick) { (void)joystick; SDL_Unsupported(); return NULL; }
bool SDL_BetopHapticsRumble(SDL_BetopHapticsContext *ctx, float ll, float lh, float rl, float rh, Uint32 ms)
{ (void)ctx; (void)ll; (void)lh; (void)rl; (void)rh; (void)ms; return SDL_Unsupported(); }
bool SDL_BetopHapticsSetEffects(SDL_BetopHapticsContext *ctx, const SDL_BetopGamepadEffects *effects, Uint32 ms)
{ (void)ctx; (void)effects; (void)ms; return SDL_Unsupported(); }
void SDL_BetopHapticsStop(SDL_BetopHapticsContext *ctx) { (void)ctx; }
void SDL_BetopHapticsClose(SDL_BetopHapticsContext *ctx) { (void)ctx; }

#endif
