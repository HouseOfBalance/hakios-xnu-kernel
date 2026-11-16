// Metal's command model over hakios-mali, the way an MTLDevice plug-in will use it: textures, render pipelines (a program
// of shaders_v7.h or compiled by compiler.c, with blending and the depth test), compute pipelines (a kernel), and
// command buffers into which render passes (a target with its load action Clear and the draws of an encoder) and
// compute dispatches are encoded, committed to the GPU in their order, and waited for -- MTLCommandBuffer's states:
// NotEnqueued, Committed, Completed, Error, the error naming the first job that did not end DONE.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "draw.hpp"
#include "mali.hpp"
#include "pass.hpp"
#include "program.h"

namespace hakios::mali::metal {

// MTLTexture: a 2D image of one level in GPU memory, linear rows 64-byte aligned (pan_layout.c, v7)
class Texture {
public:
    static std::unique_ptr<Texture> make(Device& device, uint32_t width, uint32_t height, Format format,
                                         std::string* why);
    uint32_t width() const { return target_.width; }
    uint32_t height() const { return target_.height; }
    Format format() const { return target_.format; }
    uint32_t rowBytes() const { return target_.rowBytes; }
    // -replaceRegion: of the whole texture, ROWBYTES apart in BYTES; -getBytes: likewise
    void replace(const void* bytes, uint32_t rowBytes);
    void read(void* bytes, uint32_t rowBytes) const;
    const Target& target() const { return target_; }
    const Bo& bo() const { return *bo_; }

private:
    std::unique_ptr<Bo> bo_;
    Target target_;
};

// MTLRenderPipelineState: a program and the state compiled with it
struct RenderPipeline {
    const hm_program* program = nullptr;
    bool blendSourceOver = false;    // Metal's One, OneMinusSourceAlpha
    bool depthLess = false;          // MTLDepthStencilState: Less, depth written
};

// MTLComputePipelineState
struct ComputePipeline {
    const hm_kernel* kernel = nullptr;
};

class CommandBuffer;

// MTLRenderCommandEncoder for one pass: what is set stays set for the draws after it
class RenderEncoder {
public:
    void setPipeline(const RenderPipeline& p) { pipeline_ = p; }
    // -setVertexBytes:length:atIndex: for attribute INDEX (four floats a vertex here)
    void setVertexBytes(uint32_t index, const float* data, uint32_t floats);
    // -setFragmentBytes: / -setVertexBytes: of the program's uniform buffer 0
    void setUniformBytes(const void* data, uint32_t size);
    void setFragmentTexture(const Texture* texture, bool nearest) { texture_ = texture, nearest_ = nearest; }
    // -drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:N
    void drawTriangles(uint32_t vertexCount);
    // -drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount: indexType:UInt16 indexBuffer:
    void drawIndexedTriangles(uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount);
    void endEncoding();

private:
    friend class CommandBuffer;
    RenderEncoder(CommandBuffer& cb, const Texture& target, const float clear[4]);
    void record(uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount);
    CommandBuffer& cb_;
    const Texture& target_;
    float clear_[4];
    RenderPipeline pipeline_;
    std::vector<std::vector<float>> attributes_ = std::vector<std::vector<float>>(4);
    std::vector<uint8_t> uniforms_;
    const Texture* texture_ = nullptr;
    bool nearest_ = true;
    // the draws so far: their data is kept here until the pass is built at endEncoding
    struct Draw {
        RenderPipeline pipeline;
        std::vector<std::vector<float>> attributes;
        std::vector<uint8_t> uniforms;
        std::vector<uint16_t> indices;
        uint32_t vertexCount;
        const Texture* texture;
        bool nearest;
    };
    std::vector<Draw> draws_;
    bool ended_ = false;
};

class CommandBuffer {
public:
    enum class Status { NotEnqueued, Committed, Completed, Error };

    explicit CommandBuffer(Device& device) : device_(device) {}
    // MTLRenderPassDescriptor with colour attachment 0 TARGET, load action Clear to CLEAR, store action Store
    RenderEncoder* renderCommandEncoder(const Texture& target, const float clear[4]);
    // MTLComputeCommandEncoder's one dispatch: GROUPS threadgroups of the kernel's size, uniform buffer 0 UNIFORMS,
    // the buffers the kernel writes WRITES
    void dispatch(const ComputePipeline& pipeline, const unsigned groups[3], const void* uniforms, uint32_t size,
                  const std::vector<const Bo*>& writes);
    void commit();
    void waitUntilCompleted();
    Status status() const { return status_; }
    const std::string& error() const { return error_; }
    uint64_t gpuMicroseconds() const { return gpuUs_; }

private:
    friend class RenderEncoder;
    void fail(const std::string& why);
    Device& device_;
    std::vector<std::unique_ptr<RenderEncoder>> encoders_;
    struct Work {
        std::unique_ptr<DrawPass> render;
        std::unique_ptr<ComputePass> compute;
        std::vector<const Bo*> writes;
        int clearIndex = -1;            // a pass with no draw: clears_[clearIndex]
        Submission first, last;
    };
    std::vector<Work> work_;
    std::vector<std::unique_ptr<ClearPass>> clears_;
    Status status_ = Status::NotEnqueued;
    std::string error_;
    uint64_t committedUs_ = 0, gpuUs_ = 0;
};

}  // namespace hakios::mali::metal
