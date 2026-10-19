// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime.h"
#include <intrin.h>
#include <stdio.h>
#include <string.h>
typedef struct { int64_t seconds, nanoseconds; } TimeSpec;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static uint64_t tsc_frequency, initial_tsc;
static uint64_t tick(void) { _mm_lfence(); uint64_t t = __rdtsc(); _mm_lfence(); return t; }
static BOOL CALLBACK initialize(PINIT_ONCE o, void *p, void **c) {
    (void)o; (void)p; (void)c;
    LARGE_INTEGER qfreq, begin, end; QueryPerformanceFrequency(&qfreq); QueryPerformanceCounter(&begin);
    uint64_t first = tick(); Sleep(40); uint64_t last = tick(); QueryPerformanceCounter(&end);
    tsc_frequency = (uint64_t)((double)(last-first) * (double)qfreq.QuadPart / (double)(end.QuadPart-begin.QuadPart));
    initial_tsc = tick();
    printf("Calibrated invariant TSC: %llu Hz\n", (unsigned long long)tsc_frequency); return TRUE;
}
static uint64_t GUEST_ABI frequency(void) { InitOnceExecuteOnce(&once,initialize,NULL,NULL); return tsc_frequency; }
static uint64_t GUEST_ABI process_time(void) {
    uint64_t f = frequency(), d = tick()-initial_tsc;
    return (d/f)*1000000 + ((d%f)*1000000)/f;
}
uint64_t runtime_tsc_frequency(void){return frequency();}
uint64_t runtime_process_time_us(void){return process_time();}
static int32_t GUEST_ABI clock_gettime(uint32_t id, TimeSpec *out) {
    if (!out) return (int32_t)UINT32_C(0x80020016);
    if (id == 0 || id == 9 || id == 10 || id == 13) {
        FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
        uint64_t n = (((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - UINT64_C(116444736000000000);
        out->seconds = (int64_t)(n/10000000); out->nanoseconds = (int64_t)((n%10000000)*100);
        if(id==13)out->nanoseconds=0;
        return 0;
    }
    if (id == 4 || id == 5 || id == 7 || id == 8 || id == 11 || id == 12) {
        LARGE_INTEGER counter,f; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&counter);
        out->seconds = counter.QuadPart/f.QuadPart;
        out->nanoseconds = (counter.QuadPart%f.QuadPart)*1000000000/f.QuadPart; return 0;
    }
    return (int32_t)UINT32_C(0x80020016);
}
static int GUEST_ABI posix_clock_gettime(uint32_t id,TimeSpec *out){int32_t r=clock_gettime(id,out);if(r){*runtime_errno()=r&65535;return -1;}return 0;}
static int32_t GUEST_ABI usleep(uint32_t us) {
    if (!us) { SwitchToThread(); return 0; }
    HANDLE timer = CreateWaitableTimerExW(NULL,NULL,0x2,TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(NULL,TRUE,NULL);
    if (!timer) return (int32_t)UINT32_C(0x8002000c);
    LARGE_INTEGER due; due.QuadPart = -(int64_t)us*10;
    BOOL set = SetWaitableTimer(timer,&due,0,NULL,NULL,FALSE);
    DWORD result = set ? WaitForSingleObject(timer,INFINITE) : WAIT_FAILED;
    CloseHandle(timer); return result == WAIT_OBJECT_0 ? 0 : (int32_t)UINT32_C(0x80020016);
}
uintptr_t time_resolve(const char *identity) {
    if (!strstr(identity,"#libkernel#1#libkernel#257")&&!strstr(identity,"#libScePosix#1#libkernel#257")) return 0;
    const char *api = runtime_api_name(identity);
    if (!strcmp(api,"sceKernelGetTscFrequency")) return (uintptr_t)frequency;
    if (!strcmp(api,"sceKernelGetProcessTime")) return (uintptr_t)process_time;
    if (!strcmp(api,"sceKernelClockGettime")) return (uintptr_t)clock_gettime;
    if (!strcmp(api,"sceKernelUsleep")) return (uintptr_t)usleep;
    if (!strcmp(api,"clock_gettime")||!strcmp(api,"_clock_gettime")) return (uintptr_t)posix_clock_gettime;
    return 0;
}
