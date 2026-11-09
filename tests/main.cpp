// Test runner. Usage: pulp_tests [filter-substring]
#include "Test.h"
#include "pulp/Core.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#ifdef _DEBUG
#include <crtdbg.h>
#endif

namespace t {
std::vector<Case>& cases() { static std::vector<Case> c; return c; }
int failures = 0;
int checks = 0;
static int failuresInCase = 0;
void fail(const char* file, int line, const std::string& msg) {
    ++failures;
    if (++failuresInCase <= 8) printf("    FAIL %s:%d  %s\n", file, line, msg.c_str());
}
std::string tmpDir() {
    auto p = std::filesystem::path(PULP_SOURCE_DIR) / "build" / "test-tmp";
    std::filesystem::create_directories(p);
    return p.string();
}
}

// Any allocation while a thread is inside the audio callback is counted and makes
// the real-time safety test fail.
void* operator new(std::size_t n) {
    if (pulp::Rt::inRt()) pulp::Rt::allocationsInRt.fetch_add(1);
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
#ifdef _DEBUG
    // Debug test runs use the checked heap: overruns, double frees and leaks are reported and fail the run.
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF | _CRTDBG_CHECK_EVERY_1024_DF);
    for (int kind : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) { _CrtSetReportMode(kind, _CRTDBG_MODE_FILE); _CrtSetReportFile(kind, _CRTDBG_FILE_STDERR); }
    printf("debug build: checked heap is on\n");
#endif
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0, failedCases = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (auto& c : t::cases()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        const int before = t::failures;
        t::failuresInCase = 0;
        const auto c0 = std::chrono::steady_clock::now();
        try { c.fn(); }
        catch (const std::exception& e) { t::fail("exception", 0, e.what()); }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
        ++ran;
        const bool ok = t::failures == before;
        if (!ok) ++failedCases;
        printf("%s  %-52s %7.0f ms\n", ok ? "ok  " : "FAIL", c.name, ms);
        fflush(stdout);
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("\n%d tests, %d checks, %d failed tests (%.1f s)\n", ran, t::checks, failedCases, s);
    return failedCases ? 1 : 0;
}
