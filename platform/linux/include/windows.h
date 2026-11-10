// PULP on Linux (platform/linux/README.md): the few Win32 calls PULP's checks make, on POSIX. Only a Linux build has this folder on
// its include path. PULP's program includes <windows.h> only inside #if JUCE_WINDOWS, so on Linux this serves the MP3 damage check
// alone: memory fenced by pages that may not be touched (mmap / mprotect), and its probe that such a page really stops a read.
#pragma once
#if !defined(_WIN32)
#include <csetjmp>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

typedef uint32_t DWORD;
typedef int BOOL;
typedef void* LPVOID;
typedef size_t SIZE_T;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define EXCEPTION_EXECUTE_HANDLER 1

struct SYSTEM_INFO { DWORD dwPageSize; };
inline void GetSystemInfo(SYSTEM_INFO* info) { info->dwPageSize = (DWORD)sysconf(_SC_PAGESIZE); }

namespace pulp_win32 {
inline std::mutex& sizesLock() { static std::mutex m; return m; }
inline std::map<void*, size_t>& sizes() { static std::map<void*, size_t> m; return m; } // VirtualFree(MEM_RELEASE) frees a whole block
inline int prot(DWORD p) { return p == PAGE_NOACCESS ? PROT_NONE : p == PAGE_READONLY ? PROT_READ : PROT_READ | PROT_WRITE; }
}
inline LPVOID VirtualAlloc(LPVOID, SIZE_T size, DWORD, DWORD protect) {
    void* p = mmap(nullptr, size, pulp_win32::prot(protect), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    std::lock_guard<std::mutex> g(pulp_win32::sizesLock());
    pulp_win32::sizes()[p] = size;
    return p;
}
inline BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, DWORD* old) {
    if (old) *old = PAGE_READWRITE;
    return mprotect(address, size, pulp_win32::prot(protect)) == 0;
}
inline BOOL VirtualFree(LPVOID address, SIZE_T, DWORD) {
    size_t size = 0;
    {
        std::lock_guard<std::mutex> g(pulp_win32::sizesLock());
        auto it = pulp_win32::sizes().find(address);
        if (it == pulp_win32::sizes().end()) return FALSE;
        size = it->second;
        pulp_win32::sizes().erase(it);
    }
    return munmap(address, size) == 0;
}

// __try / __except around a read that may fault: while the guard lives, SIGSEGV or SIGBUS jumps back into the __try's condition,
// which then takes the __except branch; the guard puts the previous handlers back when the statement ends. (libstdc++ defines its
// own __try for its headers, which are all included before this one.)
namespace pulp_win32 {
inline thread_local sigjmp_buf faultJump;
inline void onFault(int) { siglongjmp(faultJump, 1); }
struct FaultGuard {
    struct sigaction oldSegv {}, oldBus {};
    FaultGuard() {
        struct sigaction sa {};
        sa.sa_handler = onFault;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &oldSegv);
        sigaction(SIGBUS, &sa, &oldBus);
    }
    ~FaultGuard() {
        sigaction(SIGSEGV, &oldSegv, nullptr);
        sigaction(SIGBUS, &oldBus, nullptr);
    }
};
}
#undef __try
#define __try if (pulp_win32::FaultGuard pulp_fault_guard_; sigsetjmp(pulp_win32::faultJump, 1) == 0)
#define __except(filter) else
#endif
