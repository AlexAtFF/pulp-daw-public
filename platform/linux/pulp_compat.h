// PULP on Linux (platform/linux/README.md). platform/linux/toolchain.cmake puts this file in front of every C++ file: the three
// Microsoft C-runtime calls the engine makes, and the one Windows-only JUCE class the program uses outside #if JUCE_WINDOWS.
#pragma once
#if !defined(_WIN32)
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64 // 64-bit file offsets on 32-bit systems too
#endif
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <wchar.h>

// The engine opens a file with the path as wide characters (UTF-32 on Linux); it goes back to UTF-8, the encoding of Linux
// file names, and on to fopen.
static inline FILE* _wfopen(const wchar_t* path, const wchar_t* mode) {
    if (!path || !mode) return NULL;
    size_t n = 0;
    while (path[n]) n++;
    char* name = (char*)malloc(n * 4 + 1);
    if (!name) return NULL;
    char* o = name;
    for (const wchar_t* p = path; *p; ++p) {
        const unsigned c = (unsigned)*p;
        if (c < 0x80) *o++ = (char)c;
        else if (c < 0x800) { *o++ = (char)(0xC0 | (c >> 6)); *o++ = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { *o++ = (char)(0xE0 | (c >> 12)); *o++ = (char)(0x80 | ((c >> 6) & 0x3F)); *o++ = (char)(0x80 | (c & 0x3F)); }
        else { *o++ = (char)(0xF0 | (c >> 18)); *o++ = (char)(0x80 | ((c >> 12) & 0x3F)); *o++ = (char)(0x80 | ((c >> 6) & 0x3F)); *o++ = (char)(0x80 | (c & 0x3F)); }
    }
    *o = 0;
    char m[8] = {0};
    for (int i = 0; i < 7 && mode[i]; ++i) m[i] = (char)mode[i];
    FILE* f = fopen(name, m);
    free(name);
    return f;
}
static inline int _fseeki64(FILE* f, long long offset, int origin) { return fseeko(f, (off_t)offset, origin); }
static inline long long _ftelli64(FILE* f) { return (long long)ftello(f); }

#ifdef __cplusplus
// The File menu's "open .pulp files with PULP" entry calls juce::WindowsRegistry, which JUCE has only on Windows: here it
// registers nothing and says so.
namespace juce {
struct WindowsRegistry {
    enum WoW64Mode { WoW64_Default };
    template <typename... Args> static bool registerFileAssociation(const Args&...) { return false; }
};
}
#endif
#endif
