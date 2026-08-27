// A path tracer: spheres, three materials, anti-aliasing, threads, PPM out.
//
//   g++ -std=c++23 -O3 -march=native -pthread raytrace.cpp -o raytrace
//   ./raytrace --width 600 --samples 64 --out render.ppm
//
// Every pixel fires rays that bounce until they hit a light or run out of depth,
// averaging what comes back. That is the whole algorithm — the realism (soft
// shadows, colour bleeding, glossy reflection, refraction with total internal
// reflection) is not special-cased anywhere; it falls out of sampling directions
// randomly and letting the material decide how the ray leaves the surface.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

struct Vec3 {
    double x = 0, y = 0, z = 0;
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3 operator*(const Vec3& o) const { return {x * o.x, y * o.y, z * o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    double length_squared() const { return x * x + y * y + z * z; }
    double length() const { return std::sqrt(length_squared()); }
    Vec3 normalized() const { double l = length(); return l > 0 ? *this * (1 / l) : *this; }
};
static double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static Vec3 reflect(const Vec3& v, const Vec3& n) { return v - n * (2 * dot(v, n)); }
static Vec3 refract(const Vec3& uv, const Vec3& n, double ratio) {
    double cos_theta = std::min(dot(-uv, n), 1.0);
    Vec3 perpendicular = (uv + n * cos_theta) * ratio;
    Vec3 parallel = n * -std::sqrt(std::fabs(1.0 - perpendicular.length_squared()));
    return perpendicular + parallel;
}

struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed * 6364136223846793005ULL + 1442695040888963407ULL) {}
    double next() {                                  // xorshift: fast enough to call millions of times
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        return double(state >> 11) * (1.0 / 9007199254740992.0);
    }
    double range(double lo, double hi) { return lo + (hi - lo) * next(); }
    Vec3 unit_vector() {
        for (;;) {                                   // rejection sampling: uniform on the sphere
            Vec3 p{range(-1, 1), range(-1, 1), range(-1, 1)};
            double length_squared = p.length_squared();
            if (length_squared > 1e-8 && length_squared <= 1) return p * (1 / std::sqrt(length_squared));
        }
    }
};

struct Ray { Vec3 origin, direction; };

enum class Material { Diffuse, Metal, Glass };

struct Sphere {
    Vec3 center;
    double radius;
    Material material;
    Vec3 albedo;
    double fuzz = 0, index = 1.5;
};

struct Hit {
    double t;
    Vec3 point, normal;
    bool front_face;
    const Sphere* sphere;
};

static bool hit_sphere(const Sphere& sphere, const Ray& ray, double t_min, double t_max, Hit& hit) {
    Vec3 oc = ray.origin - sphere.center;
    double a = ray.direction.length_squared();
    double half_b = dot(oc, ray.direction);
    double c = oc.length_squared() - sphere.radius * sphere.radius;
    double discriminant = half_b * half_b - a * c;
    if (discriminant < 0) return false;
    double root = (-half_b - std::sqrt(discriminant)) / a;
    if (root < t_min || root > t_max) {
        root = (-half_b + std::sqrt(discriminant)) / a;
        if (root < t_min || root > t_max) return false;
    }
    hit.t = root;
    hit.point = ray.origin + ray.direction * root;
    Vec3 outward = (hit.point - sphere.center) * (1 / sphere.radius);
    hit.front_face = dot(ray.direction, outward) < 0;
    hit.normal = hit.front_face ? outward : -outward;
    hit.sphere = &sphere;
    return true;
}

static Vec3 ray_color(const Ray& ray, const std::vector<Sphere>& world, Rng& rng, int depth,
                      std::atomic<std::uint64_t>& ray_count) {
    if (depth <= 0) return {};
    ray_count.fetch_add(1, std::memory_order_relaxed);

    Hit hit{}, best{};
    bool found = false;
    double closest = 1e30;
    for (const Sphere& sphere : world)
        if (hit_sphere(sphere, ray, 0.001, closest, hit)) { closest = hit.t; best = hit; found = true; }

    if (!found) {                                       // the sky is the only light source here
        Vec3 unit = ray.direction.normalized();
        double t = 0.5 * (unit.y + 1.0);
        return Vec3{1, 1, 1} * (1.0 - t) + Vec3{0.5, 0.7, 1.0} * t;
    }

    const Sphere& sphere = *best.sphere;
    if (sphere.material == Material::Diffuse) {
        Vec3 direction = best.normal + rng.unit_vector();
        if (direction.length_squared() < 1e-8) direction = best.normal;
        return sphere.albedo * ray_color({best.point, direction}, world, rng, depth - 1, ray_count);
    }
    if (sphere.material == Material::Metal) {
        Vec3 reflected = reflect(ray.direction.normalized(), best.normal) + rng.unit_vector() * sphere.fuzz;
        if (dot(reflected, best.normal) <= 0) return {};
        return sphere.albedo * ray_color({best.point, reflected}, world, rng, depth - 1, ray_count);
    }
    // glass: refract, unless the angle makes that impossible (total internal reflection)
    double ratio = best.front_face ? (1.0 / sphere.index) : sphere.index;
    Vec3 unit = ray.direction.normalized();
    double cos_theta = std::min(dot(-unit, best.normal), 1.0);
    double sin_theta = std::sqrt(1.0 - cos_theta * cos_theta);
    double r0 = (1 - ratio) / (1 + ratio);
    r0 = r0 * r0;
    double reflectance = r0 + (1 - r0) * std::pow(1 - cos_theta, 5);      // Schlick approximation
    Vec3 direction = (ratio * sin_theta > 1.0 || reflectance > rng.next())
                         ? reflect(unit, best.normal)
                         : refract(unit, best.normal, ratio);
    return ray_color({best.point, direction}, world, rng, depth - 1, ray_count);
}

int main(int argc, char** argv) {
    int width = 400, samples = 32, max_depth = 24;
    std::string out_path = "/tmp/render.ppm";
    unsigned threads = std::thread::hardware_concurrency();
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--width" && i + 1 < argc) width = std::atoi(argv[++i]);
        else if (arg == "--samples" && i + 1 < argc) samples = std::atoi(argv[++i]);
        else if (arg == "--depth" && i + 1 < argc) max_depth = std::atoi(argv[++i]);
        else if (arg == "--threads" && i + 1 < argc) threads = unsigned(std::atoi(argv[++i]));
        else if (arg == "--out" && i + 1 < argc) out_path = argv[++i];
    }
    const int height = int(width / (16.0 / 9.0));

    std::vector<Sphere> world = {
        {{0, -1000.5, -1}, 1000, Material::Diffuse, {0.55, 0.55, 0.5}},
        {{0, 0, -1.2}, 0.5, Material::Diffuse, {0.75, 0.25, 0.25}},
        {{-1.05, 0, -1.2}, 0.5, Material::Glass, {1, 1, 1}, 0, 1.5},
        {{-1.05, 0, -1.2}, -0.4, Material::Glass, {1, 1, 1}, 0, 1.5},      // hollow: a bubble
        {{1.05, 0, -1.2}, 0.5, Material::Metal, {0.8, 0.75, 0.4}, 0.05},
        {{0.35, -0.32, -0.55}, 0.18, Material::Metal, {0.85, 0.85, 0.9}, 0.35},
        {{-0.45, -0.35, -0.6}, 0.15, Material::Diffuse, {0.2, 0.4, 0.75}},
    };

    Vec3 look_from{0, 0.35, 1.2}, look_at{0, 0, -1.2}, up{0, 1, 0};
    double focal = (look_from - look_at).length();
    double viewport_height = 2.0 * std::tan(0.5 * (50.0 * M_PI / 180.0)) * focal;
    double viewport_width = viewport_height * (double(width) / height);
    Vec3 w = (look_from - look_at).normalized();
    Vec3 u = cross(up, w).normalized();
    Vec3 v = cross(w, u);
    Vec3 horizontal = u * viewport_width, vertical = v * viewport_height;
    Vec3 lower_left = look_from - horizontal * 0.5 - vertical * 0.5 - w * focal;

    std::vector<Vec3> framebuffer(std::size_t(width) * height);
    std::atomic<int> next_row{0};
    std::atomic<std::uint64_t> ray_count{0};
    auto start = std::chrono::steady_clock::now();

    auto worker = [&] {
        for (;;) {
            int y = next_row.fetch_add(1);
            if (y >= height) return;
            Rng rng(std::uint64_t(y) * 9781 + 1);
            for (int x = 0; x < width; ++x) {
                Vec3 accumulated;
                for (int s = 0; s < samples; ++s) {
                    // jitter inside the pixel: this is the anti-aliasing, nothing else to it
                    double su = (x + rng.next()) / (width - 1);
                    double sv = (y + rng.next()) / (height - 1);
                    Ray ray{look_from, lower_left + horizontal * su + vertical * sv - look_from};
                    accumulated += ray_color(ray, world, rng, max_depth, ray_count);
                }
                framebuffer[std::size_t(height - 1 - y) * width + x] = accumulated * (1.0 / samples);
            }
        }
    };

    std::vector<std::thread> pool;
    for (unsigned t = 0; t < std::max(1u, threads); ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::ofstream file(out_path, std::ios::binary);
    file << "P6\n" << width << " " << height << "\n255\n";
    for (const Vec3& pixel : framebuffer) {
        auto encode = [](double channel) {
            channel = std::sqrt(std::clamp(channel, 0.0, 1.0));       // gamma 2.0
            return char(int(255.99 * channel));
        };
        char rgb[3] = {encode(pixel.x), encode(pixel.y), encode(pixel.z)};
        file.write(rgb, 3);
    }

    std::printf("%dx%d, %d samples/pixel, depth %d, %u threads\n", width, height, samples, max_depth,
                std::max(1u, threads));
    std::printf("  %llu rays traced in %.2fs = %.2f million rays/second\n",
                (unsigned long long)ray_count.load(), seconds, ray_count.load() / seconds / 1e6);
    std::printf("  %.1f rays per pixel on average (bounces included)\n",
                double(ray_count.load()) / (double(width) * height));
    std::printf("  wrote %s (%zu bytes)\n", out_path.c_str(),
                std::size_t(file.tellp()));

    // a coarse ASCII preview so the render is visible without an image viewer
    std::puts("\n  preview:");
    const char* ramp = " .:-=+*#%@";
    for (int y = 0; y < 22; ++y) {
        std::string line = "  ";
        for (int x = 0; x < 66; ++x) {
            const Vec3& pixel = framebuffer[std::size_t(y * height / 22) * width + (x * width / 66)];
            double luma = std::sqrt(std::clamp(0.2126 * pixel.x + 0.7152 * pixel.y + 0.0722 * pixel.z, 0.0, 1.0));
            line += ramp[std::min(9, int(luma * 9.999))];
        }
        std::puts(line.c_str());
    }
    return 0;
}
