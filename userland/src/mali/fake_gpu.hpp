// A GPU for the tests: HakiosMali's requests answered in this process, and the job chains run by an interpreter that is
// strict where the Mali is. Every job is read through Mesa's generated unpackers (v7.h), so a bit outside every field of
// v7.xml stops the chain with the unpacker's own sentence; a job outside every buffer, not 64-byte aligned, of a type on
// the wrong slot, or naming memory no buffer holds stops it as the GPU would fault, and the job's Exception Status gets
// the code the GPU would write (JOB_CONFIG_FAULT, TILE_RANGE_FAULT, TRANSLATION_FAULT...). Then, as the kernel does
// after a failed chain (mtk_mali_submit resets the GPU), the chain counts as done and the next one runs.
//
// What it carries out: a Write Value job (the value at the address), and a Fragment job of a frame without primitives,
// which is all a cleared frame is: every 16x16 tile in the job's bounds, inside the frame's bounds and size, is written
// to each render target from its clear colour -- if, and only if, the render target writes clean tiles (Clean Pixel
// Write Enable: a tile nothing was drawn into is "clean", and the GPU skips writing it back otherwise, which is why
// pan_prepare_rt sets the bit for a cleared target). The colour goes through the tile buffer's internal format
// (R8G8B8A8) and the render target's swizzle into its writeback format (R8G8B8A8), linear rows.
//
// What it checks of a fragment job, each a refusal by its sentence: the framebuffer pointer's tag against the descriptor
// (Type, render target count, ZS CRC extension); the Parameters and Padding and every render target through the
// unpackers; the frame shaders Never (none is modelled); sample locations inside a buffer; one sample; the effective
// tile size a power of two in 16..256 (panfrost_max_effective_tile_size); the colour buffer allocation holding every
// target's tile (4 bytes a pixel, R8G8B8A8) and within the G52's 16 KB tile buffer; a Tiler Context and its heap, when
// named, inside buffers, with the frame's size; the job's tiles inside the frame (TILE_RANGE_FAULT, as Panfrost's own
// clamp in emit_fragment_job avoids); each written target's base and row stride 64-byte aligned (pan_layout.c:
// format_minimum_alignment, v7) and every row it writes inside one buffer.
//
// What it does NOT do: run a shader, rasterize, take a tiler job, or model the caches. A chain it runs is well formed
// and names only memory the GPU was given; whether the GPU agrees is the phone's to say (mali-probe).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mali.hpp"

namespace hakios::mali {

class FakeGpu : public Transport {
public:
    struct Fault {
        uint64_t seq = 0, job = 0;
        uint32_t status = 0;      // what the job's Exception Status got (0 if the header could not be written)
        std::string why;
    };

    FakeGpu();
    ~FakeGpu() override;

    int call(uint32_t request, void* data, size_t size) override;
    void* map(uint32_t handle, uint64_t* size) override;
    void unmap(uint32_t handle, void* address, uint64_t size) override;
    uint64_t nowUs() override { return ++clockUs_; }

    // what happened, for the tests
    const std::vector<Fault>& faults() const { return faults_; }
    uint64_t jobsRun() const { return jobsRun_; }
    uint64_t pixelsWritten() const { return pixelsWritten_; }   // by fragment jobs, every target counted
    size_t liveBuffers() const { return buffers_.size(); }
    size_t mappedBuffers() const;
    // LENGTH bytes at GPU address GPU, inside one buffer, or nullptr: for a test that changes a descriptor in place
    uint8_t* memoryAt(uint64_t gpu, uint64_t length) { return at(gpu, length); }

    // the parameters it answers (GET_PARAM), changeable by a test
    std::map<uint32_t, uint64_t> params;

private:
    struct Buffer {
        std::unique_ptr<uint8_t[]> raw;
        uint8_t* mem = nullptr;
        uint64_t gpu = 0, size = 0;
        bool mapped = false;
    };
    uint8_t* at(uint64_t gpu, uint64_t length);                 // inside one buffer, or nullptr
    void runChain(uint64_t jc, bool fragment, uint64_t seq);
    // 0 when the job is done, or the Exception Status code with *why
    uint32_t runWriteValue(uint8_t* job, std::string* why);
    uint32_t runFragment(uint8_t* job, std::string* why);

    std::map<uint32_t, Buffer> buffers_;
    uint32_t nextHandle_ = 1;
    uint64_t nextGpu_ = 0x01000000;     // HakiosMali's first address (mtk_mali.c GPU_ADDRESS_FIRST), 64 KB granules
    uint64_t seq_ = 0;
    uint64_t clockUs_ = 0;
    uint64_t jobsRun_ = 0, pixelsWritten_ = 0;
    std::vector<Fault> faults_;
};

}  // namespace hakios::mali
