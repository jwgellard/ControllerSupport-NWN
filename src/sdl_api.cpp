#include "sdl_api.h"

#include <cstdint>
#include <cstring>

#include "log.h"

SdlApi sdl;

bool ResolveSdlApi(HMODULE exe)
{
    bool ok = true;
#define NWNC_RESOLVE(fn)                                                     \
    sdl.fn = reinterpret_cast<decltype(sdl.fn)>(GetProcAddress(exe, #fn));   \
    if (!sdl.fn) {                                                           \
        Log("missing SDL export: %s", #fn);                                  \
        ok = false;                                                          \
    }
    NWNC_SDL_FUNCS(NWNC_RESOLVE)
#undef NWNC_RESOLVE
    return ok;
}

void** FindDynApiSlot(HMODULE exe, const char* name, void* table, unsigned tableSize)
{
    auto p = reinterpret_cast<const uint8_t*>(GetProcAddress(exe, name));
    if (!p)
        return nullptr;
    int32_t disp;
    const uint8_t* next; // address the rip-relative displacement is based on
    if (p[0] == 0x48 && p[1] == 0xFF && p[2] == 0x25) // REX.W-prefixed form
        ++p;
    if (p[0] == 0xFF && p[1] == 0x25) { // jmp qword ptr [rip+disp32] (MSVC, nwmain.exe)
        memcpy(&disp, p + 2, sizeof(disp));
        next = p + 6;
    } else if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x05 && // mov rax, [rip+disp32]
               p[7] == 0x48 && p[8] == 0xFF && p[9] == 0xE0) { // jmp rax (MinGW, official SDL2.dll)
        memcpy(&disp, p + 3, sizeof(disp));
        next = p + 7;
    } else {
        return nullptr;
    }
    auto slot = const_cast<uint8_t*>(next) + disp;
    auto begin = static_cast<uint8_t*>(table);
    if (slot < begin || slot + sizeof(void*) > begin + tableSize)
        return nullptr;
    return reinterpret_cast<void**>(slot);
}
