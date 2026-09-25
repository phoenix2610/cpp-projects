// The same matrix multiply, four ways, and the memory hierarchy explaining the gap.
//
//   g++ -std=c++23 -O3 -march=native matmul.cpp -o matmul && ./matmul 512
//
// All four do exactly 2*N^3 floating point operations. The only thing that changes
// is the order they touch memory in — which is the entire performance story:
//   naive     B is walked down a column, so every inner step is a new cache line
//   transposed  B stored column-major first, so both operands stream
//   blocked   work on tiles that fit in L1, reusing each loaded line many times
//   blocked+  the inner loop written so the compiler can vectorise it

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using Matrix = std::vector<float>;

static Matrix random_matrix(int n, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    Matrix m(std::size_t(n) * n);
    for (auto& v : m) v = dist(rng);
    return m;
}

static void naive(const Matrix& a, const Matrix& b, Matrix& c, int n) {
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            float sum = 0;
            for (int k = 0; k < n; ++k) sum += a[i * n + k] * b[k * n + j];   // b strides by n
            c[i * n + j] = sum;
        }
}

static void transposed(const Matrix& a, const Matrix& b, Matrix& c, int n) {
    Matrix bt(std::size_t(n) * n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) bt[j * n + i] = b[i * n + j];
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            float sum = 0;
            const float* row_a = &a[i * n];
            const float* row_b = &bt[j * n];
            for (int k = 0; k < n; ++k) sum += row_a[k] * row_b[k];           // both stream
            c[i * n + j] = sum;
        }
}

static void blocked(const Matrix& a, const Matrix& b, Matrix& c, int n, int block) {
    std::fill(c.begin(), c.end(), 0.0f);
    for (int ii = 0; ii < n; ii += block)
        for (int kk = 0; kk < n; kk += block)
            for (int jj = 0; jj < n; jj += block) {
                const int i_max = std::min(ii + block, n);
                const int k_max = std::min(kk + block, n);
                const int j_max = std::min(jj + block, n);
                for (int i = ii; i < i_max; ++i)
                    for (int k = kk; k < k_max; ++k) {
                        const float a_ik = a[i * n + k];          // hoisted: one load per k, not per j
                        const float* row_b = &b[k * n];
                        float* row_c = &c[i * n];
                        for (int j = jj; j < j_max; ++j) row_c[j] += a_ik * row_b[j];
                    }
            }
}

// same tiling, but the inner loop is a contiguous fused multiply-add the compiler can widen
static void blocked_simd(const float* __restrict a, const float* __restrict b, float* __restrict c,
                         int n, int block) {
    std::fill(c, c + std::size_t(n) * n, 0.0f);
    for (int ii = 0; ii < n; ii += block)
        for (int kk = 0; kk < n; kk += block)
            for (int i = ii; i < std::min(ii + block, n); ++i) {
                float* row_c = c + std::size_t(i) * n;
                for (int k = kk; k < std::min(kk + block, n); ++k) {
                    const float a_ik = a[std::size_t(i) * n + k];
                    const float* row_b = b + std::size_t(k) * n;
                    for (int j = 0; j < n; ++j) row_c[j] += a_ik * row_b[j];
                }
            }
}

static double max_difference(const Matrix& x, const Matrix& y) {
    double worst = 0;
    for (std::size_t i = 0; i < x.size(); ++i) worst = std::max(worst, double(std::fabs(x[i] - y[i])));
    return worst;
}

template <typename F>
static double time_ms(F&& fn) {
    auto start = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

int main(int argc, char** argv) {
    const int n = argc > 1 ? std::atoi(argv[1]) : 512;
    const double flops = 2.0 * double(n) * n * n;

    Matrix a = random_matrix(n, 1), b = random_matrix(n, 2);
    Matrix reference(std::size_t(n) * n), scratch(std::size_t(n) * n);

    std::printf("%dx%d float matrices (%.1f MB each, %.2f GFLOP of work)\n\n",
                n, n, double(n) * n * 4 / 1048576.0, flops / 1e9);

    double naive_ms = time_ms([&] { naive(a, b, reference, n); });
    std::printf("  %-22s %8.1fms  %6.2f GFLOP/s   1.00x\n", "naive (i,j,k)", naive_ms, flops / naive_ms / 1e6);

    struct Result { const char* name; double ms; double error; };
    std::vector<Result> results;

    double t_ms = time_ms([&] { transposed(a, b, scratch, n); });
    results.push_back({"transposed B", t_ms, max_difference(reference, scratch)});

    for (int block : {16, 32, 64, 128}) {
        char label[48];
        std::snprintf(label, sizeof(label), "blocked (%d)", block);
        double ms = time_ms([&] { blocked(a, b, scratch, n, block); });
        results.push_back({strdup(label), ms, max_difference(reference, scratch)});
    }

    double simd_ms = time_ms([&] { blocked_simd(a.data(), b.data(), scratch.data(), n, 64); });
    results.push_back({"blocked + restrict", simd_ms, max_difference(reference, scratch)});

    for (const auto& r : results)
        std::printf("  %-22s %8.1fms  %6.2f GFLOP/s  %5.2fx   max error %.1e\n",
                    r.name, r.ms, flops / r.ms / 1e6, naive_ms / r.ms, r.error);

    std::puts("\nwhy: bytes of B touched per inner-loop step");
    std::printf("  naive        stride %d floats (%d bytes) — a fresh cache line every step\n", n, n * 4);
    std::printf("  transposed   stride 1 float (4 bytes) — 16 useful floats per 64-byte line\n");
    std::printf("  blocked      a %dx%d tile of A, B and C is %.1f KB — sized to stay in L1\n",
                64, 64, 3.0 * 64 * 64 * 4 / 1024.0);

    std::puts("\nthe same effect in isolation: summing 64MB with different strides");
    {
        constexpr std::size_t kFloats = 16 << 20;
        std::vector<float> data(kFloats, 1.0f);
        for (int stride : {1, 4, 16, 64, 256}) {
            double ms = time_ms([&] {
                float sum = 0;
                for (std::size_t i = 0; i < kFloats; i += stride) sum += data[i];
                asm volatile("" :: "r"(&sum) : "memory");
            });
            double touched = double(kFloats / stride) * 4 / 1048576.0;
            std::printf("  stride %4d  %7.2fms for %6.1f MB of useful data  (%.1f MB/s effective)\n",
                        stride, ms, touched, touched / ms * 1000);
        }
    }
    return 0;
}
