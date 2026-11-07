/* SPDX-License-Identifier: Zlib
 * Input-only native Windows HID contrast. No driver installation or output writes.
 * Run after the SDL input probe to retain the device's volatile reporting setup.
 */
#include <SDL3/SDL.h>
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include <stdio.h>
#include <string.h>

static HANDLE open_input(USHORT *length)
{
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO list = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    HANDLE result = INVALID_HANDLE_VALUE;
    for (DWORD i = 0; ; ++i) {
        SP_DEVICE_INTERFACE_DATA entry = { sizeof(entry) };
        if (!SetupDiEnumDeviceInterfaces(list, NULL, &guid, i, &entry)) break;
        DWORD bytes = 0;
        SetupDiGetDeviceInterfaceDetailW(list, &entry, NULL, 0, &bytes, NULL);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = SDL_calloc(1, bytes);
        if (!detail) break;
        detail->cbSize = sizeof(*detail);
        if (SetupDiGetDeviceInterfaceDetailW(list, &entry, detail, bytes, NULL, NULL)) {
            HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
            HIDD_ATTRIBUTES attr = { sizeof(attr) };
            PHIDP_PREPARSED_DATA parsed = NULL;
            HIDP_CAPS caps;
            if (h != INVALID_HANDLE_VALUE && HidD_GetAttributes(h, &attr) && attr.VendorID == 0x057e &&
                attr.ProductID == 0x2069 && HidD_GetPreparsedData(h, &parsed)) {
                if (HidP_GetCaps(parsed, &caps) == HIDP_STATUS_SUCCESS && caps.InputReportByteLength == 64) {
                    result = h; *length = caps.InputReportByteLength;
                }
                HidD_FreePreparsedData(parsed);
            }
            if (h != INVALID_HANDLE_VALUE && h != result) CloseHandle(h);
        }
        SDL_free(detail);
        if (result != INVALID_HANDLE_VALUE) break;
    }
    SetupDiDestroyDeviceInfoList(list);
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "--capture")) { puts("Usage: testswitch2input_native --capture"); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);
    USHORT length = 0;
    HANDLE h = open_input(&length);
    if (h == INVALID_HANDLE_VALUE) { puts("SKIP no accessible native HID 057e:2069"); return 77; }
    HidD_SetNumInputBuffers(h, 512);
    HidD_FlushQueue(h);
    OVERLAPPED ov[2] = { 0 };
    unsigned char data[2][64];
    HANDLE events[2] = { NULL, NULL };
    bool pending[2] = { false, false };
    unsigned failures = 0, count = 0;
    for (unsigned i = 0; i < 2; ++i) {
        events[i] = ov[i].hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!events[i]) { ++failures; break; }
    }
    FILETIME ft;
    Uint64 before = SDL_GetTicksNS();
    GetSystemTimePreciseAsFileTime(&ft);
    Uint64 after = SDL_GetTicksNS();
    unsigned long long ticks = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    printf("ANCHOR ns=%llu filetime_100ns=%llu uncertainty_ns=%llu\n",
           (unsigned long long)((before + after) / 2), ticks, (unsigned long long)(after - before));
    Uint64 begin = SDL_GetTicksNS(), until = begin + SDL_MS_TO_NS(6000);
    printf("PHASE native ns=%llu\n", (unsigned long long)begin);
    while (!failures && SDL_GetTicksNS() < until) {
        for (unsigned i = 0; i < 2; ++i) if (!pending[i]) {
            ResetEvent(events[i]);
            if (!ReadFile(h, data[i], length, NULL, &ov[i]) && GetLastError() != ERROR_IO_PENDING) {
                ++failures; break;
            }
            pending[i] = true;
        }
        if (failures) break;
        DWORD ready = WaitForMultipleObjects(2, events, FALSE, 20);
        if (ready == WAIT_TIMEOUT) continue;
        if (ready < WAIT_OBJECT_0 || ready >= WAIT_OBJECT_0 + 2) { ++failures; break; }
        unsigned i = ready - WAIT_OBJECT_0;
        DWORD bytes = 0;
        if (!GetOverlappedResult(h, &ov[i], &bytes, FALSE)) { ++failures; break; }
        pending[i] = false;
        Uint64 at = SDL_GetTicksNS();
        char hex[129];
        static const char digits[] = "0123456789abcdef";
        for (DWORD j = 0; j < bytes; ++j) { hex[j*2] = digits[data[i][j] >> 4]; hex[j*2+1] = digits[data[i][j] & 15]; }
        hex[bytes*2] = 0;
        printf("Switch2 raw IN: ns=%llu size=%lu data=%s\n", (unsigned long long)at, bytes, hex);
        ++count;
    }
    Uint64 end = SDL_GetTicksNS();
    CancelIoEx(h, NULL);
    for (unsigned i = 0; i < 2; ++i) {
        if (pending[i]) { DWORD bytes; GetOverlappedResult(h, &ov[i], &bytes, TRUE); }
        if (events[i]) CloseHandle(events[i]);
    }
    CloseHandle(h);
    printf("SAMPLE phase=native begin_ns=%llu end_ns=%llu gyro=0 accel=0 axes=0 buttons=0\n",
           (unsigned long long)begin, (unsigned long long)end);
    printf("RESULT native reports=%u failures=%u\n", count, failures);
    return failures ? 1 : 0;
}
