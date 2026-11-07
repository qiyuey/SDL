/* SPDX-License-Identifier: Zlib
 * Opt-in public-API regression on one real 057e:2069 controller.
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
static SDL_AtomicInt sensors;
static bool SDLCALL watch(void *unused,SDL_Event *event) {
    (void)unused;
    if(event->type==SDL_EVENT_GAMEPAD_SENSOR_UPDATE)SDL_AddAtomicInt(&sensors,1);
    return true;
}
static void service(unsigned ms) {
    Uint64 until=SDL_GetTicksNS()+SDL_MS_TO_NS(ms);
    while(SDL_GetTicksNS()<until) {SDL_UpdateJoysticks();SDL_DelayPrecise(SDL_MS_TO_NS(1));}
}
int main(int argc,char **argv) {
    if(argc!=2||strcmp(argv[1],"--play")) {puts("Usage: testswitch2async_device --play (low-amplitude real-controller test)");return 2;}
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_SWITCH2_EXPERIMENTAL_HD","1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_RUMBLE_TRACE","1");
    SDL_SetLogPriority(SDL_LOG_CATEGORY_INPUT,SDL_LOG_PRIORITY_INFO);
    if(!SDL_Init(SDL_INIT_GAMEPAD))return 1;
    int count;SDL_JoystickID *ids=SDL_GetGamepads(&count),id=0;
    for(int i=0;i<count;++i)if(SDL_GetGamepadVendorForID(ids[i])==0x057e&&SDL_GetGamepadProductForID(ids[i])==0x2069) {id=ids[i];break;}
    SDL_free(ids);if(!id) {puts("SKIP no 057e:2069");SDL_Quit();return 77;}
    SDL_Gamepad *pad=SDL_OpenGamepad(id);if(!pad) {puts(SDL_GetError());SDL_Quit();return 1;}
    SDL_AddEventWatch(watch,NULL);
    bool gyro=SDL_SetGamepadSensorEnabled(pad,SDL_SENSOR_GYRO,true);
    bool accel=SDL_SetGamepadSensorEnabled(pad,SDL_SENSOR_ACCEL,true);
    service(300);int before=SDL_GetAtomicInt(&sensors);service(400);int baseline=SDL_GetAtomicInt(&sensors)-before;
    printf("PHASE active ns=%llu\n",(unsigned long long)SDL_GetTicksNS());fflush(stdout);
    before=SDL_GetAtomicInt(&sensors);Uint64 begin=SDL_GetTicksNS();unsigned failures=0;
    for(unsigned i=0;i<500;++i) {
        float a=.02f+.03f*(i%25)/24.f;
        if(!SDL_RumbleGamepadFrequenciesEx(pad,a,160,.02f,320,.02f,180,a,360,100))++failures;
        SDL_UpdateJoysticks();Uint64 due=begin+(Uint64)(i+1)*SDL_MS_TO_NS(4),now=SDL_GetTicksNS();
        if(now<due)SDL_DelayPrecise(due-now);
    }
    int active=SDL_GetAtomicInt(&sensors)-before;
    printf("PHASE stop ns=%llu\n",(unsigned long long)SDL_GetTicksNS());fflush(stdout);
    if(!SDL_RumbleGamepadFrequenciesEx(pad,0,160,0,320,0,180,0,360,0))++failures;
    service(40);SDL_CloseGamepad(pad);
    printf("PHASE reopen ns=%llu\n",(unsigned long long)SDL_GetTicksNS());fflush(stdout);
    pad=SDL_OpenGamepad(id);
    if(!pad) {SDL_RemoveEventWatch(watch,NULL);SDL_Quit();return 1;}
    service(60);
    printf("PHASE second-active ns=%llu\n",(unsigned long long)SDL_GetTicksNS());fflush(stdout);
    if(!SDL_RumbleGamepadFrequenciesEx(pad,.025f,160,.02f,320,.02f,180,.025f,360,100))++failures;
    service(20);
    printf("PHASE close-active ns=%llu\n",(unsigned long long)SDL_GetTicksNS());fflush(stdout);
    SDL_CloseGamepad(pad);service(20);SDL_RemoveEventWatch(watch,NULL);SDL_Quit();
    printf("RESULT calls=502 failures=%u gyro=%d accel=%d baseline_events_400ms=%d active_events_2000ms=%d\n",failures,gyro,accel,baseline,active);
    return failures?1:0;
}
