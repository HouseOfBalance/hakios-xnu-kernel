// mali-probe: hakios-mali on the phone's Mali-G52, stage by stage, each with its verdict and, when it fails, why.
//
//   mali-probe            every stage against the kernel's HakiosMali (waits up to 60 s for the service)
//   mali-probe --fake     the same stages against the fake GPU, in this process (any machine)
//   mali-probe --dump     also print every frame's descriptors as Mesa's printers say them
//   mali-probe --until T  nothing but wait until the system has been up T seconds (a job list's clock: pictures
//                         taken at the run's end, which is a time since boot, wherever launchd started the list)
//   mali-probe --show N   only the triangle, then it on the panel for N seconds, sixteen times as big (a picture of
//                         the test for fbgrab: put again five times a second, as the window server draws over it)
//
// Each verdict is a line "mali-probe: <stage> PASS|FAIL|MEASURE ..." on standard output (the console, when launchd's
// job com.hakios.demo runs it from etc/rc.jobs) and, on the phone, the same line with "!!" through kern.hakios.say,
// which the kernel keeps for the host however long the run (tools/xnu-cycle.sh appends them to the log).
//
// The expected pixels are computed here from the colour, independently of the library (round(c * 255), to even, as
// pan_clear.c rounds), and every byte of a target's buffer outside the pixels the frame covers must keep the canary.
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

#include <sys/types.h>
#include <sys/sysctl.h>

#include "fake_gpu.hpp"
#include "mali.hpp"
#if defined(HAKIOS_MALI_COMPILER)
#include "compiler.h"
#endif
#include "draw.hpp"
#include "pass.hpp"
#include "shaders_v7.h"

using namespace hakios::mali;

namespace {

bool gDump = false, gFake = false;
int gShow = 0;
unsigned gPassed = 0, gFailed = 0;

void say(const char* format, ...) __attribute__((format(printf, 1, 2)));
void say(const char* format, ...) {
    char line[400];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    printf("mali-probe: %s\n", line);
#if defined(__APPLE__)
    if (!gFake) {
        // the kernel keeps 200 characters of a "!!" line (hakios_host_keep_add)
        char kept[190];
        snprintf(kept, sizeof(kept), "!!mali-probe: %s", line);
        sysctlbyname("kern.hakios.say", nullptr, nullptr, kept, strlen(kept));
    }
#endif
}

bool verdict(const char* stage, bool ok, const std::string& detail) {
    say("%s %s %s", stage, ok ? "PASS" : "FAIL", detail.c_str());
    (ok ? gPassed : gFailed)++;
    return ok;
}

uint8_t unorm8(float f) {
    f = f < 0 ? 0 : (f > 1 ? 1 : f);
    return static_cast<uint8_t>(std::nearbyint(f * 255.0f));      // the default rounding: to nearest, ties to even
}

std::string hex(uint64_t v) {
    char b[24];
    snprintf(b, sizeof(b), "%#llx", static_cast<unsigned long long>(v));
    return b;
}

std::string statusText(uint32_t status) {
    return hex(status) + " " + exceptionName(status);
}

// --- one cleared frame, measured ---------------------------------------------------------------------------------

struct ClearCase {
    const char* stage;
    uint32_t width, height, rowBytes;
    uint64_t offset;            // of the first pixel in the buffer
    Format format;
    float rgba[4];
    ClearOptions options;
    bool expectWritten = true;  // false: the frame should leave every byte as it was
};

constexpr uint8_t kCanary = 0xa5;

bool runClear(Device& device, const ClearCase& c) {
    std::string why;
    uint64_t bytes = c.offset + static_cast<uint64_t>(c.rowBytes) * c.height + 4096;   // and a page of canary after
    auto bo = device.gpu().newBo(bytes, &why);
    if (!bo) return verdict(c.stage, false, "target buffer: " + why);
    memset(bo->cpu(), kCanary, bo->size());

    Target t;
    t.bo = bo.get();
    t.offset = c.offset;
    t.width = c.width;
    t.height = c.height;
    t.rowBytes = c.rowBytes;
    t.format = c.format;
    auto pass = ClearPass::make(device, t, c.rgba, c.options, &why);
    if (!pass) return verdict(c.stage, false, "refused: " + why);

    // the pixel, from the colour alone
    uint8_t r = unorm8(c.rgba[0]), g = unorm8(c.rgba[1]), b = unorm8(c.rgba[2]), a = unorm8(c.rgba[3]);
    uint8_t want[4] = {r, g, b, a};
    if (c.format == Format::BGRA8Unorm) {
        want[0] = b, want[2] = r;
    }
    // the pixels the frame covers: the framebuffer's bounds inside the frame. Not the job's tiles: on the phone a job
    // over some of the frame's tiles wrote all of it (runs 501, 502), while the framebuffer's bounds held to the pixel
    uint32_t x0 = c.options.boundMinX, y0 = c.options.boundMinY;
    uint32_t x1 = c.options.boundMaxX == ~0u ? c.width - 1 : c.options.boundMaxX;
    uint32_t y1 = c.options.boundMaxY == ~0u ? c.height - 1 : c.options.boundMaxY;

    Transport& tr = device.gpu().transport();
    Submission s;
    uint64_t t0 = tr.nowUs();
    int error = pass->submit(&s);
    if (error != 0) return verdict(c.stage, false, std::string("SUBMIT: ") + strerror(error));
    error = device.gpu().wait(s, 2000000);
    uint64_t us = tr.nowUs() - t0;
    if (error != 0) return verdict(c.stage, false, "job " + std::to_string(s.seq) + " not done in 2 s: " + strerror(error));
    uint32_t status = pass->status();

    uint64_t covered = 0, right = 0, canaryKept = 0, canaryTotal = 0, firstWrong = ~0ull;
    const uint8_t* m = bo->cpu();
    for (uint64_t i = 0; i < bo->size(); i++) {
        bool inPixel = false;
        if (i >= c.offset && (i - c.offset) / c.rowBytes < c.height) {
            uint64_t y = (i - c.offset) / c.rowBytes, x = ((i - c.offset) % c.rowBytes) / 4;
            inPixel = x < c.width && y >= y0 && y <= y1 && x >= x0 && x <= x1 && c.expectWritten;
            if (inPixel && ((i - c.offset) % c.rowBytes) % 4 == 0) {
                covered++;
                if (memcmp(m + i, want, 4) == 0) {
                    right++;
                } else if (firstWrong == ~0ull) {
                    firstWrong = i;
                }
            }
        }
        if (!inPixel) {
            canaryTotal++;
            canaryKept += m[i] == kCanary;
            if (m[i] != kCanary && firstWrong == ~0ull) firstWrong = i;
        }
    }
    // what the GPU wrote, whatever was expected: the pixels of the target holding the colour and their bounding box
    uint64_t holding = 0;
    uint32_t bx0 = ~0u, by0 = ~0u, bx1 = 0, by1 = 0;
    for (uint32_t y = 0; y < c.height; y++) {
        for (uint32_t x = 0; x < c.width; x++) {
            if (memcmp(m + c.offset + static_cast<uint64_t>(y) * c.rowBytes + 4ull * x, want, 4) == 0) {
                holding++;
                bx0 = x < bx0 ? x : bx0, by0 = y < by0 ? y : by0, bx1 = x > bx1 ? x : bx1, by1 = y > by1 ? y : by1;
            }
        }
    }
    char region[80];
    if (holding == 0) snprintf(region, sizeof(region), "none written");
    else snprintf(region, sizeof(region), "written (%u,%u)-(%u,%u), %llu", bx0, by0, bx1, by1, (unsigned long long)holding);
    char detail[320];
    snprintf(detail, sizeof(detail), "%ux%u %s, job %s, %llu of %llu pixels %02x%02x%02x%02x, canary %llu of %llu "
             "bytes kept, %s, %llu us", c.width, c.height, c.format == Format::BGRA8Unorm ? "BGRA8" : "RGBA8",
             statusText(status).c_str(), static_cast<unsigned long long>(right), static_cast<unsigned long long>(covered),
             want[0], want[1], want[2], want[3], static_cast<unsigned long long>(canaryKept),
             static_cast<unsigned long long>(canaryTotal), region, static_cast<unsigned long long>(us));
    bool ok = (status & 0xff) == 1 && right == covered && canaryKept == canaryTotal;
    std::string text = detail;
    if (!ok && firstWrong != ~0ull) {
        uint64_t i = firstWrong & ~3ull;
        char w[96];
        snprintf(w, sizeof(w), "; first difference at byte %llu: %02x %02x %02x %02x",
                 static_cast<unsigned long long>(firstWrong), m[i], m[i + 1], m[i + 2], m[i + 3]);
        text += w;
    }
    verdict(c.stage, ok, text);
    if (!ok || gDump) {
        pass->dump(stdout);
    }
    return ok;
}

// One DrawPass of the color program (a varying from the vertices, a uniform tint pushed from buffer 0), judged pixel by
// pixel: more than a pixel inside the triangle the colour interpolated in screen space (w is 1) times the tint, through
// the blend when there is one; more than a pixel outside the clear; within 2/255 a channel (8-bit rounding, dithering)
bool runColorDraw(Device& device, const char* stage, const float colors[12], const float tint[4], const float clear[4],
                  bool over, uint32_t W = 64, uint32_t H = 64) {
    std::string why;
    const uint32_t R = (4 * W + 63) & ~63u;
    auto bo = device.gpu().newBo(static_cast<uint64_t>(R) * H + 4096, &why);
    if (!bo) return verdict(stage, false, why);
    memset(bo->cpu(), kCanary, bo->size());
    Target t;
    t.bo = bo.get();
    t.width = W;
    t.height = H;
    t.rowBytes = R;
    const float positions[12] = {-0.5f, -0.5f, 0.0f, 1.0f, 0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.5f, 0.0f, 1.0f};
    DrawInput in;
    in.attributes[0] = positions;
    in.attributes[1] = colors;
    in.uniforms = tint;
    in.uniformSize = 16;
    memcpy(in.clear, clear, sizeof(in.clear));
    in.blendOver = over;
    auto pass = DrawPass::make(device, t, hm_color, in, &why);
    if (!pass) return verdict(stage, false, "refused: " + why);
    Submission ds, fs;
    uint64_t t0 = device.gpu().transport().nowUs();
    int e = pass->submit(&ds, &fs);
    if (e == 0) e = device.gpu().wait(fs, 2000000);
    uint64_t us = device.gpu().transport().nowUs() - t0;
    // the viewport: NDC (x, y) at pixel (W/2 + W/2 x, H/2 + H/2 y)
    const float vx[3] = {W * 0.25f, W * 0.75f, W * 0.5f}, vy[3] = {H * 0.25f, H * 0.25f, H * 0.75f};
    float area = (vx[1] - vx[0]) * (vy[2] - vy[0]) - (vx[2] - vx[0]) * (vy[1] - vy[0]);
    unsigned inRight = 0, inTotal = 0, outRight = 0, outTotal = 0, worst = 0;
    uint8_t clearPx[4];
    for (int c = 0; c < 4; c++) clearPx[c] = unorm8(clear[c]);
    char firstBad[120] = "";
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            float px = x + 0.5f, py = y + 0.5f, b[3], d = 1e9f;
            for (int k = 0; k < 3; k++) {
                int i = (k + 1) % 3, j = (k + 2) % 3;
                b[k] = ((vx[j] - vx[i]) * (py - vy[i]) - (px - vx[i]) * (vy[j] - vy[i])) / area;
                float ex = vx[j] - vx[i], ey = vy[j] - vy[i];
                float dist = std::fabs(b[k] * area) / std::sqrt(ex * ex + ey * ey);
                d = dist < d ? dist : d;
            }
            bool inside = b[0] > 0 && b[1] > 0 && b[2] > 0;
            if (d <= 1.0f) continue;
            const uint8_t* q = bo->cpu() + y * R + 4 * x;
            uint8_t want[4];
            if (inside) {
                float src[4];
                for (int c = 0; c < 4; c++) src[c] = (b[0] * colors[c] + b[1] * colors[4 + c] + b[2] * colors[8 + c]) * tint[c];
                for (int c = 0; c < 4; c++) {
                    float v = over ? src[c] + clear[c] * (1.0f - src[3]) : src[c];
                    want[c] = unorm8(v);
                }
            } else {
                memcpy(want, clearPx, 4);
            }
            unsigned diff = 0;
            for (int c = 0; c < 4; c++) {
                unsigned dd = q[c] > want[c] ? q[c] - want[c] : want[c] - q[c];
                diff = dd > diff ? dd : diff;
            }
            bool good = diff <= 2;
            worst = inside && diff > worst ? diff : worst;
            (inside ? inTotal : outTotal)++;
            (inside ? inRight : outRight) += good;
            if (!good && firstBad[0] == 0)
                snprintf(firstBad, sizeof(firstBad), "; first wrong (%u,%u): %02x%02x%02x%02x, want %02x%02x%02x%02x", x, y,
                         q[0], q[1], q[2], q[3], want[0], want[1], want[2], want[3]);
        }
    }
    const uint8_t* mid = bo->cpu() + (H / 2) * R + 4 * (W / 2);
    char detail[360];
    snprintf(detail, sizeof(detail), "vertex %s tiler %s fragment %s%s%s; inside %u of %u right (largest difference %u), "
             "outside %u of %u the clear; pixel (32,32) %02x%02x%02x%02x; %llu us%s",
             statusText(pass->vertexStatus()).c_str(), statusText(pass->tilerStatus()).c_str(),
             statusText(pass->fragmentStatus()).c_str(), e ? ", " : "", e ? strerror(e) : "", inRight, inTotal, worst,
             outRight, outTotal, mid[0], mid[1], mid[2], mid[3], (unsigned long long)us, firstBad);
    bool jobs = e == 0 && (pass->vertexStatus() & 0xff) == 1 && (pass->tilerStatus() & 0xff) == 1 &&
                (pass->fragmentStatus() & 0xff) == 1;
    if (gFake) {
        say("%s MEASURE (the fake GPU checks the jobs, does not rasterize: %s) %s", stage,
            jobs ? "jobs accepted" : "jobs REFUSED", detail);
        if (!jobs) gFailed++;
        return jobs;
    }
    bool ok = jobs && inRight == inTotal && outRight == outTotal;
    verdict(stage, ok, detail);
    if (!ok || gDump) pass->dump(stdout);
    return ok;
}

// The tex program: a 4x4 RGBA8 texture of sixteen colours sampled with nearest filtering across a quad covering
// the 64x64 target (two triangles, uv 0..1): pixel (x, y) must be texel (x / 16, y / 16) exactly, judged away from
// the texel edges by a pixel
bool runTexture(Device& device) {
    const char* stage = "texture";
    std::string why;
    auto bo = device.gpu().newBo(256 * 64 + 4096, &why);
    auto tex = device.gpu().newBo(4096, &why);
    if (!bo || !tex) return verdict(stage, false, why);
    memset(bo->cpu(), kCanary, bo->size());
    uint8_t texel[16][4];
    for (int i = 0; i < 16; i++) {
        texel[i][0] = static_cast<uint8_t>(17 * i), texel[i][1] = static_cast<uint8_t>(255 - 16 * i);
        texel[i][2] = static_cast<uint8_t>((i * 97) & 255), texel[i][3] = 255;
    }
    for (int y = 0; y < 4; y++) memcpy(tex->cpu() + y * 64, texel[4 * y], 16);
    Target t;
    t.bo = bo.get();
    t.width = t.height = 64;
    t.rowBytes = 256;
    // two triangles over the whole viewport: NDC -1..1, uv 0..1 (row y of the target is uv.y = y / 64)
    const float pos[24] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1, -1, 1, 0, 1};
    const float uv[24] = {0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0};
    DrawInput in;
    in.attributes[0] = pos;
    in.attributes[1] = uv;
    in.vertexCount = 6;
    in.texture = tex.get();
    in.textureWidth = in.textureHeight = 4;
    in.textureRowBytes = 64;
    in.nearest = true;
    auto pass = DrawPass::make(device, t, hm_tex, in, &why);
    if (!pass) return verdict(stage, false, "refused: " + why);
    Submission ds, fs;
    int e = pass->submit(&ds, &fs);
    if (e == 0) e = device.gpu().wait(fs, 2000000);
    unsigned right = 0, judged = 0;
    char firstBad[120] = "";
    for (uint32_t y = 0; y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            if (x % 16 == 0 || x % 16 == 15 || y % 16 == 0 || y % 16 == 15) continue;
            judged++;
            const uint8_t* q = bo->cpu() + y * 256 + 4 * x;
            const uint8_t* w = texel[(y / 16) * 4 + x / 16];
            if (memcmp(q, w, 4) == 0) right++;
            else if (firstBad[0] == 0)
                snprintf(firstBad, sizeof(firstBad), "; first wrong (%u,%u): %02x%02x%02x%02x, want %02x%02x%02x%02x", x, y,
                         q[0], q[1], q[2], q[3], w[0], w[1], w[2], w[3]);
        }
    }
    char detail[300];
    snprintf(detail, sizeof(detail), "vertex %s tiler %s fragment %s; %u of %u pixels the texel their uv names%s",
             statusText(pass->vertexStatus()).c_str(), statusText(pass->tilerStatus()).c_str(),
             statusText(pass->fragmentStatus()).c_str(), right, judged, firstBad);
    bool jobs = e == 0 && (pass->vertexStatus() & 0xff) == 1 && (pass->tilerStatus() & 0xff) == 1 &&
                (pass->fragmentStatus() & 0xff) == 1;
    if (gFake) {
        say("%s MEASURE (the fake GPU checks the jobs, does not rasterize: %s) %s", stage, jobs ? "jobs accepted" : "jobs REFUSED", detail);
        if (!jobs) gFailed++;
        return jobs;
    }
    bool ok = jobs && right == judged;
    verdict(stage, ok, detail);
    if (!ok || gDump) pass->dump(stdout);
    return ok;
}

// The fill kernel: 4 workgroups of 16 write out[i] = i * 3 + k for i < 64 into a buffer of canaries; every word right
// and the canary after them kept
bool runCompute(Device& device) {
    const char* stage = "compute";
    std::string why;
    auto bo = device.gpu().newBo(4096, &why);
    if (!bo) return verdict(stage, false, why);
    memset(bo->cpu(), kCanary, bo->size());
    uint32_t uniforms[3] = {static_cast<uint32_t>(bo->gpu()), static_cast<uint32_t>(bo->gpu() >> 32), 1000};
    const unsigned groups[3] = {4, 1, 1};
    auto pass = ComputePass::make(device, hm_fill, groups, uniforms, sizeof(uniforms), &why);
    if (!pass) return verdict(stage, false, "refused: " + why);
    Submission s;
    uint64_t t0 = device.gpu().transport().nowUs();
    int e = pass->submit({bo.get()}, &s);
    if (e == 0) e = device.gpu().wait(s, 2000000);
    uint64_t us = device.gpu().transport().nowUs() - t0;
    const uint32_t* w = reinterpret_cast<const uint32_t*>(bo->cpu());
    unsigned right = 0, kept = 0;
    for (unsigned i = 0; i < 64; i++) right += w[i] == i * 3 + 1000;
    for (unsigned i = 64; i < bo->size() / 4; i++) kept += w[i] == 0xa5a5a5a5u;
    char detail[200];
    snprintf(detail, sizeof(detail), "job %s%s%s; %u of 64 words i*3+1000 (word 0 %u, word 63 %u), canary %u of %llu kept; %llu us",
             statusText(pass->status()).c_str(), e ? ", " : "", e ? strerror(e) : "", right, w[0], w[63], kept,
             (unsigned long long)(bo->size() / 4 - 64), (unsigned long long)us);
    bool job = e == 0 && (pass->status() & 0xff) == 1;
    if (gFake) {
        say("%s MEASURE (the fake GPU checks the job, runs no shader: %s) %s", stage, job ? "job accepted" : "job REFUSED", detail);
        if (!job) gFailed++;
        return job;
    }
    bool ok = job && right == 64 && kept == bo->size() / 4 - 64;
    verdict(stage, ok, detail);
    if (!ok || gDump) pass->dump(stdout);
    return ok;
}

// Depth: one draw of two triangles, the near one (z -0.5, green) first and the far one (z +0.5, red) after it,
// overlapping; with the depth test where both cover a pixel it must be green, without it red
bool runDepth(Device& device, bool test) {
    const char* stage = test ? "depth" : "depth-off";
    std::string why;
    auto bo = device.gpu().newBo(256 * 64 + 4096, &why);
    if (!bo) return verdict(stage, false, why);
    memset(bo->cpu(), kCanary, bo->size());
    Target t;
    t.bo = bo.get();
    t.width = t.height = 64;
    t.rowBytes = 256;
    // near: pixels (8,8) (56,8) (8,56), so x + y < 64; far: (56,56) (8,40) (40,8), so x + y > 48: both in 48..64
    const float pos[24] = {-0.75f, -0.75f, -0.5f, 1, 0.75f, -0.75f, -0.5f, 1, -0.75f, 0.75f, -0.5f, 1,
                           0.75f, 0.75f, 0.5f, 1, -0.75f, 0.25f, 0.5f, 1, 0.25f, -0.75f, 0.5f, 1};
    const float col[24] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
    const float one[4] = {1, 1, 1, 1};
    DrawInput in;
    in.attributes[0] = pos;
    in.attributes[1] = col;
    in.vertexCount = 6;
    in.uniforms = one;
    in.uniformSize = 16;
    in.depthTest = test;
    auto pass = DrawPass::make(device, t, hm_color, in, &why);
    if (!pass) return verdict(stage, false, "refused: " + why);
    Submission ds, fs;
    int e = pass->submit(&ds, &fs);
    if (e == 0) e = device.gpu().wait(fs, 2000000);
    // pixels well inside both triangles: x + y in pixel space between the two hypotenuses, away from the edges
    unsigned green = 0, red = 0, judged = 0;
    for (uint32_t y = 14; y < 50; y++) {
        for (uint32_t x = 14; x < 50; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            bool both = px > 10 && py > 10 && px < 54 && py < 54 && px + py > 50 && px + py < 62;
            if (!both) continue;
            judged++;
            const uint8_t* q = bo->cpu() + y * 256 + 4 * x;
            green += q[0] == 0 && q[1] == 0xff && q[2] == 0;
            red += q[0] == 0xff && q[1] == 0 && q[2] == 0;
        }
    }
    char detail[220];
    snprintf(detail, sizeof(detail), "vertex %s tiler %s fragment %s; where both cover: %u green (near), %u red (far) of %u",
             statusText(pass->vertexStatus()).c_str(), statusText(pass->tilerStatus()).c_str(),
             statusText(pass->fragmentStatus()).c_str(), green, red, judged);
    bool jobs = e == 0 && (pass->fragmentStatus() & 0xff) == 1 && (pass->tilerStatus() & 0xff) == 1;
    if (gFake) {
        say("%s MEASURE (the fake GPU does not rasterize: %s) %s", stage, jobs ? "jobs accepted" : "jobs REFUSED", detail);
        if (!jobs) gFailed++;
        return jobs;
    }
    bool ok = jobs && judged > 0 && (test ? green == judged : red == judged);
    verdict(stage, ok, detail);
    if (!ok || gDump) pass->dump(stdout);
    return ok;
}

// The compiler inside the driver: the programs compiled here, in this process, by Mesa's Bifrost compiler, held word for
// word against shaders_v7.h (compiled on the Mac by the same compiler.c), then drawn and dispatched with
bool runCompiler(Device& device) {
#if !defined(HAKIOS_MALI_COMPILER)
    (void)device;
    say("compiler MEASURE not built in (mali27.sh links Mesa's compiler with HAKIOS_MALI_COMPILER)");
    return true;
#else
    char why[200] = "";
    uint64_t t0 = device.gpu().transport().nowUs();
    hm_compile_init();
    struct hm_program color;
    if (hm_compile_program("color", static_cast<unsigned>(device.productId()), &color, why, sizeof(why)) != 0)
        return verdict("compiler", false, why);
    uint64_t compiled = device.gpu().transport().nowUs() - t0;
    auto same = [](const void* a, const void* b, size_t n) { return memcmp(a, b, n) == 0; };
    bool code = color.vs_size == hm_color.vs_size && color.fs_size == hm_color.fs_size &&
                same(color.vs_code, hm_color.vs_code, color.vs_size) && same(color.fs_code, hm_color.fs_code, color.fs_size);
    bool states = same(color.vs_rsd, hm_color.vs_rsd, 64) && same(color.fs_rsd, hm_color.fs_rsd, 64) &&
                  same(color.fs_rsd_depth, hm_color.fs_rsd_depth, 64) && same(color.blend_off, hm_color.blend_off, 16) &&
                  same(color.blend_over, hm_color.blend_over, 16);
    bool tables = color.fs_push_count == hm_color.fs_push_count && color.vs_out_count == hm_color.vs_out_count &&
                  color.fs_in_count == hm_color.fs_in_count &&
                  same(color.fs_push, hm_color.fs_push, 4 * color.fs_push_count) &&
                  same(color.vs_out, hm_color.vs_out, sizeof(hm_varying) * color.vs_out_count) &&
                  same(color.fs_in, hm_color.fs_in, sizeof(hm_varying) * color.fs_in_count);
    struct hm_kernel fill;
    bool kernel = hm_compile_kernel("fill", static_cast<unsigned>(device.productId()), &fill, why, sizeof(why)) == 0 &&
                  fill.size == hm_fill.size && same(fill.code, hm_fill.code, fill.size) && same(fill.rsd, hm_fill.rsd, 64);
    char detail[260];
    snprintf(detail, sizeof(detail), "color compiled on the phone in %llu us: vertex %u bytes, fragment %u bytes; code %s, "
             "states %s, tables %s the Mac's; fill %s", (unsigned long long)compiled, color.vs_size, color.fs_size,
             code ? "=" : "DIFFERENT from", states ? "=" : "DIFFERENT from", tables ? "=" : "DIFFERENT from",
             kernel ? "= the Mac's" : why[0] ? why : "DIFFERENT from the Mac's");
    bool ok = verdict("compiler", code && states && tables && kernel, detail);
    // and drawn with what was compiled here: the varyings stage again, with the phone's own program
    {
        std::string w;
        auto bo = device.gpu().newBo(256 * 64 + 4096, &w);
        Target t;
        t.bo = bo.get();
        t.width = t.height = 64;
        t.rowBytes = 256;
        const float positions[12] = {-0.5f, -0.5f, 0.0f, 1.0f, 0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.5f, 0.0f, 1.0f};
        const float rgb[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
        const float one[4] = {1, 1, 1, 1};
        DrawInput in;
        in.attributes[0] = positions;
        in.attributes[1] = rgb;
        in.uniforms = one;
        in.uniformSize = 16;
        auto pass = bo ? DrawPass::make(device, t, color, in, &w) : nullptr;
        Submission ds, fs;
        int e = pass ? pass->submit(&ds, &fs) : -1;
        if (e == 0) e = device.gpu().wait(fs, 2000000);
        const uint8_t* mid = bo ? bo->cpu() + 32 * 256 + 4 * 32 : nullptr;
        bool drawn = e == 0 && pass && (pass->fragmentStatus() & 0xff) == 1 && mid[3] == 0xff && (mid[0] | mid[1] | mid[2]);
        char d2[160];
        snprintf(d2, sizeof(d2), "the phone's color program drawn: fragment %s, pixel (32,32) %02x%02x%02x%02x",
                 pass ? statusText(pass->fragmentStatus()).c_str() : w.c_str(), mid ? mid[0] : 0, mid ? mid[1] : 0,
                 mid ? mid[2] : 0, mid ? mid[3] : 0);
        ok = verdict("compiler-draw", drawn, d2) && ok;
    }
    hm_program_free(&color);
    if (fill.code) hm_kernel_free(&fill);
    return ok;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);       // every line out as it is printed: a crash must not take them along
    if (argc == 3 && strcmp(argv[1], "--until") == 0) {
        struct timeval boot = {}, now = {};
        size_t n = sizeof(boot);
        int mib[2] = {CTL_KERN, KERN_BOOTTIME};
        if (sysctl(mib, 2, &boot, &n, nullptr, 0) != 0) return 1;
        gettimeofday(&now, nullptr);
        long up = now.tv_sec - boot.tv_sec, until = atol(argv[2]);
        printf("mali-probe: up %ld s, waiting until %ld s\n", up, until);
        if (until > up) sleep(static_cast<unsigned>(until - up));
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fake") == 0) gFake = true;
        else if (strcmp(argv[i], "--dump") == 0) gDump = true;
        else if (strcmp(argv[i], "--show") == 0 && i + 1 < argc) gShow = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: mali-probe [--fake] [--dump] [--show SECONDS] | mali-probe --until SECONDS\n");
            return 2;
        }
    }
    say("started (%s)", gFake ? "the fake GPU" : "HakiosMali");

    // 1. the service
    std::string why;
    std::unique_ptr<Transport> transport;
    if (gFake) {
        transport.reset(new FakeGpu());
    } else {
        transport = openKernelMali(60, &why);
    }
    if (!verdict("open", transport != nullptr, transport ? "" : why)) {
        say("done: %u passed, %u failed; milestone clear64 FAIL", gPassed, gFailed);
        return 1;
    }
    FakeGpu* fake = gFake ? static_cast<FakeGpu*>(transport.get()) : nullptr;
    Gpu gpu(std::move(transport));

    // 2. what the GPU is
    {
        uint64_t v[5] = {};
        uint32_t which[5] = {kParamGpuProdId, kParamGpuRevision, kParamShaderPresent, kParamTilerFeatures,
                             kParamThreadTlsAlloc};
        std::string detail;
        bool ok = true;
        for (int i = 0; i < 5; i++) {
            int e = gpu.param(which[i], &v[i]);
            ok = ok && e == 0;
        }
        const Model* model = modelOf(v[0]);
        char b[200];
        snprintf(b, sizeof(b), "product %#llx (%s) revision %#llx shaders %#llx tiler features %#llx (levels %llu) "
                 "TLS %#llx", (unsigned long long)v[0], model ? model->name : "not a v7 GPU", (unsigned long long)v[1],
                 (unsigned long long)v[2], (unsigned long long)v[3], (unsigned long long)((v[3] >> 8) & 0xf),
                 (unsigned long long)v[4]);
        verdict("params", ok && model != nullptr, b);
    }
    Device device(gpu);
    if (!verdict("device", device.init(&why) == 0, why)) {
        say("done: %u passed, %u failed; milestone clear64 FAIL", gPassed, gFailed);
        return 1;
    }

    // 3. a buffer: made, at a GPU address, mapped, written and read back
    if (gShow == 0) {
        auto bo = gpu.newBo(65536, &why);
        bool ok = bo != nullptr;
        uint64_t wrong = 0;
        if (ok) {
            uint32_t* w = reinterpret_cast<uint32_t*>(bo->cpu());
            for (uint32_t i = 0; i < bo->size() / 4; i++) w[i] = i * 2654435761u;
            for (uint32_t i = 0; i < bo->size() / 4; i++) wrong += w[i] != i * 2654435761u;
            ok = wrong == 0 && bo->gpu() != 0 && bo->gpu() < (1ull << 32);
            why = "handle " + std::to_string(bo->handle()) + " at GPU " + hex(bo->gpu()) + ", " +
                  std::to_string(bo->size()) + " bytes, " + std::to_string(wrong) + " words read back wrong";
        }
        verdict("buffer", ok, why);
    }

    // 4. the smallest job: Write Value
    if (gShow == 0) {
        auto target = gpu.newBo(4096, &why);
        std::unique_ptr<WriteValue> job;
        if (target) {
            memset(target->cpu(), kCanary, 64);
            job = WriteValue::make(device, *target, 16, 0x6d616c69u, &why);
        }
        if (!job) {
            verdict("write-value", false, why);
        } else {
            Submission s;
            int e = job->submit(&s);
            if (e == 0) e = gpu.wait(s, 2000000);
            const uint32_t* w = reinterpret_cast<const uint32_t*>(target->cpu());
            bool neighbours = w[3] == 0xa5a5a5a5u && w[5] == 0xa5a5a5a5u;
            char b[160];
            snprintf(b, sizeof(b), "job %s, word %#x (want 0x6d616c69), neighbours %s%s%s", statusText(job->status()).c_str(),
                     w[4], neighbours ? "kept" : "CHANGED", e ? ", " : "", e ? strerror(e) : "");
            bool ok = e == 0 && (job->status() & 0xff) == 1 && w[4] == 0x6d616c69u && neighbours;
            verdict("write-value", ok, b);
            if (!ok || gDump) job->dump(stdout);
        }
    }

    // 5. THE MILESTONE: one Fragment job clears a 64x64 RGBA8 target to a known colour, read back right
    ClearCase clear64 = {"clear64", 64, 64, 256, 0, Format::RGBA8Unorm, {0.2f, 0.4f, 0.6f, 0.8f}, {}, true};
    bool milestone = runClear(device, clear64);
    if (gShow == 0) {

    // 6.. what narrows it down or goes further, each on its own
    ClearCase tiler = {"clear64-tiler", 64, 64, 256, 0, Format::RGBA8Unorm, {1.0f, 0.0f, 0.5f, 1.0f}, {}, true};
    tiler.options.withTiler = true;
    runClear(device, tiler);

    ClearCase odd = {"clear-48x20", 48, 20, 256, 256, Format::RGBA8Unorm, {0.0f, 1.0f, 0.0f, 1.0f}, {}, true};
    runClear(device, odd);

    ClearCase bgra = {"clear64-bgra", 64, 64, 256, 0, Format::BGRA8Unorm, {0.2f, 0.4f, 0.6f, 0.8f}, {}, true};
    runClear(device, bgra);

    // a job over tiles (1,1)-(2,2) alone: the whole frame is written all the same (runs 501, 502)
    ClearCase tiles = {"clear64-tiles", 64, 64, 256, 0, Format::RGBA8Unorm, {0.0f, 0.0f, 1.0f, 1.0f}, {}, true};
    tiles.options.minTx = tiles.options.minTy = 1;
    tiles.options.maxTx = tiles.options.maxTy = 2;
    runClear(device, tiles);

    // run 501: the job's tiles alone did not keep the GPU inside (1,1)-(2,2) -- it wrote the whole 64x64. Which bounds
    // does it keep? The framebuffer's alone, then both
    ClearCase fbBounds = {"clear64-fbbounds", 64, 64, 256, 0, Format::RGBA8Unorm, {1.0f, 0.0f, 1.0f, 1.0f}, {}, true};
    fbBounds.options.boundMinX = fbBounds.options.boundMinY = 16;
    fbBounds.options.boundMaxX = fbBounds.options.boundMaxY = 47;
    runClear(device, fbBounds);
    ClearCase both = {"clear64-both", 64, 64, 256, 0, Format::RGBA8Unorm, {0.0f, 1.0f, 1.0f, 1.0f}, {}, true};
    both.options.minTx = both.options.minTy = 1;
    both.options.maxTx = both.options.maxTy = 2;
    both.options.boundMinX = both.options.boundMinY = 16;
    both.options.boundMaxX = both.options.boundMaxY = 47;
    runClear(device, both);
    // and a frame bounded off the tile grid: pixels (5,7)-(40,50)
    ClearCase odd2 = {"clear64-fbbounds-odd", 64, 64, 256, 0, Format::RGBA8Unorm, {1.0f, 1.0f, 0.0f, 1.0f}, {}, true};
    odd2.options.boundMinX = 5, odd2.options.boundMinY = 7, odd2.options.boundMaxX = 40, odd2.options.boundMaxY = 50;
    runClear(device, odd2);

    ClearCase big = {"clear-750x1624", 750, 1624, 3008, 0, Format::BGRA8Unorm, {0.1f, 0.2f, 0.3f, 1.0f}, {}, true};
    runClear(device, big);

    // the fake's model of a frame without primitives whose target does not write clean tiles: nothing is written
    ClearCase noclean = {"clear64-noclean", 64, 64, 256, 0, Format::RGBA8Unorm, {1.0f, 1.0f, 1.0f, 1.0f}, {}, false};
    noclean.options.cleanPixelWrite = false;
    runClear(device, noclean);

    }   // (not with --show)

    // the first triangle: shaders compiled by Mesa's Bifrost compiler (shaders_v7.h), a vertex job, a tiler job and the
    // Fragment job, all written here. NDC (-0.5,-0.5) (0.5,-0.5) (0,0.5) land on pixels (16,16) (48,16) (32,48);
    // a pixel whose centre is more than a pixel inside must be red, more than a pixel outside the clear colour
    // (black), and the edges are not judged. The fake GPU checks the jobs and does not rasterize.
    {
        auto bo = gpu.newBo(256 * 64 + 4096, &why);
        Target t;
        t.bo = bo.get();
        t.width = t.height = 64;
        t.rowBytes = 256;
        const float positions[12] = {-0.5f, -0.5f, 0.0f, 1.0f, 0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.5f, 0.0f, 1.0f};
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        std::unique_ptr<DrawPass> tri;
        if (bo) {
            memset(bo->cpu(), kCanary, bo->size());
            DrawInput din;
            din.attributes[0] = positions;
            memcpy(din.clear, black, sizeof(din.clear));
            tri = DrawPass::make(device, t, hm_tri, din, &why);
        }
        if (!tri) {
            verdict("triangle", false, "refused: " + why);
        } else {
            Submission ds, fs;
            uint64_t t0 = gpu.transport().nowUs();
            int e = tri->submit(&ds, &fs);
            if (e == 0) e = gpu.wait(fs, 2000000);
            uint64_t us = gpu.transport().nowUs() - t0;
            const float vx[3] = {16, 48, 32}, vy[3] = {16, 16, 48};
            unsigned inRight = 0, inTotal = 0, outRight = 0, outTotal = 0, red = 0;
            uint32_t bx0 = ~0u, by0 = ~0u, bx1 = 0, by1 = 0, row20 = 0, row44 = 0;
            const uint8_t redPx[4] = {0xff, 0, 0, 0xff}, blackPx[4] = {0, 0, 0, 0xff};
            for (uint32_t y = 0; y < 64; y++) {
                for (uint32_t x = 0; x < 64; x++) {
                    float px = x + 0.5f, py = y + 0.5f, d = 1e9f;
                    bool inside = true;
                    for (int k = 0; k < 3; k++) {
                        float ax = vx[k], ay = vy[k], bx = vx[(k + 1) % 3], by = vy[(k + 1) % 3];
                        float ex = bx - ax, ey = by - ay, len = std::sqrt(ex * ex + ey * ey);
                        // the signed distance to the edge, positive inside (the vertices are counter-clockwise
                        // with y down... either winding: compared against the third vertex's side)
                        float cx = vx[(k + 2) % 3], cy = vy[(k + 2) % 3];
                        float side = ((cx - ax) * ey - (cy - ay) * ex) > 0 ? 1.0f : -1.0f;
                        float dist = side * ((px - ax) * ey - (py - ay) * ex) / len;
                        inside = inside && dist > 0;
                        d = std::fabs(dist) < d ? std::fabs(dist) : d;
                    }
                    const uint8_t* p = bo->cpu() + y * 256 + 4 * x;
                    bool isRed = memcmp(p, redPx, 4) == 0;
                    if (isRed) {
                        red++;
                        bx0 = x < bx0 ? x : bx0, by0 = y < by0 ? y : by0, bx1 = x > bx1 ? x : bx1, by1 = y > by1 ? y : by1;
                        row20 += y == 20, row44 += y == 44;
                    }
                    if (d <= 1.0f) continue;
                    if (inside) {
                        inTotal++;
                        inRight += isRed;
                    } else {
                        outTotal++;
                        outRight += memcmp(p, blackPx, 4) == 0;
                    }
                }
            }
            unsigned canaryKept = 0;
            for (uint64_t i = 256 * 64; i < bo->size(); i++) canaryKept += bo->cpu()[i] == kCanary;
            const float* w = tri->positionsWritten();
            char b[400];
            snprintf(b, sizeof(b), "vertex %s tiler %s fragment %s%s%s; inside %u of %u red, outside %u of %u black; "
                     "red %u pixels in (%u,%u)-(%u,%u), rows 20/44 %u/%u wide; vertex 0 written at (%g, %g, %g, %g); "
                     "canary %u of %llu kept; %llu us", statusText(tri->vertexStatus()).c_str(),
                     statusText(tri->tilerStatus()).c_str(), statusText(tri->fragmentStatus()).c_str(), e ? ", " : "",
                     e ? strerror(e) : "", inRight, inTotal, outRight, outTotal, red, red ? bx0 : 0, red ? by0 : 0, bx1,
                     by1, row20, row44, w[0], w[1], w[2], w[3], canaryKept,
                     (unsigned long long)(bo->size() - 256 * 64), (unsigned long long)us);
            // the target itself, for the host (tools/probe-picture.py LOG OUT.png triangle): RGB, base64, 96 bytes a line
            {
                static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                std::vector<uint8_t> rgb;
                for (uint32_t i = 0; i < 64 * 64; i++) {
                    rgb.insert(rgb.end(), bo->cpu() + 4 * i, bo->cpu() + 4 * i + 3);
                }
                const unsigned per = 72, lines = (rgb.size() + per - 1) / per;
                for (unsigned l = 0; l < lines; l++) {
                    std::string text;
                    for (size_t i = l * per; i < rgb.size() && i < (l + 1) * per; i += 3) {
                        uint32_t v = rgb[i] << 16 | rgb[i + 1] << 8 | rgb[i + 2];
                        for (int k = 18; k >= 0; k -= 6) text += b64[(v >> k) & 63];
                    }
                    printf("mali-picture triangle 64x64 %u/%u: %s\n", l + 1, lines, text.c_str());
                }
            }
            // --show: the target sixteen times as big on the panel (B G R A there), on dark grey, with the clear64
            // colour as a band under it, for the window server's frames to be drawn over and put back
            if (gShow > 0 && !gFake) {
                const uint32_t W = 1080, H = 1200, S = 16, X0 = (W - 64 * S) / 2, Y0 = 40;
                std::vector<uint8_t> pic(static_cast<size_t>(W) * H * 4, 0x20);
                for (uint32_t y = 0; y < H; y++) {
                    for (uint32_t x = 0; x < W; x++) {
                        uint8_t* q = &pic[(static_cast<size_t>(y) * W + x) * 4];
                        q[3] = 0xff;
                        if (x >= X0 && x < X0 + 64 * S && y >= Y0 && y < Y0 + 64 * S) {
                            const uint8_t* p = bo->cpu() + ((y - Y0) / S) * 256 + 4 * ((x - X0) / S);
                            q[0] = p[2], q[1] = p[1], q[2] = p[0];
                        } else if (y >= Y0 + 64 * S + 40 && y < Y0 + 64 * S + 120 && x >= X0 && x < X0 + 64 * S) {
                            q[0] = 0x99, q[1] = 0x66, q[2] = 0x33;      // clear64's colour, 33 66 99, as B G R
                        }
                    }
                }
                int shown = 0, first = gpu.show(pic.data(), W, H, W * 4);
                say("show: the triangle sixteen times as big on the panel for %d s (%s)", gShow,
                    first == 0 ? "shown" : strerror(first));
                for (int i = 1; first == 0 && i < gShow * 5; i++) {
                    usleep(200000);
                    shown += gpu.show(pic.data(), W, H, W * 4) == 0;
                }
                (void)shown;
            }
            bool jobs = e == 0 && (tri->vertexStatus() & 0xff) == 1 && (tri->tilerStatus() & 0xff) == 1 &&
                        (tri->fragmentStatus() & 0xff) == 1;
            if (fake != nullptr) {
                say("triangle MEASURE (the fake GPU checks the jobs, does not rasterize: %s) %s",
                    jobs ? "jobs accepted" : "jobs REFUSED", b);
                if (!jobs) gFailed++;
            } else {
                bool ok = jobs && inRight == inTotal && outRight == outTotal && canaryKept == bo->size() - 256 * 64;
                verdict("triangle", ok, b);
                if (!ok || gDump) tri->dump(stdout);
            }
        }
    }

    // the color program (draw.hpp, shaders_v7.h): a varying, a pushed uniform, blending
    if (gShow == 0) {
        const float rgb[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
        const float white[12] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
        const float one[4] = {1, 1, 1, 1}, tint[4] = {0.5f, 1.0f, 0.25f, 1.0f}, half[4] = {0.5f, 0.5f, 0.5f, 0.5f};
        const float black[4] = {0, 0, 0, 1}, blue[4] = {0, 0, 1, 1};
        runColorDraw(device, "varyings", rgb, one, black, false);
        runColorDraw(device, "uniform", rgb, tint, black, false);
        runColorDraw(device, "blend", white, half, blue, true);
        runColorDraw(device, "varyings-160x96", rgb, one, black, false, 160, 96);
        runColorDraw(device, "varyings-750x1624", rgb, one, black, false, 750, 1624);
        runTexture(device);
        runCompute(device);
        runDepth(device, false);
        runDepth(device, true);
        runCompiler(device);
    }

    // the time of a submission and its wait, twenty clears of 64x64
    {
        auto bo = gpu.newBo(65536, &why);
        Target t;
        t.bo = bo.get();
        t.width = t.height = 64;
        t.rowBytes = 256;
        float c[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        uint64_t total = 0, worst = 0;
        unsigned done = 0;
        for (int i = 0; bo && i < 20; i++) {
            auto pass = ClearPass::make(device, t, c, ClearOptions(), &why);
            Submission s;
            uint64_t t0 = gpu.transport().nowUs();
            if (!pass || pass->submit(&s) != 0 || gpu.wait(s, 2000000) != 0 || (pass->status() & 0xff) != 1) break;
            uint64_t us = gpu.transport().nowUs() - t0;
            total += us;
            worst = us > worst ? us : worst;
            done++;
        }
        say("timing MEASURE %u of 20 clears done, %llu us each on average, %llu us the longest", done,
            (unsigned long long)(done ? total / done : 0), (unsigned long long)worst);
    }

    if (fake != nullptr) {
        say("fake: %llu jobs run, %zu faults, %llu pixels written", (unsigned long long)fake->jobsRun(),
            fake->faults().size(), (unsigned long long)fake->pixelsWritten());
        for (const auto& f : fake->faults()) say("fake fault: %s", f.why.c_str());
    }
    say("done: %u passed, %u failed; milestone clear64 %s", gPassed, gFailed, milestone ? "PASS" : "FAIL");
    return milestone ? 0 : 1;
}
