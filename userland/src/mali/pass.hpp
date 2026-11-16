// What the library builds on the GPU: the device's shared buffers, a Write Value job, and a render pass of one colour
// target whose load action is Clear and into which nothing is drawn -- one Fragment job over a Framebuffer Descriptor
// and its Render Target, the frame Panfrost submits for a clear-only batch (pan_cmdstream.c submit_batch: no
// vertex or tiler chain, emit_fbd, emit_fragment_job; on v7 no Tiler Context either: jm_emit_tiler_desc runs only for a
// draw). Every refusal is a sentence that names what is wrong, and nothing is submitted after one.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "mali.hpp"

namespace hakios::mali {

enum class Format { RGBA8Unorm, BGRA8Unorm };

// the device: the GPU and what every frame on it shares
class Device {
public:
    explicit Device(Gpu& gpu) : gpu_(gpu) {}
    // the GPU's parameters read and checked (a v7 GPU of pan_props.c's table: the phone's is 0x7402, G52 r1), the
    // sample positions uploaded. 0, or -1 with *why
    int init(std::string* why);

    Gpu& gpu() { return gpu_; }
    uint64_t productId() const { return productId_; }
    const Model* model() const { return model_; }
    // half the tile buffer, as Panfrost budgets it (panfrost_query_optimal_tib_size)
    uint32_t tileBufferBudget() const { return model_ ? model_->tileBuffer / 2 : kG52TileBufferBudget; }
    unsigned tilerLevels() const { return tilerLevels_; }
    const Bo& samplePositions() const { return *samplePositions_; }
    // the tiler heap, made the first time it is asked for (a frame with a Tiler Context names it); nullptr with *why
    const Bo* tilerHeap(std::string* why);

private:
    Gpu& gpu_;
    uint64_t productId_ = 0;
    const Model* model_ = nullptr;
    unsigned tilerLevels_ = 0;
    std::unique_ptr<Bo> samplePositions_, tilerHeap_;
};

// a colour target: WIDTH x HEIGHT pixels of FORMAT in BO from byte OFFSET on, ROW_BYTES apart
struct Target {
    const Bo* bo = nullptr;
    uint64_t offset = 0;
    uint32_t width = 0, height = 0, rowBytes = 0;
    Format format = Format::RGBA8Unorm;
};

struct ClearOptions {
    // a Tiler Context and its heap in the frame, as a frame with draws has them (jm_emit_tiler_desc), instead of none
    bool withTiler = false;
    // Clean Pixel Write Enable on the target, as pan_prepare_rt sets it for a clear; off only to see that the GPU then
    // writes no tile of a frame without primitives
    bool cleanPixelWrite = true;
    // the tiles the Fragment job runs, 16-pixel units, inclusive; ~0u: to the frame's last
    uint32_t minTx = 0, minTy = 0, maxTx = ~0u, maxTy = ~0u;
    // the Framebuffer Parameters' Bound Min/Max, pixels, inclusive; ~0u: to the frame's last (pan_emit_fbd always
    // writes the whole frame here and limits the work with the job's tiles alone)
    uint32_t boundMinX = 0, boundMinY = 0, boundMaxX = ~0u, boundMaxY = ~0u;
};

// One cleared frame, built and ready to submit. Its descriptors live in a command buffer of its own.
class ClearPass {
public:
    // nullptr with *why when the target, the colour or the options cannot make a frame (nothing is written then)
    static std::unique_ptr<ClearPass> make(Device& device, const Target& target, const float rgba[4],
                                           const ClearOptions& options, std::string* why);

    // to the fragment slot
    int submit(Submission* out);
    // the Fragment job's Exception Status, as the GPU left it (0: not run, 1: DONE)
    uint32_t status() const;
    uint64_t jobAddress() const { return cmd_->gpu() + kJob; }
    // the bytes pixel (x, y) must hold after the frame
    const uint8_t* expectedPixel() const { return expected_; }
    // the job, the framebuffer and its target as Mesa's printers say them
    void dump(FILE* fp) const;

private:
    ClearPass(Device& device) : device_(device) {}
    static constexpr uint32_t kJob = 0x000, kFbd = 0x040, kHeapDesc = 0x100, kTilerCtx = 0x140;
    Device& device_;
    Target target_;
    std::unique_ptr<Bo> cmd_;
    const Bo* heap_ = nullptr;
    uint8_t expected_[4] = {0, 0, 0, 0};
};

// The first triangle: one draw of three vertices into a 64x64 RGBA8 target cleared first, with the shaders of
// shaders_v7.h (Mesa's Bifrost compiler: gl_Position from attribute 0, colour 0 constant red; their Renderer State as
// Panfrost prepares it). As Panfrost submits a batch with a draw on v7 without IDVS: a chain of a vertex job and a
// tiler job that depends on it (slot 1), then the Fragment job (slot 0) over a Framebuffer Descriptor that names the
// Tiler Context the tiler job filled. POSITIONS are four floats a vertex in normalized device coordinates; the
// shader's viewport puts (x, y) on pixel (32 + 32x, 32 + 32y).
class TrianglePass {
public:
    static std::unique_ptr<TrianglePass> make(Device& device, const Target& target, const float positions[12],
                                              const float clear[4], std::string* why);
    // the two chains, each with its number: the draw's (vertex and tiler), then the frame's (fragment)
    int submit(Submission* draw, Submission* frame);
    // the jobs' Exception Status, as the GPU left them
    uint32_t vertexStatus() const;
    uint32_t tilerStatus() const;
    uint32_t fragmentStatus() const;
    uint64_t commandAddress() const { return cmd_->gpu(); }
    // what the vertex job wrote into the position varying buffer: four floats a vertex, in pixels after the viewport
    const float* positionsWritten() const { return reinterpret_cast<const float*>(cmd_->cpu() + kPositions); }
    void dump(FILE* fp) const;

    // where each thing is in the command buffer
    static constexpr uint32_t kVertexJob = 0x000, kTilerJob = 0x100, kFragmentJob = 0x200, kFbd = 0x240,
        kHeapDesc = 0x300, kTilerCtx = 0x340, kVsRsd = 0x400, kFsRsd = 0x440, kAttribute = 0x4c0,
        kAttributeBuffers = 0x500, kVaryings = 0x540, kVaryingBuffers = 0x580, kViewport = 0x5c0,
        kLocalStorage = 0x600, kVertices = 0x640, kPositions = 0x680, kVsCode = 0x800, kFsCode = 0x900;

private:
    TrianglePass(Device& device) : device_(device) {}
    Device& device_;
    Target target_;
    std::unique_ptr<Bo> cmd_;
    const Bo* heap_ = nullptr;
};

// One Write Value job: the GPU writes VALUE (32 bits) at byte OFFSET of TARGET. Its job in a buffer of its own; done
// when submit's chain is (wait), status() then 1.
class WriteValue {
public:
    static std::unique_ptr<WriteValue> make(Device& device, const Bo& target, uint64_t offset, uint32_t value,
                                            std::string* why);
    int submit(Submission* out);
    uint32_t status() const;
    void dump(FILE* fp) const;

private:
    WriteValue(Device& device) : device_(device) {}
    Device& device_;
    const Bo* target_ = nullptr;
    std::unique_ptr<Bo> cmd_;
};

}  // namespace hakios::mali
