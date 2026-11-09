#pragma once
// A small expression language for mapping formulas and the formula controller.
// Text is compiled once on a control thread; evaluating it allocates nothing.
//   numbers, + - * / % ^, comparisons (< > <= >= ==, giving 1 or 0), brackets,
//   pi, e, and the variables named when compiling,
//   sin cos tan abs sqrt exp log floor round frac min max clamp(x, lo, hi) if(cond, a, b)
//   sine(x) tri(x) saw(x) square(x): waves with a period of 1 that run from 0 to 1.
#include "pulp/Core.h"

namespace pulp {

class Formula {
public:
    static std::shared_ptr<const Formula> compile(const std::string& text, const std::vector<std::string>& variables, std::string* error = nullptr);
    double eval(const double* variables) const;         // variables in the order given to compile()
    const std::string& text() const { return text_; }
private:
    struct Op { int code; double value; };
    std::vector<Op> ops_;
    std::string text_;
    friend struct FormulaParser;
};

} // namespace pulp
