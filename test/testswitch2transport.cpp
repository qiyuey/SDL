// SPDX-License-Identifier: Zlib
// Windows-only, opt-in transport experiment. Synthetic low-amplitude packets;
// never replaces the SDL DLL or changes device drivers.
// clang-format off
#include <SDL3/SDL.h>
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include <libusb.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../src/joystick/hidapi/SDL_switch2_rumble.h"
// clang-format on

static unsigned long long ns()
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<unsigned long long>((static_cast<long double>(t.QuadPart) * 1e9L) / f.QuadPart);
}
struct Slot
{
    OVERLAPPED overlap{};
    libusb_transfer *transfer{};
    std::array<unsigned char, 128> data{};
    bool pending{}, stop{};
    unsigned id{};
    unsigned long long begin{}, returned{}, complete{};
    int status{}, bytes{};
};
static std::mutex state_mutex;
static unsigned successful{}, failed{}, cancelled{};
static std::atomic<unsigned long long> first_device_error{};
static void record(Slot &s)
{
    if (s.status == 0 && s.bytes >= 64)
        ++successful;
    else if (s.status == LIBUSB_TRANSFER_CANCELLED || s.status == ERROR_OPERATION_ABORTED)
        ++cancelled;
    else {
        ++failed;
        if (!first_device_error)
            first_device_error = s.complete;
    }
    std::printf("{\"event\":\"complete\",\"id\":%u,\"stop\":%d,\"submit_ns\":%llu,\"return_ns\":%llu,\"complete_ns\":%llu,\"status\":%d,\"bytes\":%d}\n",
                s.id, s.stop, s.begin, s.returned, s.complete, s.status, s.bytes);
    s.pending = false;
}
static void LIBUSB_CALL completed(libusb_transfer *t)
{
    auto observed = ns();
    std::lock_guard guard(state_mutex);
    auto &s = *static_cast<Slot *>(t->user_data);
    s.complete = observed;
    s.status = static_cast<int>(t->status);
    s.bytes = t->actual_length;
    record(s);
}
static HANDLE open_native(unsigned &length, bool &numbered)
{
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO list = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    HANDLE result = INVALID_HANDLE_VALUE;
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA entry{};
        entry.cbSize = sizeof entry;
        if (!SetupDiEnumDeviceInterfaces(list, nullptr, &guid, i, &entry))
            break;
        DWORD size{};
        SetupDiGetDeviceInterfaceDetailW(list, &entry, nullptr, 0, &size, nullptr);
        std::vector<unsigned char> memory(size);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(memory.data());
        detail->cbSize = sizeof *detail;
        if (!SetupDiGetDeviceInterfaceDetailW(list, &entry, detail, size, nullptr, nullptr))
            continue;
        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof attributes;
        PHIDP_PREPARSED_DATA parsed{};
        HIDP_CAPS caps{};
        if (HidD_GetAttributes(h, &attributes) && attributes.VendorID == 0x057e && attributes.ProductID == 0x2069 &&
            HidD_GetPreparsedData(h, &parsed)) {
            if (HidP_GetCaps(parsed, &caps) == HIDP_STATUS_SUCCESS && caps.OutputReportByteLength >= 64 && caps.OutputReportByteLength <= 128) {
                numbered = false;
                USHORT count = caps.NumberOutputValueCaps;
                std::vector<HIDP_VALUE_CAPS> values(count);
                if (count && HidP_GetValueCaps(HidP_Output, values.data(), &count, parsed) == HIDP_STATUS_SUCCESS)
                    for (USHORT j = 0; j < count; ++j)
                        numbered |= values[j].ReportID != 0;
                count = caps.NumberOutputButtonCaps;
                std::vector<HIDP_BUTTON_CAPS> buttons(count);
                if (count && HidP_GetButtonCaps(HidP_Output, buttons.data(), &count, parsed) == HIDP_STATUS_SUCCESS)
                    for (USHORT j = 0; j < count; ++j)
                        numbered |= buttons[j].ReportID != 0;
                length = caps.OutputReportByteLength;
                std::printf("{\"event\":\"hid_caps\",\"output_length\":%u,\"numbered\":%d,\"usage_page\":%u,\"usage\":%u}\n", length, numbered, caps.UsagePage, caps.Usage);
                result = h;
            }
            HidD_FreePreparsedData(parsed);
        }
        if (result != INVALID_HANDLE_VALUE)
            break;
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(list);
    return result;
}
static void packet(Slot &s, unsigned id, bool stop, bool native, bool numbered)
{
    s.data.fill(0);
    unsigned char raw[64]{};
    float a = stop ? 0.f : .02f + .03f * static_cast<float>(id % 25) / 24.f;
    raw[0] = 2;
    raw[1] = static_cast<unsigned char>(0x50 | (id & 15));
    Switch2PackRumble(Switch2FrequencyCode(320), Switch2AmplitudeCode(stop ? 0.f : .02f),
                      Switch2FrequencyCode(160), Switch2AmplitudeCode(a), raw + 2);
    std::memcpy(raw + 17, raw + 1, 6);
    std::memcpy(s.data.data() + (native && !numbered ? 1 : 0), raw, sizeof raw);
    s.stop = stop;
    s.id = id;
}
int main(int argc, char **argv)
{
    if (argc == 2 && !std::strcmp(argv[1], "--self-test")) {
        for (unsigned id = 0; id < 32; ++id) {
            Slot active{}, silent{}, unnumbered{};
            packet(active, id, false, false, true);
            packet(silent, id, true, false, true);
            packet(unnumbered, id, false, true, false);
            if (active.data[0] != 2 || active.data[1] != (0x50 | (id & 15)) ||
                unnumbered.data[0] != 0 || std::memcmp(active.data.data(), unnumbered.data.data() + 1, 64))
                return 1;
            for (unsigned side = 0; side < 2; ++side) {
                unsigned long long a = 0, z = 0;
                for (unsigned i = 0; i < 5; ++i) {
                    a |= static_cast<unsigned long long>(active.data[2 + 16 * side + i]) << (8 * i);
                    z |= static_cast<unsigned long long>(silent.data[2 + 16 * side + i]) << (8 * i);
                }
                if (((a >> 10) & 1023) > 21 || ((a >> 30) & 1023) > 52 || ((z >> 10) & 1023) || ((z >> 30) & 1023))
                    return 1;
            }
        }
        std::puts("PASS: bounded amplitudes, stereo stop, sequence wrap and native report-ID layout");
        return 0;
    }
    std::string mode = argc > 1 ? argv[1] : "";
    if (argc < 3 || std::strcmp(argv[2], "--play") ||
        (mode != "native1" && mode != "native2" && mode != "usb1" && mode != "usb2")) {
        std::puts("Usage: testswitch2transport native1|native2|usb1|usb2 --play [--disconnect|--cancel-stop|--long]");
        return 2;
    }
    if (argc > 4 || (argc == 4 && std::strcmp(argv[3], "--disconnect") &&
                     std::strcmp(argv[3], "--cancel-stop") && std::strcmp(argv[3], "--long")))
        return 2;
    bool disconnect = argc > 3 && !std::strcmp(argv[3], "--disconnect");
    bool cancel_stop = argc > 3 && !std::strcmp(argv[3], "--cancel-stop");
    bool long_run = argc > 3 && !std::strcmp(argv[3], "--long");
    bool native = mode.starts_with("native");
    unsigned depth = mode.back() == '2' ? 2 : 1;
    FILETIME clock;
    GetSystemTimePreciseAsFileTime(&clock);
    unsigned long long filetime = (static_cast<unsigned long long>(clock.dwHighDateTime) << 32) | clock.dwLowDateTime;
    std::printf("{\"event\":\"anchor\",\"qpc_ns\":%llu,\"filetime_100ns\":%llu,\"mode\":\"%s\"}\n", ns(), filetime, mode.c_str());
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_SWITCH2_EXPERIMENTAL_HD", "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL init: %s\n", SDL_GetError());
        return 1;
    }
    int count{};
    auto *pads = SDL_GetGamepads(&count);
    SDL_Gamepad *pad{};
    for (int i = 0; i < count; ++i)
        if (SDL_GetGamepadVendorForID(pads[i]) == 0x057e && SDL_GetGamepadProductForID(pads[i]) == 0x2069) {
            pad = SDL_OpenGamepad(pads[i]);
            break;
        }
    SDL_free(pads);
    if (!pad) {
        std::fprintf(stderr, "SKIP no 057e:2069\n");
        SDL_Quit();
        return 77;
    }
    SDL_RumbleGamepadFrequenciesEx(pad, 0, 160, 0, 320, 0, 160, 0, 320, 0);
    SDL_UpdateJoysticks();
    SDL_Delay(100);
    unsigned length = 64;
    bool numbered = true;
    HANDLE handle = INVALID_HANDLE_VALUE;
    libusb_context *context{};
    libusb_device_handle *usb{};
    if (native) {
        handle = open_native(length, numbered);
        if (handle == INVALID_HANDLE_VALUE || (!numbered && length < 65)) {
            std::fprintf(stderr, "HID open/layout failure %lu\n", GetLastError());
            if (pad)
                SDL_CloseGamepad(pad);
            SDL_Quit();
            return 1;
        }
    } else {
        // WinUSB interface handles opened by SDL can prevent a second composite
        // device open. Preserve device initialization, then release SDL handles.
        SDL_CloseGamepad(pad);
        pad = nullptr;
        SDL_Quit();
        int init_result = libusb_init(&context);
        if (!init_result)
            usb = libusb_open_device_with_vid_pid(context, 0x057e, 0x2069);
        int claim_result = usb ? libusb_claim_interface(usb, 0) : LIBUSB_ERROR_NO_DEVICE;
        if (init_result || !usb || claim_result) {
            std::fprintf(stderr, "libusb init=%d open=%d claim=%d\n", init_result, usb != nullptr, claim_result);
            if (usb)
                libusb_close(usb);
            if (context)
                libusb_exit(context);
            if (pad)
                SDL_CloseGamepad(pad);
            SDL_Quit();
            return 1;
        }
    }
    std::atomic<bool> events_running{ true };
    std::thread events;
    if (!native)
        events = std::thread([&] {while(events_running) {timeval tv{0,1000};libusb_handle_events_timeout(context,&tv);} });
    std::array<Slot, 2> slots{};
    for (unsigned i = 0; i < depth; ++i) {
        auto &s = slots[i];
        if (native)
            s.overlap.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        else
            s.transfer = libusb_alloc_transfer(0);
    }
    auto pump = [&] {
        if (!native)
            return;
        std::lock_guard guard(state_mutex);
        for (unsigned i = 0; i < depth; ++i) {
            auto &s = slots[i];
            if (!s.pending || WaitForSingleObject(s.overlap.hEvent, 0) != WAIT_OBJECT_0)
                continue;
            DWORD bytes{};
            s.complete = ns();
            s.status = GetOverlappedResult(handle, &s.overlap, &bytes, FALSE) ? 0 : static_cast<int>(GetLastError());
            s.bytes = static_cast<int>(bytes);
            record(s);
        }
    };
    auto pending = [&] {std::lock_guard guard(state_mutex);unsigned n=0;for(unsigned i=0;i<depth;++i)n+=slots[i].pending;return n; };
    auto submit = [&](Slot &s, unsigned id, bool stop) {
        std::lock_guard guard(state_mutex);
        if (s.pending)
            return false;
        packet(s, id, stop, native, numbered);
        s.pending = true;
        s.status = 0;
        s.bytes = 0;
        s.complete = 0;
        s.begin = ns();
        int result = 0;
        if (native) {
            ResetEvent(s.overlap.hEvent);
            DWORD bytes{};
            BOOL ok = WriteFile(handle, s.data.data(), length, &bytes, &s.overlap);
            if (!ok && GetLastError() != ERROR_IO_PENDING)
                result = static_cast<int>(GetLastError());
        } else {
            libusb_fill_interrupt_transfer(s.transfer, usb, 1, s.data.data(), 64, completed, &s, 250);
            result = libusb_submit_transfer(s.transfer);
        }
        s.returned = ns();
        if (result) {
            s.complete = ns();
            s.status = result;
            record(s);
        }
        std::printf("{\"event\":\"submit\",\"id\":%u,\"stop\":%d,\"begin_ns\":%llu,\"returned_ns\":%llu,\"result\":%d}\n", id, stop, s.begin, s.returned, result);
        return result == 0;
    };
    auto begin = ns(), next = begin;
    unsigned id = 0, coalesced = 0;
    auto duration = disconnect ? 10'000'000'000ULL : (cancel_stop ? 796'200'000ULL : (long_run ? 3'000'200'000ULL : 800'000'000ULL));
    std::printf("{\"event\":\"active_start\",\"ns\":%llu,\"depth\":%u,\"disconnect\":%d}\n", begin, depth, disconnect);
    std::fflush(stdout);
    while (ns() - begin < duration) {
        pump();
        {
            std::lock_guard guard(state_mutex);
            if (first_device_error)
                break;
        }
        auto now = ns();
        if (now >= next) {
            unsigned sample = static_cast<unsigned>((now - begin) / 4'000'000);
            bool sent = false;
            for (unsigned i = 0; i < depth; ++i) {
                bool free;
                {
                    std::lock_guard guard(state_mutex);
                    free = !slots[i].pending;
                }
                if (free) {
                    sent = submit(slots[i], sample, false);
                    break;
                }
            }
            ++id;
            if (!sent)
                ++coalesced;
            next = begin + (static_cast<unsigned long long>(sample) + 1) * 4'000'000;
        }
        SDL_DelayPrecise(100'000);
    }
    if (cancel_stop && !first_device_error) {
        auto deadline = ns() + 2'000'000'000;
        while (pending() && ns() < deadline) {
            pump();
            SDL_DelayPrecise(100'000);
        }
        if (pending()) {
            std::fprintf(stderr, "Cannot prepare cancellation test\n");
            ExitProcess(2);
        }
        // Deliberately create a full pipeline immediately before Stop, so the
        // cancellation path is exercised rather than racing a completed I/O.
        for (unsigned i = 0; i < depth; ++i)
            submit(slots[i], 1000 + i, false);
    }
    auto stop_requested = ns();
    std::printf("{\"event\":\"stop_requested\",\"ns\":%llu,\"in_flight\":%u}\n", stop_requested, pending());
    {
        std::lock_guard guard(state_mutex);
        for (unsigned i = 0; i < depth; ++i)
            if (slots[i].pending) {
                int r = native ? (CancelIoEx(handle, &slots[i].overlap) ? 0 : static_cast<int>(GetLastError())) : libusb_cancel_transfer(slots[i].transfer);
                std::printf("{\"event\":\"cancel\",\"id\":%u,\"result\":%d}\n", slots[i].id, r);
            }
    }
    auto drain_deadline = ns() + 2'000'000'000;
    while (pending() && ns() < drain_deadline) {
        pump();
        SDL_DelayPrecise(100'000);
    }
    if (pending()) {
        std::fprintf(stderr, "FATAL cancellation did not drain; buffers retained until process exit\n");
        std::fflush(stdout);
        ExitProcess(2);
    }
    unsigned long long stop_completed{};
    // Also try silence after a transport fault: a timeout is not proof that
    // the device disconnected. A removed device simply rejects this request.
    {
        submit(slots[0], 0xffffffffu, true);
        drain_deadline = ns() + 2'000'000'000;
        while (pending() && ns() < drain_deadline) {
            pump();
            SDL_DelayPrecise(100'000);
        }
        if (pending()) {
            std::fprintf(stderr, "FATAL stop did not complete\n");
            std::fflush(stdout);
            ExitProcess(2);
        }
        stop_completed = slots[0].complete;
    }
    SDL_Delay(30);
    pump();
    events_running = false;
    if (!native) {
        libusb_interrupt_event_handler(context);
        events.join();
    }
    for (unsigned i = 0; i < depth; ++i) {
        if (native)
            CloseHandle(slots[i].overlap.hEvent);
        else
            libusb_free_transfer(slots[i].transfer);
    }
    if (native)
        CloseHandle(handle);
    else {
        libusb_release_interface(usb, 0);
        libusb_close(usb);
        libusb_exit(context);
    }
    if (pad)
        SDL_CloseGamepad(pad);
    SDL_Quit();
    std::printf("{\"event\":\"summary\",\"mode\":\"%s\",\"successful\":%u,\"failed\":%u,\"cancelled\":%u,\"coalesced\":%u,\"stop_request_ns\":%llu,\"stop_complete_ns\":%llu,\"first_device_error_ns\":%llu,\"pending_after_close\":0}\n", mode.c_str(), successful, failed, cancelled, coalesced, stop_requested, stop_completed, first_device_error.load());
    return failed ? 1 : 0;
}
