// A draw of any program of shaders_v7.h (program.h) into an RGBA8Unorm target of any size, as Panfrost submits a batch with
// one draw on v7 without IDVS: the vertex job and the tiler job that depends on it (slot 1), then the Fragment job
// (slot 0) over the frame whose Tiler Context they filled. What the draw needs at draw time is written here through
// Mesa's packers (v7.h):
//   - the vertex attributes, one buffer each, of the program's formats (panfrost_emit_vertex_data);
//   - the varyings linked as panfrost_emit_varying_descs links them: the general buffer laid out by the fragment
//     shader's inputs (pan_assign_varyings, in the vertex shader's order, the consumer's format and size), the position
//     buffer (MALI_SNAP_4), a record of MALI_CONSTANT for an output nobody reads;
//   - the words each stage pushes, copied out of the uniform data by the program's map (panfrost_emit_const_buf), and
//     the uniform buffer table naming the same data (panfrost_emit_ubo);
//   - the Renderer States with the shaders' addresses merged in, the target's Blend descriptor (off, or source-over).
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "mali.hpp"
#include "pass.hpp"
#include "program.h"

namespace hakios::mali {

struct DrawInput {
    // per attribute of the program, VERTEX_COUNT of its elements (four floats each for the programs here)
    const float* attributes[4] = {nullptr, nullptr, nullptr, nullptr};
    uint32_t vertexCount = 3;
    // uniform buffer 0: what the program's pushed words are read from (bytes)
    const void* uniforms = nullptr;
    uint32_t uniformSize = 0;
    float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool blendOver = false;     // Metal's source-over, premultiplied: One, OneMinusSourceAlpha
    // texture 0 and sampler 0 of a program that samples (RGBA8Unorm, linear rows, one level), and its filtering
    const Bo* texture = nullptr;
    uint64_t textureOffset = 0;
    uint32_t textureWidth = 0, textureHeight = 0, textureRowBytes = 0;
    bool nearest = true;
    // Metal's depth test Less with writes, in the tile buffer alone (cleared to 1.0, not written back): what a draw of
    // several triangles needs to keep the nearest
    bool depthTest = false;
    // an index buffer: INDEX_COUNT 16-bit indices into the VERTEX_COUNT vertices (triangles of them)
    const uint16_t* indices = nullptr;
    uint32_t indexCount = 0;
};

class DrawPass {
public:
    static std::unique_ptr<DrawPass> make(Device& device, const Target& target, const hm_program& program,
                                          const DrawInput& input, std::string* why);
    int submit(Submission* draw, Submission* frame);
    uint32_t vertexStatus() const;
    uint32_t tilerStatus() const;
    uint32_t fragmentStatus() const;
    void dump(FILE* fp) const;
    // what the vertex job wrote into the position varying buffer: four floats a vertex, in pixels after the viewport
    const float* positionsWritten() const;

    static constexpr uint32_t kVertexJob = 0x000, kTilerJob = 0x100, kFragmentJob = 0x200, kFbd = 0x240,
        kHeapDesc = 0x300, kTilerCtx = 0x340, kVsRsd = 0x400, kFsRsd = 0x440, kAttributes = 0x4c0,
        kAttributeBuffers = 0x500, kVaryings = 0x5c0, kVaryingBuffers = 0x640, kViewport = 0x680,
        kLocalStorage = 0x6c0, kUbos = 0x700, kVsPush = 0x740, kFsPush = 0x7c0, kVertices = 0x800,
        kGeneral = 0xc00, kPositions = 0xe00, kVsCode = 0x1000, kFsCode = 0x1800, kUniforms = 0x2000, kTexture = 0x3000,
        kSurface = 0x3040, kSampler = 0x3080, kSysvals = 0x3100, kSysvalSize = 32, kIndices = 0x3200, kSize = 0x4000;

private:
    DrawPass(Device& device) : device_(device) {}
    Device& device_;
    Target target_;
    std::unique_ptr<Bo> cmd_;
    const Bo* heap_ = nullptr;
    const Bo* texture_ = nullptr;
};

// One dispatch of a compute kernel of shaders_v7.h (struct hm_kernel) as GENX(jm_launch_grid) submits it on v7: one
// compute job on slot 1, its Renderer State with the code's address merged in, the words it pushes copied out of the
// uniform data by the kernel's map, the uniform buffer table naming the same data, Local Storage without thread or
// workgroup storage.
class ComputePass {
public:
    static std::unique_ptr<ComputePass> make(Device& device, const hm_kernel& kernel, const unsigned groups[3],
                                             const void* uniforms, uint32_t uniformSize, std::string* why);
    // with the buffers the kernel writes (each remembers the job)
    int submit(const std::vector<const Bo*>& uses, Submission* out);
    uint32_t status() const;
    void dump(FILE* fp) const;

    static constexpr uint32_t kJob = 0x000, kRsd = 0x100, kUbos = 0x140, kPush = 0x180, kLocalStorage = 0x1c0,
        kCode = 0x400, kUniforms = 0xc00, kSize = 0x1000;

private:
    ComputePass(Device& device) : device_(device) {}
    Device& device_;
    std::unique_ptr<Bo> cmd_;
};

}  // namespace hakios::mali
