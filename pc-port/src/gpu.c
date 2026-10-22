// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime.h"
#include "../gpu/iss_gpu.h"
#include "../gpu/iss_dialog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uintptr_t (*resolve_import)(const char *);
static int (*handle_fault)(void *,void *);
static int (*readback_memory)(uint64_t,uint64_t);
static int (*invalidate_memory)(uint64_t,uint64_t);
static void (*publish_feedback_identity)(uint64_t,uint64_t,uint64_t);
// Every thread's stack into the session's crash-threads.txt (gpu/freeze_watchdog.cpp).
static void (*dump_threads)(unsigned long);
// A pack of the game opened: its shaders go first in the ahead-of-time compiler (ISS_AOT_COMPUTE).
static void (*file_opened)(const char *);
// The save data dialog drawn by the DLL over the game (gpu/save_dialog_ui.cpp).
static int (*dialog_open)(const IssDialog *);
static int (*dialog_poll)(IssDialogResult *);
static void (*dialog_close)(void);
static int (*dialog_active)(void);
// The window's first size: 1280x720, or 720 high in the aspect of ISS_RESOLUTION (native/ultrawide.cpp),
// narrowed to 90% of the screen. The picture is fitted to whatever size the window gets.
static void window_size(uint32_t *width,uint32_t *height){
    *width=1280;*height=720;
    const char *resolution=getenv("ISS_RESOLUTION");unsigned w=0,h=0;
    if(!resolution||sscanf(resolution,"%ux%u",&w,&h)!=2||!w||!h)return;
    const double aspect=(double)w/h,limit=GetSystemMetrics(SM_CXSCREEN)*0.9;
    double window_w=720*aspect,window_h=720;
    if(limit>0&&window_w>limit){window_w=limit;window_h=limit/aspect;}
    *width=(uint32_t)window_w&~1u;*height=(uint32_t)window_h&~1u;
}
int gpu_start(uint32_t sdk_version,uint64_t image_base){
    const char *enabled=getenv("ISS_GPU");if(!enabled||strcmp(enabled,"1"))return 0;
    char path[2048];GetModuleFileNameA(NULL,path,sizeof(path));char *slash=strrchr(path,'\\');if(!slash)return -1;strcpy(slash,"\\iss-gpu.dll");
    HMODULE dll=LoadLibraryA(path);if(!dll){fprintf(stderr,"GPU DLL load failed: %lu\n",GetLastError());return -1;}
    int (*initialize)(const IssGpuCallbacks *)=(void *)GetProcAddress(dll,"iss_gpu_initialize");
    resolve_import=(void *)GetProcAddress(dll,"iss_gpu_resolve");handle_fault=(void *)GetProcAddress(dll,"iss_gpu_fault");
    readback_memory=(void *)GetProcAddress(dll,"iss_gpu_readback");
    invalidate_memory=(void *)GetProcAddress(dll,"iss_gpu_invalidate");
    publish_feedback_identity=(void *)GetProcAddress(dll,"iss_gpu_feedback_identity");
    file_opened=(void *)GetProcAddress(dll,"iss_gpu_file_opened");
    void (*mapped)(uint64_t,uint64_t,uint8_t *)=(void *)GetProcAddress(dll,"iss_gpu_map");
    dialog_open=(void *)GetProcAddress(dll,"iss_gpu_dialog_open");dialog_poll=(void *)GetProcAddress(dll,"iss_gpu_dialog_poll");
    dialog_close=(void *)GetProcAddress(dll,"iss_gpu_dialog_close");dialog_active=(void *)GetProcAddress(dll,"iss_gpu_dialog_active");
    dump_threads=(void *)GetProcAddress(dll,"iss_gpu_dump_threads");
    if(!initialize||!resolve_import||!handle_fault||!mapped)return -1;
    IssGpuCallbacks callbacks={0};callbacks.struct_size=sizeof(callbacks);callbacks.sdk_version=sdk_version;
    callbacks.psf_attributes=545259522;window_size(&callbacks.width,&callbacks.height);
    callbacks.serial=runtime_title_id();callbacks.title=runtime_game_name();callbacks.user_dir=runtime_user_root();
    callbacks.tsc_frequency=runtime_tsc_frequency;callbacks.process_time_us=runtime_process_time_us;
    callbacks.errno_location=runtime_errno;callbacks.backing=memory_backing;callbacks.clamp=memory_clamp;
    callbacks.visit_mappings=memory_visit;callbacks.protect=memory_gpu_protect;
    callbacks.image_base=image_base;
    callbacks.mapping_epoch=memory_mapping_epoch;
    callbacks.mapping_lock_shared=memory_mapping_lock_shared;
    callbacks.mapping_unlock_shared=memory_mapping_unlock_shared;
    if(initialize(&callbacks))return -1;memory_set_map_hook(mapped);
    puts("Vulkan renderer initialized; waiting for guest graphics submissions.");return 0;
}
uintptr_t gpu_resolve(const char *name){return resolve_import?resolve_import(name):0;}
int gpu_dialog_available(void){return dialog_open&&dialog_poll&&dialog_close;}
int gpu_dialog_open(const void *dialog){return dialog_open?dialog_open((const IssDialog *)dialog):-1;}
int gpu_dialog_poll(void *result){return dialog_poll?dialog_poll((IssDialogResult *)result):-1;}
void gpu_dialog_close(void){if(dialog_close)dialog_close();}
int gpu_dialog_active(void){return dialog_active?dialog_active():0;}
int gpu_readback(uint64_t address,uint64_t size){return readback_memory?readback_memory(address,size):0;}
int gpu_invalidate(uint64_t address,uint64_t size){return invalidate_memory?invalidate_memory(address,size):0;}
void gpu_file_opened(const char *guest_path){if(file_opened)file_opened(guest_path);}
int gpu_feedback_identity_available(void){return publish_feedback_identity!=NULL;}
void gpu_feedback_identity(uint64_t owner_object,uint64_t shader_object,uint64_t record_address){
    if(publish_feedback_identity)publish_feedback_identity(owner_object,shader_object,record_address);
}
int gpu_handle_fault(EXCEPTION_POINTERS *e){
    if(!handle_fault||e->ExceptionRecord->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION||e->ExceptionRecord->NumberParameters<2)return 0;
    return handle_fault(e,(void *)e->ExceptionRecord->ExceptionInformation[1]);
}
static DWORD WINAPI dump_threads_thread(void *tid){dump_threads((unsigned long)(uintptr_t)tid);return 0;}
// On its own thread, at most 3 s: a thread stopped in the middle of the heap or the loader must not
// keep a crashed game from ending.
// ISS_CRASH_THREADS=1 (the launcher's "Freeze and crash reports").
void gpu_dump_threads(unsigned long tid){
    const char *enabled=getenv("ISS_CRASH_THREADS");
    if(!dump_threads||!enabled||strcmp(enabled,"1"))return;
    HANDLE t=CreateThread(NULL,0,dump_threads_thread,(void *)(uintptr_t)tid,0,NULL);
    if(t){WaitForSingleObject(t,3000);CloseHandle(t);}
}
