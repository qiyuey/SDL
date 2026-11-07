/* SPDX-License-Identifier: Zlib
 * Runs the production transport with fake libusb, no device enumeration/I/O.
 */
#include <SDL3/SDL.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#define SDL_internal_h_
#define HAVE_LIBUSB 1
#define HAVE_DXGI1_6_H 1
#include "../src/misc/SDL_libusb.h"
#include "../src/joystick/hidapi/SDL_switch2_rumble_async.h"

static SDL_Mutex *fake_lock;
static struct libusb_transfer *pending[4];
static struct { Uint8 bytes[64]; bool stop; } sent[2048];
static unsigned sent_count, maximum, allocations, frees, cancel_count;
static bool auto_cancel = true, auto_stop = true;
static int fail_submit;

static struct libusb_transfer *LIBUSB_CALL allocate(int n) {
    (void)n; ++allocations;
    return SDL_calloc(1, sizeof(struct libusb_transfer));
}
static void LIBUSB_CALL release_transfer(struct libusb_transfer *t) {
    SDL_LockMutex(fake_lock);
    for (unsigned i=0;i<4;++i) assert(pending[i]!=t);
    ++frees; SDL_UnlockMutex(fake_lock);SDL_free(t);
}
static void finish(struct libusb_transfer *t, enum libusb_transfer_status status, int bytes) {
    SDL_LockMutex(fake_lock);
    for(unsigned i=0;i<4;++i) if(pending[i]==t)pending[i]=NULL;
    t->status=status;t->actual_length=bytes;
    SDL_UnlockMutex(fake_lock);
    t->callback(t);
}
static int LIBUSB_CALL submit(struct libusb_transfer *t) {
    SDL_LockMutex(fake_lock);
    assert(sent_count<2048);
    SDL_memcpy(sent[sent_count].bytes,t->buffer,64);
    bool stop=((SDL_Switch2RumbleSlot*)t->user_data)->stop;
    sent[sent_count++].stop=stop;
    int failure=fail_submit;fail_submit=0;
    if(failure) {SDL_UnlockMutex(fake_lock);return failure;}
    unsigned i;for(i=0;i<4;++i)if(!pending[i]) {pending[i]=t;break;}
    assert(i<4);
    unsigned count=0;for(unsigned j=0;j<4;++j)count+=pending[j]!=NULL;if(count>maximum)maximum=count;
    bool instant=stop && auto_stop;
    SDL_UnlockMutex(fake_lock);
    if(instant)finish(t,LIBUSB_TRANSFER_COMPLETED,64);
    return 0;
}
static int LIBUSB_CALL cancel(struct libusb_transfer *t) {
    SDL_LockMutex(fake_lock);++cancel_count;bool instant=auto_cancel;SDL_UnlockMutex(fake_lock);
    if(instant)finish(t,LIBUSB_TRANSFER_CANCELLED,0);
    return 0;
}
static SDL_LibUSBContext api;
static void reset(void) {
    SDL_LockMutex(fake_lock);
    for(unsigned i=0;i<4;++i)assert(!pending[i]);
    sent_count=maximum=cancel_count=0;auto_cancel=auto_stop=true;fail_submit=0;
    SDL_UnlockMutex(fake_lock);
}
static SDL_Switch2Rumble *create(void) {
    SDL_Switch2Rumble *s=SDL_Switch2RumbleCreate(&api,(libusb_device_handle*)1,1);assert(s);return s;
}
static void queue(SDL_Switch2Rumble *s,Uint8 tag,bool stop) {
    Uint8 b[64]={2,0x50,0xff,0xff,0xff,0xff,0xff};
    SDL_memcpy(b+17,b+1,6);b[63]=tag;assert(SDL_Switch2RumbleQueue(s,b,stop));
}
static unsigned calls(void) {SDL_LockMutex(fake_lock);unsigned n=sent_count;SDL_UnlockMutex(fake_lock);return n;}
static void wait_calls(unsigned n) {
    Uint64 end=SDL_GetTicksNS()+SDL_MS_TO_NS(2000);
    while(calls()<n) {if(SDL_GetTicksNS()>=end)fprintf(stderr,"waiting %u got %u\n",n,calls());assert(SDL_GetTicksNS()<end);SDL_Delay(1);}
}
static struct libusb_transfer *request(unsigned ordinal) {
    SDL_LockMutex(fake_lock);struct libusb_transfer *t=NULL;
    for(unsigned i=0;i<4;++i)if(pending[i]&&((SDL_Switch2RumbleSlot*)pending[i]->user_data)->order==ordinal)t=pending[i];
    SDL_UnlockMutex(fake_lock);assert(t);return t;
}
static void check(unsigned index,Uint8 tag,bool stop) {
    SDL_LockMutex(fake_lock);assert(index<sent_count);assert(sent[index].stop==stop);
    if(!stop)assert(sent[index].bytes[63]==tag);
    assert((sent[index].bytes[1]&15)==(index&15u));assert(sent[index].bytes[1]==sent[index].bytes[17]);
    if(stop)for(unsigned side=0;side<2;++side) {
        Uint64 v=0;for(unsigned j=0;j<5;++j)v|=(Uint64)sent[index].bytes[2+16*side+j]<<(8*j);
        assert(!((v>>10)&1023)&&!((v>>30)&1023));
    }
    SDL_UnlockMutex(fake_lock);
}
static int SDLCALL destroy_thread(void *arg) {SDL_Switch2RumbleDestroy(arg);return 0;}

int main(void) {
    _set_error_mode(_OUT_TO_STDERR);
    setvbuf(stdout,NULL,_IONBF,0);
    fake_lock=SDL_CreateMutex();assert(fake_lock);
    api.alloc_transfer=allocate;api.free_transfer=release_transfer;api.submit_transfer=submit;api.cancel_transfer=cancel;
    reset();SDL_Switch2Rumble *s=create();
    queue(s,1,false);wait_calls(1);queue(s,2,false);wait_calls(2);
    struct libusb_transfer *first=request(0),*second=request(1);
    Uint8 old[64];SDL_memcpy(old,first->buffer,64);
    finish(second,LIBUSB_TRANSFER_COMPLETED,64);
    for(unsigned i=0;i<10000;++i)queue(s,(Uint8)(i%250),false);
    SDL_Delay(10);assert(calls()==2);assert(!SDL_memcmp(old,first->buffer,64));
    finish(first,LIBUSB_TRANSFER_COMPLETED,64);wait_calls(3);check(2,249,false);
    finish(request(2),LIBUSB_TRANSFER_COMPLETED,64);
    assert(SDL_Switch2RumblePause(s));assert(SDL_Switch2RumbleResume(s));
    unsigned before=calls();SDL_Delay(10);assert(calls()==before);
    SDL_Switch2RumbleDestroy(s);assert(maximum==2);
    puts("PASS: 10000 replacements, immutable in-flight buffers, reversed callbacks, FIFO retirement and pause/resume");

    reset();s=create();auto_stop=false;
    queue(s,1,false);wait_calls(1);queue(s,2,false);wait_calls(2);
    queue(s,0,true);wait_calls(3);check(2,0,true);assert(cancel_count==2);
    queue(s,9,false);SDL_Delay(10);assert(calls()==3);
    finish(request(2),LIBUSB_TRANSFER_COMPLETED,64);wait_calls(4);check(3,9,false);
    auto_stop=true;assert(SDL_Switch2RumblePause(s));SDL_Switch2RumbleDestroy(s);
    puts("PASS: full-pipeline Stop cancels/drains, stereo silence acknowledged before newer active data");

    reset();s=create();auto_stop=false;
    queue(s,1,false);wait_calls(1);queue(s,2,false);wait_calls(2);queue(s,9,false);
    finish(request(0),LIBUSB_TRANSFER_ERROR,0);wait_calls(3);check(2,9,false);
    finish(request(2),LIBUSB_TRANSFER_COMPLETED,64);
    queue(s,0,true);wait_calls(4);check(3,0,true);
    queue(s,7,false);finish(request(3),LIBUSB_TRANSFER_COMPLETED,63);
    wait_calls(5);check(4,0,true);assert(calls()==5);
    finish(request(4),LIBUSB_TRANSFER_COMPLETED,64);wait_calls(6);check(5,7,false);
    auto_stop=true;SDL_Switch2RumbleDestroy(s);
    puts("PASS: older failed packets never replay after newer state; short/failed Stop retries before active output");

    reset();s=create();queue(s,1,false);wait_calls(1);queue(s,2,false);wait_calls(2);
    finish(request(1),LIBUSB_TRANSFER_NO_DEVICE,0);
    Uint64 until=SDL_GetTicksNS()+SDL_MS_TO_NS(2000);
    for(;;) {SDL_LockMutex(s->lock);bool empty=s->count==0;SDL_UnlockMutex(s->lock);if(empty)break;assert(SDL_GetTicksNS()<until);SDL_Delay(1);}
    Uint8 b[64]={2};assert(!SDL_Switch2RumbleQueue(s,b,false));
    before=calls();SDL_Switch2RumbleDestroy(s);assert(calls()==before);
    s=create();queue(s,4,false);wait_calls(before+1);SDL_Switch2RumbleDestroy(s);
    puts("PASS: NO_DEVICE aborts/drains all I/O, rejects stale output, fresh device gets independent lifetime");

    reset();s=create();queue(s,1,false);wait_calls(1);queue(s,2,false);wait_calls(2);auto_cancel=false;
    first=request(0);second=request(1);
    SDL_Thread *closing=SDL_CreateThread(destroy_thread,"close test",s);assert(closing);
    SDL_Delay(20);SDL_LockMutex(fake_lock);assert(cancel_count==2);assert(frees<allocations);SDL_UnlockMutex(fake_lock);
    finish(second,LIBUSB_TRANSFER_CANCELLED,0);finish(first,LIBUSB_TRANSFER_CANCELLED,0);
    SDL_WaitThread(closing,NULL);assert(allocations==frees);
    puts("PASS: close waits for delayed cancellation callbacks before freeing transfer and buffer storage");

    reset();s=create();queue(s,5,false);wait_calls(1);queue(s,6,false);wait_calls(2);
    SDL_Switch2RumbleDisconnect(s);
    assert(!SDL_Switch2RumblePause(s));assert(!SDL_Switch2RumbleQueue(s,b,false));
    SDL_Switch2RumbleDestroy(s);assert(allocations==frees);
    puts("PASS: input-side disconnect notification rejects output and drains cancellation before close");

    reset();s=create();queue(s,1,false);wait_calls(1);
    SDL_LockMutex(s->lock);assert(s->retry_ns==0);SDL_UnlockMutex(s->lock);
    queue(s,2,false);wait_calls(2);
    SDL_LockMutex(s->lock);assert(s->retry_ns==0 && s->count==2);SDL_UnlockMutex(s->lock);
    queue(s,2,false);SDL_Delay(5);assert(calls()==2);
    SDL_Switch2RumbleDestroy(s);
    puts("PASS: healthy changes submit without a timer gate; identical in-flight repeats are deduplicated");

    reset();s=create();queue(s,1,false);wait_calls(1);
    finish(request(0),LIBUSB_TRANSFER_COMPLETED,64);
    SDL_LockMutex(s->lock);s->retry_ns=SDL_GetTicksNS()+SDL_MS_TO_NS(1000);SDL_UnlockMutex(s->lock);
    queue(s,2,false);SDL_Delay(20);assert(calls()==1);
    queue(s,0,true);
    until=SDL_GetTicksNS()+SDL_MS_TO_NS(200);
    while(calls()<2) {assert(SDL_GetTicksNS()<until);SDL_Delay(1);}
    check(1,0,true);SDL_Switch2RumbleDestroy(s);
    puts("PASS: Stop interrupts an outstanding retry wait instead of sleeping until its old deadline");

    reset();fail_submit=LIBUSB_ERROR_NO_DEVICE;s=create();queue(s,1,false);wait_calls(1);SDL_Delay(10);
    assert(!SDL_Switch2RumbleQueue(s,b,false));SDL_Switch2RumbleDestroy(s);assert(allocations==frees);
    for(unsigned depth=1;depth<=4;++depth) {
        char value[8];SDL_snprintf(value,sizeof(value),"%u",depth);
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_SWITCH2_RUMBLE_SLOTS",value);
        reset();s=create();assert(s->limit==depth);
        for(unsigned i=0;i<depth;++i) {queue(s,(Uint8)(i+1),false);wait_calls(i+1);}
        for(unsigned i=0;i<10000;++i)queue(s,(Uint8)(i%250),false);
        for(unsigned i=depth;i>1;--i)finish(request(i-1),LIBUSB_TRANSFER_COMPLETED,64);
        SDL_Delay(5);assert(calls()==depth);
        finish(request(0),LIBUSB_TRANSFER_COMPLETED,64);wait_calls(depth+1);check(depth,249,false);
        auto_stop=false;queue(s,0,true);wait_calls(depth+2);check(depth+1,0,true);
        queue(s,8,false);SDL_Delay(5);assert(calls()==depth+2);
        finish(request(depth+1),LIBUSB_TRANSFER_COMPLETED,64);wait_calls(depth+3);check(depth+2,8,false);
        auto_stop=true;SDL_Switch2RumbleDestroy(s);assert(maximum==depth);assert(allocations==frees);
        reset();s=create();
        for(unsigned i=0;i<depth;++i) {queue(s,(Uint8)(i+1),false);wait_calls(i+1);}
        finish(request(depth-1),LIBUSB_TRANSFER_NO_DEVICE,0);
        assert(!SDL_Switch2RumblePause(s));SDL_Switch2RumbleDestroy(s);assert(allocations==frees);
        printf("PASS: depth %u bounded coalescing, reversed completion, stop barrier and NO_DEVICE drain\n",depth);
    }
    SDL_ResetHint("SDL_JOYSTICK_HIDAPI_SWITCH2_RUMBLE_SLOTS");
    SDL_DestroyMutex(fake_lock);puts("PASS: immediate submit failure and cleanup; no physical USB output");return 0;
}
