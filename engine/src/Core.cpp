#include "pulp/Core.h"

namespace pulp {
thread_local int Rt::depth = 0;
std::atomic<int64> Rt::allocationsInRt{0};
}
