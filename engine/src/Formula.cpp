#include "pulp/Formula.h"
#include <cctype>

namespace pulp {

namespace {
enum Code {
    Num, Var, Add, Sub, Mul, Div, Mod, Pow, Neg, Lt, Gt, Le, Ge, Eq,
    Sin, Cos, Tan, Abs, Sqrt, Exp, Log, Floor, Round, Frac, Min, Max, Clamp, If, WSine, WTri, WSaw, WSquare
};
struct Fn { const char* name; int code; int args; };
const Fn kFns[] = {
    {"sin", Sin, 1}, {"cos", Cos, 1}, {"tan", Tan, 1}, {"abs", Abs, 1}, {"sqrt", Sqrt, 1}, {"exp", Exp, 1}, {"log", Log, 1},
    {"floor", Floor, 1}, {"round", Round, 1}, {"frac", Frac, 1}, {"min", Min, 2}, {"max", Max, 2}, {"clamp", Clamp, 3}, {"if", If, 3},
    {"sine", WSine, 1}, {"tri", WTri, 1}, {"saw", WSaw, 1}, {"square", WSquare, 1}};
constexpr int kStack = 48;
}

struct FormulaParser {
    const std::string& s;
    const std::vector<std::string>& vars;
    std::vector<Formula::Op>& ops;
    size_t p = 0;
    std::string err;
    int depth = 0, maxDepth = 0;

    void emit(int code, double v = 0.0, int pops = 0, int pushes = 1) {
        ops.push_back({code, v});
        depth += pushes - pops;
        maxDepth = std::max(maxDepth, depth);
    }
    void skip() { while (p < s.size() && std::isspace((unsigned char)s[p])) ++p; }
    bool eat(const char* t) {
        skip();
        const size_t n = std::strlen(t);
        if (s.compare(p, n, t) == 0) { p += n; return true; }
        return false;
    }
    bool fail(const std::string& m) { if (err.empty()) err = m; return false; }

    bool expr() {
        if (!sum()) return false;
        skip();
        int code = -1;
        if (eat("<=")) code = Le; else if (eat(">=")) code = Ge; else if (eat("==")) code = Eq; else if (eat("<")) code = Lt; else if (eat(">")) code = Gt;
        if (code < 0) return true;
        if (!sum()) return false;
        emit(code, 0.0, 2);
        return true;
    }
    bool sum() {
        if (!term()) return false;
        for (;;) {
            if (eat("+")) { if (!term()) return false; emit(Add, 0.0, 2); }
            else if (eat("-")) { if (!term()) return false; emit(Sub, 0.0, 2); }
            else return true;
        }
    }
    bool term() {
        if (!unary()) return false;
        for (;;) {
            if (eat("*")) { if (!unary()) return false; emit(Mul, 0.0, 2); }
            else if (eat("/")) { if (!unary()) return false; emit(Div, 0.0, 2); }
            else if (eat("%")) { if (!unary()) return false; emit(Mod, 0.0, 2); }
            else return true;
        }
    }
    bool unary() {
        if (eat("-")) { if (!unary()) return false; emit(Neg, 0.0, 1); return true; }
        if (eat("+")) return unary();
        if (!primary()) return false;
        if (eat("^")) { if (!unary()) return false; emit(Pow, 0.0, 2); }
        return true;
    }
    bool primary() {
        skip();
        if (p >= s.size()) return fail("the formula ends too soon");
        if (eat("(")) {
            if (!expr()) return false;
            if (!eat(")")) return fail("a closing bracket is missing");
            return true;
        }
        const char c = s[p];
        if (std::isdigit((unsigned char)c) || c == '.') {
            size_t used = 0;
            double v = 0.0;
            try { v = std::stod(s.substr(p), &used); } catch (...) { return fail("that is not a number"); }
            p += used;
            emit(Num, v);
            return true;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t q = p;
            while (q < s.size() && (std::isalnum((unsigned char)s[q]) || s[q] == '_')) ++q;
            std::string name = s.substr(p, q - p);
            for (auto& ch : name) ch = (char)std::tolower((unsigned char)ch);
            p = q;
            skip();
            if (p < s.size() && s[p] == '(') {
                ++p;
                const Fn* fn = nullptr;
                for (auto& f : kFns) if (name == f.name) fn = &f;
                if (!fn) return fail("there is no function called " + name);
                for (int a = 0; a < fn->args; ++a) {
                    if (a > 0 && !eat(",")) return fail(name + " needs " + std::to_string(fn->args) + " values");
                    if (!expr()) return false;
                }
                if (!eat(")")) return fail("a closing bracket is missing after " + name);
                emit(fn->code, 0.0, fn->args);
                return true;
            }
            if (name == "pi") { emit(Num, kPi); return true; }
            if (name == "e") { emit(Num, 2.718281828459045); return true; }
            for (size_t i = 0; i < vars.size(); ++i) if (name == vars[i]) { emit(Var, (double)i); return true; }
            return fail("there is nothing called " + name);
        }
        return fail(std::string("unexpected '") + c + "'");
    }
};

std::shared_ptr<const Formula> Formula::compile(const std::string& text, const std::vector<std::string>& variables, std::string* error) {
    auto f = std::make_shared<Formula>();
    f->text_ = text;
    FormulaParser ps{text, variables, f->ops_};
    bool ok = ps.expr();
    ps.skip();
    if (ok && ps.p < text.size()) ok = ps.fail(std::string("unexpected '") + text[ps.p] + "'");
    if (ok && ps.maxDepth > kStack) ok = ps.fail("the formula is too deeply nested");
    if (!ok) { if (error) *error = ps.err; return nullptr; }
    return f;
}

double Formula::eval(const double* v) const {
    double st[kStack];
    int n = 0;
    auto wave = [](double x) { return x - std::floor(x); };
    for (const Op& o : ops_) {
        switch (o.code) {
            case Num: st[n++] = o.value; break;
            case Var: st[n++] = v[(int)o.value]; break;
            case Add: st[n - 2] += st[n - 1]; --n; break;
            case Sub: st[n - 2] -= st[n - 1]; --n; break;
            case Mul: st[n - 2] *= st[n - 1]; --n; break;
            case Div: st[n - 2] = st[n - 1] != 0.0 ? st[n - 2] / st[n - 1] : 0.0; --n; break;
            case Mod: st[n - 2] = st[n - 1] != 0.0 ? std::fmod(st[n - 2], st[n - 1]) : 0.0; --n; break;
            case Pow: st[n - 2] = std::pow(st[n - 2], st[n - 1]); --n; break;
            case Neg: st[n - 1] = -st[n - 1]; break;
            case Lt: st[n - 2] = st[n - 2] < st[n - 1] ? 1.0 : 0.0; --n; break;
            case Gt: st[n - 2] = st[n - 2] > st[n - 1] ? 1.0 : 0.0; --n; break;
            case Le: st[n - 2] = st[n - 2] <= st[n - 1] ? 1.0 : 0.0; --n; break;
            case Ge: st[n - 2] = st[n - 2] >= st[n - 1] ? 1.0 : 0.0; --n; break;
            case Eq: st[n - 2] = st[n - 2] == st[n - 1] ? 1.0 : 0.0; --n; break;
            case Sin: st[n - 1] = std::sin(st[n - 1]); break;
            case Cos: st[n - 1] = std::cos(st[n - 1]); break;
            case Tan: st[n - 1] = std::tan(st[n - 1]); break;
            case Abs: st[n - 1] = std::fabs(st[n - 1]); break;
            case Sqrt: st[n - 1] = st[n - 1] > 0.0 ? std::sqrt(st[n - 1]) : 0.0; break;
            case Exp: st[n - 1] = std::exp(st[n - 1]); break;
            case Log: st[n - 1] = st[n - 1] > 0.0 ? std::log(st[n - 1]) : 0.0; break;
            case Floor: st[n - 1] = std::floor(st[n - 1]); break;
            case Round: st[n - 1] = std::round(st[n - 1]); break;
            case Frac: st[n - 1] = wave(st[n - 1]); break;
            case Min: st[n - 2] = std::min(st[n - 2], st[n - 1]); --n; break;
            case Max: st[n - 2] = std::max(st[n - 2], st[n - 1]); --n; break;
            case Clamp: st[n - 3] = clampT(st[n - 3], st[n - 2], st[n - 1]); n -= 2; break;
            case If: st[n - 3] = st[n - 3] != 0.0 ? st[n - 2] : st[n - 1]; n -= 2; break;
            case WSine: st[n - 1] = 0.5 - 0.5 * std::cos(kTwoPi * st[n - 1]); break;
            case WTri: { const double x = wave(st[n - 1]); st[n - 1] = x < 0.5 ? 2.0 * x : 2.0 - 2.0 * x; break; }
            case WSaw: st[n - 1] = wave(st[n - 1]); break;
            case WSquare: st[n - 1] = wave(st[n - 1]) < 0.5 ? 1.0 : 0.0; break;
            default: break;
        }
    }
    const double r = n > 0 ? st[0] : 0.0;
    return std::isfinite(r) ? r : 0.0;
}

} // namespace pulp
