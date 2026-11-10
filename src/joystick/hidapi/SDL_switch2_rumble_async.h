/* SPDX-License-Identifier: Zlib
 * Local Switch 2 Pro ordered, bounded asynchronous rumble transport.
 * The HIDAPI libusb event thread owns callback dispatch. Destroy before
 * releasing the borrowed HID handle/context. No callbacks acquire joystick locks.
 */
#ifndef SDL_switch2_rumble_async_h_
#define SDL_switch2_rumble_async_h_

#include "../../core/windows/SDL_windows.h"
#undef interface
// SDL_LibUSBContext must be included by the caller before this header.

typedef struct SDL_Switch2Rumble SDL_Switch2Rumble;
typedef struct
{
    SDL_Switch2Rumble *owner;
    struct libusb_transfer *transfer;
    Uint8 data[64];
    Uint64 order;
    bool busy, complete, cancelling, stop;
    enum libusb_transfer_status status;
    int bytes;
} SDL_Switch2RumbleSlot;

struct SDL_Switch2Rumble
{
    SDL_LibUSBContext *usb;
    libusb_device_handle *handle;
    SDL_Mutex *lock;
    SDL_Condition *drained;
    SDL_Thread *thread;
    HANDLE wake;
    SDL_Switch2RumbleSlot slots[4];
    Uint8 latest[64], silence[64], endpoint;
    unsigned head, count, limit;
    Uint64 order, retry_ns, last_submit_ns;
    bool accepting, latest_valid, latest_pending, stop_pending, stop_inflight;
    bool barrier, dead, quitting, had_output;
};

static void SDL_Switch2RumbleWake(SDL_Switch2Rumble *s)
{
    SetEvent(s->wake);
    SDL_BroadcastCondition(s->drained);
}

static void LIBUSB_CALL SDL_Switch2RumbleComplete(struct libusb_transfer *transfer)
{
    SDL_Switch2RumbleSlot *slot = transfer->user_data;
    SDL_Switch2Rumble *s = slot->owner;
    SDL_LockMutex(s->lock);
    slot->status = transfer->status;
    slot->bytes = transfer->actual_length;
    slot->complete = true;
    if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        s->dead = true;
        s->accepting = false;
        s->barrier = true;
    }
    SDL_Switch2RumbleWake(s);
    SDL_UnlockMutex(s->lock);
}

static void SDL_Switch2RumbleRetire(SDL_Switch2Rumble *s)
{
    // Later completions cannot free a slot or acknowledge state ahead of the head.
    while (s->count && s->slots[s->head].complete) {
        SDL_Switch2RumbleSlot *slot = &s->slots[s->head];
        bool ok = slot->status == LIBUSB_TRANSFER_COMPLETED && slot->bytes == 64;
        bool deliberate_cancel = slot->cancelling && slot->status == LIBUSB_TRANSFER_CANCELLED;
        if (slot->stop) {
            s->stop_inflight = false;
            if (ok) {
                s->stop_pending = false;
                s->had_output = false;
            }
        }
        if (!ok && !deliberate_cancel && !s->dead) {
            // Never retry an old report behind a newer in-flight report. Drain
            // the entire endpoint first, then submit only current state.
            s->barrier = true;
            if (!s->stop_pending) s->latest_pending = true;
            s->retry_ns = SDL_GetTicksNS() + SDL_MS_TO_NS(4);
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "Switch2 rumble failure: order=%llu status=%d bytes=%d",
                        (unsigned long long)slot->order, slot->status, slot->bytes);
        }
        slot->busy = slot->complete = false;
        s->head = (s->head + 1) % s->limit;
        --s->count;
        SDL_BroadcastCondition(s->drained);
    }
}

static void SDL_Switch2RumbleSubmit(SDL_Switch2Rumble *s, bool stop)
{
    SDL_Switch2RumbleSlot *slot = &s->slots[(s->head + s->count) % s->limit];
    Uint64 now = SDL_GetTicksNS();
    slot->order = s->order++;
    SDL_memcpy(slot->data, stop ? s->silence : s->latest, 64);
    // Assign the firmware sequence at submission, after all coalescing/barriers.
    slot->data[1] = slot->data[17] = (Uint8)(0x50 | (slot->order & 15));
    slot->busy = true;
    slot->complete = slot->cancelling = false;
    slot->stop = stop;
    slot->bytes = 0;
    slot->status = LIBUSB_TRANSFER_ERROR;
    ++s->count;
    if (stop) s->stop_inflight = true;
    else {
        s->latest_pending = false;
        s->had_output = true;
    }
    s->retry_ns = 0;
    s->last_submit_ns = now;
    libusb_fill_interrupt_transfer(slot->transfer, s->handle, s->endpoint,
                                  slot->data, 64, SDL_Switch2RumbleComplete, slot, 250);
    // Callbacks may run immediately, or race a producer publishing Stop.
    SDL_UnlockMutex(s->lock);
    int result = s->usb->submit_transfer(slot->transfer);
    SDL_LockMutex(s->lock);
    if (result != 0) {
        slot->status = result == LIBUSB_ERROR_NO_DEVICE ? LIBUSB_TRANSFER_NO_DEVICE : LIBUSB_TRANSFER_ERROR;
        slot->bytes = 0;
        slot->complete = true;
        if (result == LIBUSB_ERROR_NO_DEVICE) {
            s->dead = true;
            s->accepting = false;
            s->barrier = true;
        }
    }
}

static int SDLCALL SDL_Switch2RumbleThread(void *data)
{
    SDL_Switch2Rumble *s = data;
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    SDL_LockMutex(s->lock);
    for (;;) {
        SDL_Switch2RumbleRetire(s);
        if (s->dead) {
            s->latest_pending = s->stop_pending = false;
            s->barrier = true;
        }
        if (s->barrier || s->quitting) {
            for (unsigned i = 0; i < s->limit; ++i) {
                SDL_Switch2RumbleSlot *slot = &s->slots[i];
                if (slot->busy && !slot->complete && !slot->cancelling) {
                    slot->cancelling = true;
                    SDL_UnlockMutex(s->lock);
                    s->usb->cancel_transfer(slot->transfer);
                    SDL_LockMutex(s->lock);
                }
            }
            SDL_Switch2RumbleRetire(s);
            if (!s->count) s->barrier = false;
        }
        if (s->quitting && !s->count) break;
        Uint64 now = SDL_GetTicksNS();
        if (!s->dead && !s->barrier) {
            if (s->stop_pending) {
                if (!s->count && !s->stop_inflight && now >= s->retry_ns)
                    SDL_Switch2RumbleSubmit(s, true);
            } else if (s->latest_pending && s->count < s->limit && now >= s->retry_ns) {
                SDL_Switch2RumbleSubmit(s, false);
            }
        }
        // Only failed-transfer backoff is timed. Healthy output is woken by
        // publication or completion; bounded slots provide USB backpressure.
        bool timed = !s->dead && !s->barrier &&
                     ((s->stop_pending && !s->count) ||
                      (!s->stop_pending && s->latest_pending && s->count < s->limit));
        now = SDL_GetTicksNS();
        if (timed && now >= s->retry_ns) continue;
        Uint64 delay = timed ? s->retry_ns - now : 0;
        SDL_UnlockMutex(s->lock);
        DWORD result = WAIT_OBJECT_0;
        // Retry timing need not be precise, but Stop, disconnect and close must
        // interrupt it. Never sleep/spin past a newly published safety barrier.
        DWORD timeout = timed ? (DWORD)((delay + SDL_NS_PER_MS - 1) / SDL_NS_PER_MS) : INFINITE;
        result = WaitForSingleObject(s->wake, timeout);
        SDL_LockMutex(s->lock);
        if (result == WAIT_FAILED) {
            s->dead = true;
            s->accepting = false;
            s->barrier = true;
        }
    }
    SDL_UnlockMutex(s->lock);
    return 0;
}

static void SDL_Switch2RumbleStopLocked(SDL_Switch2Rumble *s)
{
    s->latest_pending = false;
    if (!s->stop_pending && !s->dead && (s->had_output || s->count)) {
        SDL_memcpy(s->silence, s->latest, 64);
        for (unsigned side = 0; side < 2; ++side) {
            Uint64 bits = 0;
            for (unsigned j = 0; j < 5; ++j) bits |= (Uint64)s->silence[2 + 16 * side + j] << (j * 8);
            bits &= ~(((Uint64)1023 << 10) | ((Uint64)1023 << 30));
            for (unsigned j = 0; j < 5; ++j) s->silence[2 + 16 * side + j] = (Uint8)(bits >> (j * 8));
        }
        s->stop_pending = true;
        s->barrier = true;
        s->retry_ns = 0;
    }
    SDL_Switch2RumbleWake(s);
}

static bool SDL_Switch2RumbleQueue(SDL_Switch2Rumble *s, const Uint8 *data, bool stop)
{
    SDL_LockMutex(s->lock);
    if (!s->accepting || s->dead || s->quitting) {
        SDL_UnlockMutex(s->lock);
        return SDL_SetError("Switch2 asynchronous rumble transport is unavailable");
    }
    if (stop) {
        SDL_Switch2RumbleStopLocked(s);
    } else {
        Uint8 value[64];
        SDL_memcpy(value, data, 64);
        value[1] = value[17] = 0x50; // Producer counters are assigned at actual submission.
        bool same = s->latest_valid && SDL_memcmp(value, s->latest, 64) == 0;
        Uint64 now = SDL_GetTicksNS();
        // Device polling can repeat the same state several times per report.
        // Deduplicate only those repeats; changed curve values never wait for
        // a software tick. Identical keepalives use the driver's original
        // 12 ms interval. Activation after Stop must not be deduped.
        if (same && !s->stop_pending &&
            (s->latest_pending || s->count || (s->had_output && now - s->last_submit_ns < SDL_MS_TO_NS(12)))) {
            SDL_UnlockMutex(s->lock);
            return true;
        }
        SDL_memcpy(s->latest, value, 64);
        s->latest_valid = s->latest_pending = true;
        SDL_Switch2RumbleWake(s);
    }
    SDL_UnlockMutex(s->lock);
    return true;
}

static void SDL_Switch2RumbleDisconnect(SDL_Switch2Rumble *s)
{
    SDL_LockMutex(s->lock);
    s->dead = true;
    s->accepting = false;
    s->barrier = true;
    s->latest_pending = s->stop_pending = false;
    SDL_Switch2RumbleWake(s);
    SDL_UnlockMutex(s->lock);
}

static bool SDL_Switch2RumblePause(SDL_Switch2Rumble *s)
{
    Uint64 until = SDL_GetTicksNS() + SDL_MS_TO_NS(1000);
    SDL_LockMutex(s->lock);
    s->accepting = false;
    SDL_Switch2RumbleStopLocked(s);
    while (s->count || s->stop_pending) {
        if (SDL_GetTicksNS() >= until) {
            s->dead = true;
            s->barrier = true;
            s->stop_pending = false;
            SDL_Switch2RumbleWake(s);
        }
        SDL_WaitConditionTimeout(s->drained, s->lock, 10);
    }
    bool ok = !s->dead;
    SDL_UnlockMutex(s->lock);
    return ok;
}

static bool SDL_Switch2RumbleResume(SDL_Switch2Rumble *s)
{
    SDL_LockMutex(s->lock);
    bool ok = !s->dead && !s->quitting && !s->count && !s->stop_pending;
    if (ok) {
        s->accepting = true;
        s->latest_valid = s->latest_pending = false;
        s->retry_ns = 0;
    }
    SDL_UnlockMutex(s->lock);
    return ok;
}

static void SDL_Switch2RumbleDestroy(SDL_Switch2Rumble *s)
{
    if (!s) return;
    if (s->thread) {
        SDL_Switch2RumblePause(s);
        SDL_LockMutex(s->lock);
        s->quitting = true;
        SDL_Switch2RumbleWake(s);
        SDL_UnlockMutex(s->lock);
        SDL_WaitThread(s->thread, NULL);
    }
    for (unsigned i = 0; i < s->limit; ++i)
        if (s->slots[i].transfer) s->usb->free_transfer(s->slots[i].transfer);
    if (s->wake) CloseHandle(s->wake);
    SDL_DestroyCondition(s->drained);
    SDL_DestroyMutex(s->lock);
    SDL_free(s);
}

static SDL_Switch2Rumble *SDL_Switch2RumbleCreate(SDL_LibUSBContext *usb, libusb_device_handle *handle, Uint8 endpoint)
{
    SDL_Switch2Rumble *s = SDL_calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->usb = usb;
    s->handle = handle;
    s->endpoint = endpoint;
    // Private diagnostic override. Queue depth is a transport choice, not channels.
    // 1..4 comparison: two is the smallest depth sustaining changing 4/5 ms
    // inputs without coalescing on the tested Windows full-speed OUT endpoint.
    const char *depth = SDL_GetHint("SDL_JOYSTICK_HIDAPI_SWITCH2_RUMBLE_SLOTS");
    int requested = depth ? SDL_atoi(depth) : 2;
    s->limit = requested >= 1 && requested <= 4 ? (unsigned)requested : 2;
    s->lock = SDL_CreateMutex();
    s->drained = SDL_CreateCondition();
    s->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    for (unsigned i = 0; i < s->limit; ++i) {
        s->slots[i].owner = s;
        s->slots[i].transfer = usb->alloc_transfer(0);
    }
    bool allocated = true;
    for (unsigned i = 0; i < s->limit; ++i) allocated = allocated && s->slots[i].transfer != NULL;
    if (!s->lock || !s->drained || !s->wake || !allocated) {
        SDL_Switch2RumbleDestroy(s);
        return NULL;
    }
    s->accepting = true;
    s->thread = SDL_CreateThread(SDL_Switch2RumbleThread, "Switch2 rumble", s);
    if (!s->thread) {
        SDL_Switch2RumbleDestroy(s);
        return NULL;
    }
    return s;
}
#endif
