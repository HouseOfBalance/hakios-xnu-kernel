// Metal's command model over hakios-mali (metal.hpp).
#include <cstring>

#include "metal.hpp"

namespace hakios::mali::metal {

std::unique_ptr<Texture> Texture::make(Device& device, uint32_t width, uint32_t height, Format format,
                                       std::string* why) {
    if (width == 0 || height == 0 || width > 4096 || height > 4096) {
        if (why) *why = "a texture of " + std::to_string(width) + "x" + std::to_string(height) + " (1 to 4096 a side)";
        return nullptr;
    }
    std::unique_ptr<Texture> t(new Texture());
    uint32_t rowBytes = (4 * width + 63) & ~63u;     // pan_layout.c: linear rows 64-byte aligned on v7
    t->bo_ = device.gpu().newBo(static_cast<uint64_t>(rowBytes) * height, why);
    if (!t->bo_) return nullptr;
    t->target_.bo = t->bo_.get();
    t->target_.width = width;
    t->target_.height = height;
    t->target_.rowBytes = rowBytes;
    t->target_.format = format;
    return t;
}

void Texture::replace(const void* bytes, uint32_t rowBytes) {
    for (uint32_t y = 0; y < target_.height; y++) {
        memcpy(bo_->cpu() + static_cast<uint64_t>(y) * target_.rowBytes,
               static_cast<const uint8_t*>(bytes) + static_cast<uint64_t>(y) * rowBytes, 4ull * target_.width);
    }
}

void Texture::read(void* bytes, uint32_t rowBytes) const {
    for (uint32_t y = 0; y < target_.height; y++) {
        memcpy(static_cast<uint8_t*>(bytes) + static_cast<uint64_t>(y) * rowBytes,
               bo_->cpu() + static_cast<uint64_t>(y) * target_.rowBytes, 4ull * target_.width);
    }
}

RenderEncoder::RenderEncoder(CommandBuffer& cb, const Texture& target, const float clear[4]) : cb_(cb), target_(target) {
    memcpy(clear_, clear, sizeof(clear_));
}

void RenderEncoder::setVertexBytes(uint32_t index, const float* data, uint32_t floats) {
    if (index < attributes_.size()) attributes_[index].assign(data, data + floats);
}

void RenderEncoder::setUniformBytes(const void* data, uint32_t size) {
    uniforms_.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
}

void RenderEncoder::record(uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount) {
    if (ended_) {
        cb_.fail("a draw after endEncoding");
        return;
    }
    if (pipeline_.program == nullptr) {
        cb_.fail("a draw with no render pipeline state set");
        return;
    }
    Draw d{pipeline_, attributes_, uniforms_, {}, vertexCount, texture_, nearest_};
    if (indices) d.indices.assign(indices, indices + indexCount);
    draws_.push_back(std::move(d));
}

void RenderEncoder::drawTriangles(uint32_t vertexCount) { record(vertexCount, nullptr, 0); }

void RenderEncoder::drawIndexedTriangles(uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount) {
    record(vertexCount, indices, indexCount);
}

// the pass: its draws into one DrawPass (an encoder that drew nothing is a clear alone, ClearPass's frame)
void RenderEncoder::endEncoding() {
    if (ended_) return;
    ended_ = true;
    std::string why;
    CommandBuffer::Work w;
    if (draws_.empty()) {
        // Metal: an encoder with no draw still runs its load and store actions
        auto clear = ClearPass::make(cb_.device_, target_.target(), clear_, ClearOptions(), &why);
        if (!clear) {
            cb_.fail("the pass: " + why);
            return;
        }
        // kept as a one-draw-free pass: a ClearPass submitted as the render work's fragment chain
        cb_.clears_.push_back(std::move(clear));
        w.clearIndex = static_cast<int>(cb_.clears_.size()) - 1;
        cb_.work_.push_back(std::move(w));
        return;
    }
    std::vector<std::pair<const hm_program*, DrawInput>> draws;
    for (const Draw& d : draws_) {
        DrawInput in;
        for (size_t a = 0; a < 4; a++) {
            in.attributes[a] = d.attributes[a].empty() ? nullptr : d.attributes[a].data();
        }
        in.vertexCount = d.vertexCount;
        in.uniforms = d.uniforms.empty() ? nullptr : d.uniforms.data();
        in.uniformSize = static_cast<uint32_t>(d.uniforms.size());
        memcpy(in.clear, clear_, sizeof(in.clear));
        in.blendOver = d.pipeline.blendSourceOver;
        in.depthTest = d.pipeline.depthLess;
        if (d.texture) {
            in.texture = &d.texture->bo();
            in.textureWidth = d.texture->width();
            in.textureHeight = d.texture->height();
            in.textureRowBytes = d.texture->rowBytes();
            in.nearest = d.nearest;
        }
        if (!d.indices.empty()) {
            in.indices = d.indices.data();
            in.indexCount = static_cast<uint32_t>(d.indices.size());
        }
        draws.emplace_back(d.pipeline.program, in);
    }
    w.render = DrawPass::makeMany(cb_.device_, target_.target(), draws, &why);
    if (!w.render) {
        cb_.fail("the pass: " + why);
        return;
    }
    cb_.work_.push_back(std::move(w));
}

RenderEncoder* CommandBuffer::renderCommandEncoder(const Texture& target, const float clear[4]) {
    encoders_.emplace_back(new RenderEncoder(*this, target, clear));
    return encoders_.back().get();
}

void CommandBuffer::dispatch(const ComputePipeline& pipeline, const unsigned groups[3], const void* uniforms,
                             uint32_t size, const std::vector<const Bo*>& writes) {
    if (pipeline.kernel == nullptr) {
        fail("a dispatch with no compute pipeline state set");
        return;
    }
    std::string why;
    Work w;
    w.compute = ComputePass::make(device_, *pipeline.kernel, groups, uniforms, size, &why);
    if (!w.compute) {
        fail("the dispatch: " + why);
        return;
    }
    w.writes = writes;
    work_.push_back(std::move(w));
}

void CommandBuffer::fail(const std::string& why) {
    if (status_ != Status::Error) {
        status_ = Status::Error;
        error_ = why;
    }
}

// in the order encoded: the kernel's queue is in order, so a pass's draws are done before the next pass reads them
void CommandBuffer::commit() {
    for (auto& e : encoders_) e->endEncoding();
    if (status_ == Status::Error) return;
    committedUs_ = device_.gpu().transport().nowUs();
    for (Work& w : work_) {
        int e = 0;
        if (w.render) {
            e = w.render->submit(&w.first, &w.last);
        } else if (w.compute) {
            e = w.compute->submit(w.writes, &w.last);
        } else if (w.clearIndex >= 0) {
            e = clears_[w.clearIndex]->submit(&w.last);
        }
        if (e != 0) {
            fail(std::string("SUBMIT: ") + strerror(e));
            return;
        }
    }
    status_ = Status::Committed;
}

void CommandBuffer::waitUntilCompleted() {
    if (status_ != Status::Committed) return;
    for (Work& w : work_) {
        if (device_.gpu().wait(w.last, 5000000) != 0) {
            fail("a job chain not done in 5 s");
            return;
        }
    }
    gpuUs_ = device_.gpu().transport().nowUs() - committedUs_;
    // what each job left in its header: the first that is not DONE is the command buffer's error
    for (size_t i = 0; i < work_.size(); i++) {
        const Work& w = work_[i];
        uint32_t s = w.render ? (w.render->fragmentStatus() & 0xff) != 1 ? w.render->fragmentStatus()
                                 : (w.render->tilerStatus() & 0xff) != 1 ? w.render->tilerStatus() : w.render->vertexStatus()
                   : w.compute ? w.compute->status() : clears_[w.clearIndex]->status();
        if ((s & 0xff) != 1) {
            fail("work " + std::to_string(i) + " ended " + exceptionName(s));
            return;
        }
    }
    status_ = Status::Completed;
}

}  // namespace hakios::mali::metal
