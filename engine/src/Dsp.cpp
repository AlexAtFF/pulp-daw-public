#include "pulp/Dsp.h"

namespace pulp::dsp {

void fft(float* re, float* im, int n, bool inverse) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = kTwoPi / len * (inverse ? 1.0 : -1.0);
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (int k = 0; k < len / 2; ++k) {
                const int a = i + k, b = i + k + len / 2;
                const double xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
                re[b] = (float)(re[a] - xr); im[b] = (float)(im[a] - xi);
                re[a] = (float)(re[a] + xr); im[a] = (float)(im[a] + xi);
                const double t = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = t;
            }
        }
    }
    if (inverse) for (int i = 0; i < n; ++i) { re[i] /= (float)n; im[i] /= (float)n; }
}

} // namespace pulp::dsp
