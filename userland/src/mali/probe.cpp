// mali-probe: hakios-mali on the phone's Mali-G52, stage by stage, each with its verdict and, when it fails, why.
//
//   mali-probe            every stage against the kernel's HakiosMali (waits up to 60 s for the service)
//   mali-probe --fake     the same stages against the fake GPU, in this process (any machine)
//   mali-probe --dump     also print every frame's descriptors as Mesa's printers say them
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
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include "fake_gpu.hpp"
#include "mali.hpp"
#include "pass.hpp"

using namespace hakios::mali;

namespace {

bool gDump = false, gFake = false;
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
    // the pixels the frame covers: its tiles, inside the frame
    uint32_t lastTx = (c.width - 1) >> 4, lastTy = (c.height - 1) >> 4;
    uint32_t x0 = c.options.minTx * 16, y0 = c.options.minTy * 16;
    uint32_t x1 = (c.options.maxTx == ~0u ? lastTx : c.options.maxTx) * 16 + 15;
    uint32_t y1 = (c.options.maxTy == ~0u ? lastTy : c.options.maxTy) * 16 + 15;
    if (x1 >= c.width) x1 = c.width - 1;
    if (y1 >= c.height) y1 = c.height - 1;

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
    char detail[320];
    snprintf(detail, sizeof(detail), "%ux%u %s, job %s, %llu of %llu pixels %02x%02x%02x%02x, canary %llu of %llu "
             "bytes kept, %llu us", c.width, c.height, c.format == Format::BGRA8Unorm ? "BGRA8" : "RGBA8",
             statusText(status).c_str(), static_cast<unsigned long long>(right), static_cast<unsigned long long>(covered),
             want[0], want[1], want[2], want[3], static_cast<unsigned long long>(canaryKept),
             static_cast<unsigned long long>(canaryTotal), static_cast<unsigned long long>(us));
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

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);       // every line out as it is printed: a crash must not take them along
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fake") == 0) gFake = true;
        else if (strcmp(argv[i], "--dump") == 0) gDump = true;
        else {
            fprintf(stderr, "usage: mali-probe [--fake] [--dump]\n");
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
    {
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
    {
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

    // 6.. what narrows it down or goes further, each on its own
    ClearCase tiler = {"clear64-tiler", 64, 64, 256, 0, Format::RGBA8Unorm, {1.0f, 0.0f, 0.5f, 1.0f}, {}, true};
    tiler.options.withTiler = true;
    runClear(device, tiler);

    ClearCase odd = {"clear-48x20", 48, 20, 256, 256, Format::RGBA8Unorm, {0.0f, 1.0f, 0.0f, 1.0f}, {}, true};
    runClear(device, odd);

    ClearCase bgra = {"clear64-bgra", 64, 64, 256, 0, Format::BGRA8Unorm, {0.2f, 0.4f, 0.6f, 0.8f}, {}, true};
    runClear(device, bgra);

    ClearCase tiles = {"clear64-tiles", 64, 64, 256, 0, Format::RGBA8Unorm, {0.0f, 0.0f, 1.0f, 1.0f}, {}, true};
    tiles.options.minTx = tiles.options.minTy = 1;
    tiles.options.maxTx = tiles.options.maxTy = 2;
    runClear(device, tiles);

    ClearCase big = {"clear-750x1624", 750, 1624, 3008, 0, Format::BGRA8Unorm, {0.1f, 0.2f, 0.3f, 1.0f}, {}, true};
    runClear(device, big);

    // the fake's model of a frame without primitives whose target does not write clean tiles: nothing is written
    ClearCase noclean = {"clear64-noclean", 64, 64, 256, 0, Format::RGBA8Unorm, {1.0f, 1.0f, 1.0f, 1.0f}, {}, false};
    noclean.options.cleanPixelWrite = false;
    runClear(device, noclean);

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
