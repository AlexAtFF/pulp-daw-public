#pragma once
// A very small test framework: TEST(name) { CHECK(...); }
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace t {

struct Case { const char* name; std::function<void()> fn; };
std::vector<Case>& cases();
struct Reg { Reg(const char* n, std::function<void()> f) { cases().push_back({n, std::move(f)}); } };
extern int failures;
extern int checks;
void fail(const char* file, int line, const std::string& msg);
std::string tmpDir();

} // namespace t

#define TEST(name) \
    static void test_##name(); \
    static t::Reg reg_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond) do { ++t::checks; if (!(cond)) t::fail(__FILE__, __LINE__, #cond); } while (0)
#define CHECK_MSG(cond, ...) do { ++t::checks; if (!(cond)) { char b_[512]; snprintf(b_, sizeof b_, __VA_ARGS__); t::fail(__FILE__, __LINE__, std::string(#cond) + "  " + b_); } } while (0)
#define CHECK_NEAR(a, b, tol) do { ++t::checks; const double a_ = (double)(a), b_ = (double)(b); if (!(std::fabs(a_ - b_) <= (tol))) { char m_[256]; snprintf(m_, sizeof m_, "%s = %.9g, expected %.9g (tolerance %g)", #a, a_, b_, (double)(tol)); t::fail(__FILE__, __LINE__, m_); } } while (0)
