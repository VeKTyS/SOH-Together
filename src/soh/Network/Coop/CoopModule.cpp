// Where soh.exe sits in memory. Used to send pointers to functions / static data
// (like an enemy's current action) as "offset from the start of the exe": both
// games are the same exe, but Windows loads it at a different address each time.
// Own file because windows.h doesn't get along with the game headers.

#include "soh/Network/Coop/CoopGame.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace CoopGame {

bool ExeRange(uintptr_t& start, uintptr_t& end) {
#ifdef _WIN32
    static uintptr_t sStart = 0;
    static uintptr_t sEnd = 0;
    if (sStart == 0) {
        auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        if (base == nullptr) {
            return false;
        }
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        sStart = reinterpret_cast<uintptr_t>(base);
        sEnd = sStart + nt->OptionalHeader.SizeOfImage;
    }
    start = sStart;
    end = sEnd;
    return true;
#else
    return false; // other platforms: exe pointers are just not copied
#endif
}

} // namespace CoopGame
