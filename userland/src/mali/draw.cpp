// DrawPass (draw.hpp).
#include <cmath>
#include <cstring>
#include <vector>

#include "draw.hpp"
#include "shaders_v7.h"
#include "v7.h"

namespace hakios::mali {

// One draw's descriptors in a block of the pass's buffer (CPU and GPU addresses CPU, GPU), its vertex job numbered VINDEX
// and its tiler job VINDEX + 1, depending on the vertex job and (pan_jc_add_job: tiler jobs are chained) on the tiler
// job PREV_TILER before it, NEXT after it in the chain. 0, or the reason the draw cannot be written.
static std::string writeDraw(Device& device, const Target& t, const hm_program& p, const DrawInput& in, uint8_t* cpu,
                             uint64_t gpu, uint64_t tilerCtx, unsigned vIndex, unsigned prevTiler, uint64_t next) {
    auto no = [&p](const std::string& s) { return std::string(p.name) + ": " + s; };
    const struct hm_v7_sizes* sz = hm_v7_sizes();
    const bool bgra = t.format == Format::BGRA8Unorm;
    using D = DrawPass;
    constexpr uint32_t kVertexJob = D::kVertexJob, kTilerJob = D::kTilerJob, kVsRsd = D::kVsRsd, kFsRsd = D::kFsRsd,
        kAttributes = D::kAttributes, kAttributeBuffers = D::kAttributeBuffers, kVaryings = D::kVaryings,
        kVaryingBuffers = D::kVaryingBuffers, kViewport = D::kViewport, kLocalStorage = D::kLocalStorage,
        kUbos = D::kUbos, kVsPush = D::kVsPush, kFsPush = D::kFsPush, kVertices = D::kVertices, kGeneral = D::kGeneral,
        kPositions = D::kPositions, kVsCode = D::kVsCode, kFsCode = D::kFsCode, kUniforms = D::kUniforms,
        kTexture = D::kTexture, kSurface = D::kSurface, kSampler = D::kSampler, kSysvals = D::kSysvals,
        kSysvalSize = D::kSysvalSize, kIndices = D::kIndices, kSize = D::kSize;
    (void)device;
    const unsigned per = in.primitive == DrawInput::Primitive::Triangles ? 3 :
                         in.primitive == DrawInput::Primitive::Lines ? 2 : 1;
    if (in.primitive != DrawInput::Primitive::Triangles && in.indices != nullptr)
        return no("indices with lines or points (not served yet)");
    if (in.indices == nullptr && (in.vertexCount < per || in.vertexCount % per != 0 || in.vertexCount > 48))
        return no("a vertex count that is not a whole number of primitives, up to 48");
    if (in.indices != nullptr) {
        if (in.vertexCount < 1 || in.vertexCount > 48 || in.indexCount < 3 || in.indexCount % 3 != 0 ||
            in.indexCount * 2 > kSize - kIndices)
            return no("an indexed draw of 1 to 48 vertices and a whole number of triangles of indices");
        for (uint32_t i = 0; i < in.indexCount; i++) {
            if (in.indices[i] >= in.vertexCount) return no("an index past the vertices");
        }
    }
    if (p.attribute_count > 4) return no("more than four vertex attributes");
    uint32_t vertexBytes = 0;
    for (uint32_t a = 0; a < p.attribute_count; a++) {
        if (in.attributes[a] == nullptr) return no("attribute " + std::to_string(a) + " has no data");
        vertexBytes += p.attribute_sizes[a] * in.vertexCount;
        for (uint32_t i = 0; i < p.attribute_sizes[a] / 4 * in.vertexCount; i++) {
            if (!std::isfinite(in.attributes[a][i])) return no("an attribute value that is not a finite number");
        }
    }
    if (vertexBytes > kGeneral - kVertices) return no("the vertices do not fit");
    if (p.texture_count > 1 || p.sampler_count > 1) return no("more than one texture or sampler");
    if (p.texture_count != 0) {
        const Bo* tb = in.texture;
        if (tb == nullptr) return no("the program samples a texture and none is given");
        uint64_t tbase = tb->gpu() + in.textureOffset;
        if (in.textureWidth == 0 || in.textureHeight == 0 || in.textureWidth > 4096 || in.textureHeight > 4096 ||
            in.textureRowBytes < 4 * in.textureWidth || (tbase & 63) != 0 || (in.textureRowBytes & 63) != 0 ||
            in.textureOffset + static_cast<uint64_t>(in.textureRowBytes) * (in.textureHeight - 1) + 4ull * in.textureWidth > tb->size())
            return no("the texture is not 64-byte aligned rows of RGBA8 inside its buffer (pan_layout.c, v7)");
    }
    if (in.blendOver && !p.can_blend) return no("the program has no blended state");
    // uniform buffer 0 is the caller's, 1 the system values (compiler.c: the viewport's scale and offset)
    auto pushable = [&](const hm_push_word& w) {
        return (w.ubo == 0 && w.offset + 4u <= in.uniformSize) || (w.ubo == 1 && w.offset + 4u <= kSysvalSize);
    };
    for (uint32_t i = 0; i < p.vs_push_count; i++) {
        if (!pushable(p.vs_push[i])) return no("the vertex shader pushes a word the uniform data does not hold");
    }
    for (uint32_t i = 0; i < p.fs_push_count; i++) {
        if (!pushable(p.fs_push[i])) return no("the fragment shader pushes a word the uniform data does not hold");
    }
    if (in.uniformSize > kTexture - kUniforms) return no("the uniform data do not fit");
    if (p.vs_size > kFsCode - kVsCode || p.fs_size > kUniforms - kFsCode - 128) return no("the shaders do not fit");

    // the varyings, linked (pan_assign_varyings, panfrost_emit_varying_descs): general buffer offsets by the vertex
    // shader's output order, for the outputs the fragment shader reads, in the reader's format and size
    std::vector<int32_t> offset(p.vs_out_count, -1);
    std::vector<uint32_t> format(p.vs_out_count, 0);
    uint32_t stride = 0;
    for (uint32_t i = 0; i < p.vs_out_count; i++) {
        for (uint32_t j = 0; j < p.fs_in_count; j++) {
            if (p.fs_in[j].location == p.vs_out[i].location && p.vs_out[i].location != HM_VARYING_SLOT_POS) {
                offset[i] = static_cast<int32_t>(stride);
                format[i] = p.fs_in[j].format;
                stride += p.fs_in[j].size;
            }
        }
    }
    for (uint32_t j = 0; j < p.fs_in_count; j++) {
        bool found = false;
        for (uint32_t i = 0; i < p.vs_out_count; i++) found |= p.vs_out[i].location == p.fs_in[j].location;
        if (!found) return no("the fragment shader reads a varying the vertex shader does not write");
    }
    if (stride * in.vertexCount > kPositions - kGeneral) return no("the varyings do not fit");
    if (p.vs_out_count + p.fs_in_count > 16) return no("more than sixteen varying records");

    memset(cpu, 0, kSize);
    // the shaders, their Renderer State with their addresses merged in, and the target's Blend descriptor
    memcpy(cpu + kVsCode, p.vs_code, p.vs_size);
    memcpy(cpu + kFsCode, p.fs_code, p.fs_size);
    uint32_t rsd[16];
    memcpy(rsd, p.vs_rsd, sizeof(rsd));
    rsd[0] |= static_cast<uint32_t>(gpu + kVsCode);
    rsd[1] |= static_cast<uint32_t>((gpu + kVsCode) >> 32);
    memcpy(cpu + kVsRsd, rsd, sizeof(rsd));
    if (in.depthTest && in.blendOver) return no("a depth test with blending (not served yet)");
    memcpy(rsd, in.depthTest ? p.fs_rsd_depth : in.blendOver ? p.fs_rsd_blended : p.fs_rsd, sizeof(rsd));
    rsd[0] |= static_cast<uint32_t>(gpu + kFsCode);
    rsd[1] |= static_cast<uint32_t>((gpu + kFsCode) >> 32);
    memcpy(cpu + kFsRsd, rsd, sizeof(rsd));
    memcpy(cpu + kFsRsd + sz->renderer_state,
           in.blendOver ? (bgra ? p.blend_over_bgra : p.blend_over) : (bgra ? p.blend_off_bgra : p.blend_off), sz->blend);

    // the vertex attributes, one buffer each, and the tables' ends
    uint64_t at = kVertices;
    for (uint32_t a = 0; a < p.attribute_count; a++) {
        uint32_t bytes = p.attribute_sizes[a] * in.vertexCount;
        memcpy(cpu + at, in.attributes[a], bytes);
        hm_v7_attribute(cpu + kAttributes + a * sz->attribute, a, p.attribute_formats[a], 0, true);
        hm_v7_attribute_buffer(cpu + kAttributeBuffers + a * sz->attribute_buffer, gpu + at, p.attribute_sizes[a], bytes);
        at = (at + bytes + 63) & ~63ull;
    }
    hm_v7_attribute_buffer(cpu + kAttributeBuffers + p.attribute_count * sz->attribute_buffer, 0, 0, 0);

    // the varying records: the vertex shader's (producer), then the fragment shader's (consumer); buffers GENERAL 0,
    // POSITION 1, then the table's end
    for (uint32_t i = 0; i < p.vs_out_count; i++) {
        uint8_t* rec = cpu + kVaryings + i * sz->attribute;
        if (p.vs_out[i].location == HM_VARYING_SLOT_POS) {
            hm_v7_attribute(rec, 1, hm_v7_position_format(), 0, false);
        } else if (offset[i] < 0) {
            hm_v7_attribute(rec, 0, hm_v7_constant_format(), 0, false);
        } else {
            hm_v7_attribute(rec, 0, format[i], offset[i], false);
        }
    }
    for (uint32_t j = 0; j < p.fs_in_count; j++) {
        int32_t off = 0;
        for (uint32_t i = 0; i < p.vs_out_count; i++) {
            if (p.vs_out[i].location == p.fs_in[j].location) off = offset[i];
        }
        hm_v7_attribute(cpu + kVaryings + (p.vs_out_count + j) * sz->attribute, 0, p.fs_in[j].format, off, false);
    }
    if (stride != 0) {
        hm_v7_attribute_buffer(cpu + kVaryingBuffers, gpu + kGeneral, stride, stride * in.vertexCount);
    }
    hm_v7_attribute_buffer(cpu + kVaryingBuffers + sz->attribute_buffer, gpu + kPositions, 16, 16 * in.vertexCount);
    hm_v7_attribute_buffer(cpu + kVaryingBuffers + 2 * sz->attribute_buffer, 0, 0, 0);

    // the uniforms: buffer 0 the caller's, buffer 1 the system values -- the viewport transform's scale and offset
    // (w/2, h/2, 0.5), as panfrost_upload_sysvals gives PAN_SYSVAL_VIEWPORT_SCALE and _OFFSET for a viewport of the whole
    // target with depth 0..1 -- the table naming both, and each stage's pushed words (panfrost_emit_const_buf)
    if (in.uniformSize != 0) {
        memcpy(cpu + kUniforms, in.uniforms, in.uniformSize);
        hm_v7_uniform_buffer(cpu + kUbos, gpu + kUniforms, in.uniformSize);
    }
    float sysvals[8] = {t.width / 2.0f, t.height / 2.0f, 0.5f, 0.0f, t.width / 2.0f, t.height / 2.0f, 0.5f, 0.0f};
    memcpy(cpu + kSysvals, sysvals, sizeof(sysvals));
    hm_v7_uniform_buffer(cpu + kUbos + sz->uniform_buffer, gpu + kSysvals, sizeof(sysvals));
    auto word = [&](const hm_push_word& w) {
        return w.ubo == 0 ? static_cast<const uint8_t*>(in.uniforms) + w.offset : cpu + kSysvals + w.offset;
    };
    for (uint32_t i = 0; i < p.vs_push_count; i++) memcpy(cpu + kVsPush + 4 * i, word(p.vs_push[i]), 4);
    for (uint32_t i = 0; i < p.fs_push_count; i++) memcpy(cpu + kFsPush + 4 * i, word(p.fs_push[i]), 4);

    // texture 0 and sampler 0 (GENX(panfrost_new_texture), panfrost_create_sampler_state)
    if (p.texture_count != 0) {
        hm_v7_surface_with_stride(cpu + kSurface, in.texture->gpu() + in.textureOffset, static_cast<int32_t>(in.textureRowBytes), 0);
        hm_v7_texture_2d(cpu + kTexture, hm_texture_format_rgba8, hm_texture_swizzle_identity, in.textureWidth,
                         in.textureHeight, gpu + kSurface);
        hm_v7_sampler(cpu + kSampler, in.nearest);
    }
    hm_v7_viewport(cpu + kViewport, 0, 0, t.width - 1, t.height - 1, 0.0f, 1.0f);
    hm_v7_local_storage(cpu + kLocalStorage);
    struct hm_v7_draw vertex = {};
    vertex.state = gpu + kVsRsd;
    vertex.attributes = gpu + kAttributes;
    vertex.attribute_buffers = gpu + kAttributeBuffers;
    vertex.varyings = gpu + kVaryings;
    vertex.varying_buffers = gpu + kVaryingBuffers;
    vertex.thread_storage = gpu + kLocalStorage;
    vertex.push_uniforms = p.vs_push_count ? gpu + kVsPush : 0;
    vertex.uniform_buffers = gpu + kUbos;
    hm_v7_vertex_job(cpu + kVertexJob, vIndex, gpu + kTilerJob, in.vertexCount, &vertex);
    struct hm_v7_draw fragment = {};
    fragment.state = gpu + kFsRsd;
    fragment.position = gpu + kPositions;
    fragment.viewport = gpu + kViewport;
    fragment.varyings = p.fs_in_count ? gpu + kVaryings + p.vs_out_count * sz->attribute : 0;
    fragment.varying_buffers = gpu + kVaryingBuffers;
    fragment.thread_storage = gpu + kLocalStorage;
    fragment.push_uniforms = p.fs_push_count ? gpu + kFsPush : 0;
    fragment.uniform_buffers = gpu + kUbos;
    fragment.textures = p.texture_count ? gpu + kTexture : 0;
    fragment.samplers = p.sampler_count ? gpu + kSampler : 0;
    if (in.indices != nullptr) {
        memcpy(cpu + kIndices, in.indices, 2 * in.indexCount);
        hm_v7_tiler_job_indexed(cpu + kTilerJob, vIndex + 1, vIndex, next, in.vertexCount, tilerCtx, &fragment, in.indexCount,
                                gpu + kIndices);
    } else {
        hm_v7_tiler_job_mode(cpu + kTilerJob, vIndex + 1, vIndex, next, in.vertexCount, tilerCtx, &fragment, 0, 0,
                             in.primitive == DrawInput::Primitive::Lines ? 2 : in.primitive == DrawInput::Primitive::Points ? 1 : 8);
    }

    if (prevTiler != 0) {
        // dependency 2: the tiler job before it (pan_jc_add_job), in Job Header word 5 bits 16..31
        uint32_t w5;
        memcpy(&w5, cpu + kTilerJob + 20, 4);
        w5 |= prevTiler << 16;
        memcpy(cpu + kTilerJob + 20, &w5, 4);
    }
    return std::string();
}

std::unique_ptr<DrawPass> DrawPass::make(Device& device, const Target& t, const hm_program& p, const DrawInput& in,
                                         std::string* why) {
    return makeMany(device, t, {{&p, in}}, why);
}

std::unique_ptr<DrawPass> DrawPass::makeMany(Device& device, const Target& t,
                                             const std::vector<std::pair<const hm_program*, DrawInput>>& draws,
                                             std::string* why) {
    auto no = [why](const std::string& s) {
        if (why) *why = s;
        return std::unique_ptr<DrawPass>();
    };
    if (draws.empty() || draws.size() > kMaxDraws) return no("a pass of 1 to " + std::to_string(kMaxDraws) + " draws");
    if (t.bo == nullptr || t.width == 0 || t.height == 0 || t.width > 4096 || t.height > 4096)
        return no("a draw goes into a target of 1 to 4096 pixels a side");
    const bool bgra = t.format == Format::BGRA8Unorm;
    uint64_t base = t.bo->gpu() + t.offset;
    if ((base & 63) != 0 || (t.rowBytes & 63) != 0 || t.rowBytes < 4 * t.width ||
        t.offset + static_cast<uint64_t>(t.rowBytes) * (t.height - 1) + 4ull * t.width > t.bo->size())
        return no("the target is not 64-byte aligned rows inside its buffer (pan_layout.c, v7)");
    std::unique_ptr<DrawPass> pass(new DrawPass(device));
    pass->target_ = t;
    pass->draws_ = static_cast<uint32_t>(draws.size());
    for (const auto& d : draws) {
        if (d.first->texture_count && d.second.texture) pass->textures_.push_back(d.second.texture);
    }
    pass->cmd_ = device.gpu().newBo(kSize * draws.size(), why);
    if (!pass->cmd_) return nullptr;
    pass->heap_ = device.tilerHeap(why);
    if (pass->heap_ == nullptr) return nullptr;
    uint8_t* cpu = pass->cmd_->cpu();
    uint64_t gpu = pass->cmd_->gpu();
    // the draws, each in a block of its own, chained vertex, tiler, vertex, tiler...; the frame's own descriptors in
    // the first block's room for them
    for (size_t i = 0; i < draws.size(); i++) {
        uint64_t next = i + 1 < draws.size() ? gpu + (i + 1) * kSize + kVertexJob : 0;
        unsigned vIndex = static_cast<unsigned>(2 * i + 1);
        std::string e = writeDraw(device, t, *draws[i].first, draws[i].second, cpu + i * kSize, gpu + i * kSize,
                                  gpu + kTilerCtx, vIndex, i ? vIndex - 1 : 0, next);
        if (!e.empty()) return no("draw " + std::to_string(i) + ", " + e);
    }
    const DrawInput& in = draws[0].second;
    hm_v7_tiler_heap(cpu + kHeapDesc, pass->heap_->gpu(), static_cast<uint32_t>(pass->heap_->size()));
    hm_v7_tiler_context(cpu + kTilerCtx, gpu + kHeapDesc, t.width, t.height,
                        hm_v7_hierarchy_mask(t.width, t.height, device.tilerLevels()));

    struct hm_v7_fb fb = {};
    fb.width = t.width;
    fb.height = t.height;
    fb.bound_max_x = t.width - 1;
    fb.bound_max_y = t.height - 1;
    fb.sample_locations = device.samplePositions().gpu();
    fb.tiler = gpu + kTilerCtx;
    hm_v7_tile_size(4, device.tileBufferBudget(), &fb.tile_size, &fb.cbuf_allocation);
    fb.rt_count = 1;
    fb.z_clear = 1.0f;
    struct hm_v7_rt rt = {};
    rt.format = bgra ? HM_RT_BGRA8_UNORM : HM_RT_RGBA8_UNORM;
    rt.base = base;
    rt.row_stride = t.rowBytes;
    rt.write_enable = true;
    rt.clear = true;
    rt.clean_pixel_write = true;
    rt.dithering = true;
    hm_v7_pack_clear_rgba8(in.clear, rt.clear_words);
    const char* refused = nullptr;
    uint64_t fbd = hm_v7_framebuffer(cpu + kFbd, gpu + kFbd, &fb, &rt, &refused);
    if (fbd == 0) return no(refused);
    hm_v7_fragment_job(cpu + kFragmentJob, 1, 0, fbd, 0, 0, (t.width - 1) >> 4, (t.height - 1) >> 4);
    return pass;
}

const float* DrawPass::positionsWritten() const { return reinterpret_cast<const float*>(cmd_->cpu() + kPositions); }

int DrawPass::submit(Submission* draw, Submission* frame) {
    std::vector<const Bo*> uses = {cmd_.get(), target_.bo, &device_.samplePositions(), heap_};
    for (const Bo* tex : textures_) uses.push_back(tex);
    int error = device_.gpu().submit(cmd_->gpu() + kVertexJob, false, uses, draw);
    if (error == 0) error = device_.gpu().submit(cmd_->gpu() + kFragmentJob, true, uses, frame);
    return error;
}

static uint32_t statusOf(const uint8_t* job) {
    struct hm_v7_job h;
    hm_v7_read_job(job, &h, nullptr);
    return h.exception_status;
}
uint32_t DrawPass::vertexStatus() const { return statusOf(cmd_->cpu() + kVertexJob); }
uint32_t DrawPass::tilerStatus() const { return statusOf(cmd_->cpu() + kTilerJob); }
uint32_t DrawPass::fragmentStatus() const { return statusOf(cmd_->cpu() + kFragmentJob); }

void DrawPass::dump(FILE* fp) const {
    fprintf(fp, "Vertex job:\n");
    hm_v7_print_job(fp, cmd_->cpu() + kVertexJob);
    fprintf(fp, "Tiler job:\n");
    hm_v7_print_job(fp, cmd_->cpu() + kTilerJob);
    fprintf(fp, "Fragment job:\n");
    hm_v7_print_job(fp, cmd_->cpu() + kFragmentJob);
    const float* g = reinterpret_cast<const float*>(cmd_->cpu() + kGeneral);
    const float* q = reinterpret_cast<const float*>(cmd_->cpu() + kPositions);
    fprintf(fp, "position 0 (%g %g %g %g), general varyings of vertex 0: %g %g %g %g\n", q[0], q[1], q[2], q[3], g[0],
            g[1], g[2], g[3]);
}

std::unique_ptr<ComputePass> ComputePass::make(Device& device, const hm_kernel& k, const unsigned groups[3],
                                               const void* uniforms, uint32_t uniformSize, std::string* why) {
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> ubos;
    if (uniformSize != 0) {
        ubos.emplace_back(0, std::vector<uint8_t>(static_cast<const uint8_t*>(uniforms),
                                                  static_cast<const uint8_t*>(uniforms) + uniformSize));
    }
    return makeWith(device, k, groups, k.local, ubos, why);
}

std::unique_ptr<ComputePass> ComputePass::makeWith(Device& device, const hm_kernel& k, const unsigned groups[3],
                                                   const unsigned local[3],
                                                   const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& ubos,
                                                   std::string* why) {
    auto no = [why, &k](const std::string& s) {
        if (why) *why = std::string(k.name) + ": " + s;
        return std::unique_ptr<ComputePass>();
    };
    const struct hm_v7_sizes* sz = hm_v7_sizes();
    for (int i = 0; i < 3; i++) {
        if (groups[i] == 0 || groups[i] > 65535) return no("a workgroup count outside 1..65535");
        if (local[i] == 0 || local[i] > 256) return no("a threadgroup size outside 1..256");
    }
    if (local[0] * local[1] * local[2] > 256) return no("more than 256 threads a threadgroup");
    if (k.tls_size != 0) return no("a kernel that spills to thread storage (not served yet)");
    if (k.size > kUniforms - kCode) return no("the kernel does not fit");
    uint32_t tableEntries = k.ubo_count ? k.ubo_count : 1;
    if (tableEntries * sz->uniform_buffer > kPush - kUbos) return no("too many uniform buffers");
    if (k.push_count * 4 > kCode - kPush) return no("too many pushed words");
    // where each buffer's bytes go, 16-aligned, and every pushed word inside one of them
    std::vector<std::pair<uint32_t, uint32_t>> at;   // (ubo index, offset in the command buffer)
    uint32_t next = kUniforms;
    for (const auto& u : ubos) {
        if (u.first >= tableEntries) return no("uniform buffer " + std::to_string(u.first) + " is past the kernel's table");
        if (next + u.second.size() > kSize) return no("the uniform data do not fit");
        at.emplace_back(u.first, next);
        next = (next + static_cast<uint32_t>(u.second.size()) + 15) & ~15u;
    }
    auto source = [&](const hm_push_word& w) -> const uint8_t* {
        for (size_t i = 0; i < ubos.size(); i++) {
            if (ubos[i].first == w.ubo && w.offset + 4u <= ubos[i].second.size()) return ubos[i].second.data() + w.offset;
        }
        return nullptr;
    };
    for (uint32_t i = 0; i < k.push_count; i++) {
        if (source(k.push[i]) == nullptr)
            return no("the kernel pushes word " + std::to_string(k.push[i].offset) + " of uniform buffer " +
                      std::to_string(k.push[i].ubo) + ", which the data do not hold");
    }
    std::unique_ptr<ComputePass> pass(new ComputePass(device));
    pass->cmd_ = device.gpu().newBo(kSize, why);
    if (!pass->cmd_) return nullptr;
    uint8_t* cpu = pass->cmd_->cpu();
    uint64_t gpu = pass->cmd_->gpu();
    memset(cpu, 0, kSize);
    memcpy(cpu + kCode, k.code, k.size);
    uint32_t rsd[16];
    memcpy(rsd, k.rsd, sizeof(rsd));
    rsd[0] |= static_cast<uint32_t>(gpu + kCode);
    rsd[1] |= static_cast<uint32_t>((gpu + kCode) >> 32);
    memcpy(cpu + kRsd, rsd, sizeof(rsd));
    for (size_t i = 0; i < ubos.size(); i++) {
        memcpy(cpu + at[i].second, ubos[i].second.data(), ubos[i].second.size());
        hm_v7_uniform_buffer(cpu + kUbos + at[i].first * sz->uniform_buffer, gpu + at[i].second,
                             static_cast<uint32_t>(ubos[i].second.size()));
    }
    for (uint32_t i = 0; i < k.push_count; i++) memcpy(cpu + kPush + 4 * i, source(k.push[i]), 4);
    if (k.wls_size != 0) {
        // panfrost_emit_shared_memory: pan_wls_adjust_size(size) * the instances (the grid to powers of two,
        // panfrost_choose_wls_instance_count) * the core id range
        auto pow2 = [](unsigned v) { unsigned p = 1; while (p < v) p <<= 1; return p; };
        unsigned size = pow2(k.wls_size < 128 ? 128 : k.wls_size);
        unsigned instances = pow2(groups[0]) * pow2(groups[1]) * pow2(groups[2]);
        uint64_t bytes = static_cast<uint64_t>(size) * instances * device.coreIdRange();
        if (bytes > (64u << 20)) return no("more than 64 MB of workgroup storage");
        pass->wls_ = device.gpu().newBo(bytes, why);
        if (!pass->wls_) return nullptr;
        hm_v7_local_storage_wls(cpu + kLocalStorage, pass->wls_->gpu(), size, instances);
    } else {
        hm_v7_local_storage(cpu + kLocalStorage);
    }
    struct hm_v7_draw d = {};
    d.state = gpu + kRsd;
    d.thread_storage = gpu + kLocalStorage;
    d.uniform_buffers = gpu + kUbos;
    d.push_uniforms = k.push_count ? gpu + kPush : 0;
    hm_v7_compute_job(cpu + kJob, 1, 0, groups, local, &d);
    return pass;
}

int ComputePass::submit(const std::vector<const Bo*>& uses, Submission* out) {
    std::vector<const Bo*> all = uses;
    all.push_back(cmd_.get());
    if (wls_) all.push_back(wls_.get());
    return device_.gpu().submit(cmd_->gpu() + kJob, false, all, out);
}

uint32_t ComputePass::status() const { return statusOf(cmd_->cpu() + kJob); }

void ComputePass::dump(FILE* fp) const {
    fprintf(fp, "Compute job:\n");
    hm_v7_print_job(fp, cmd_->cpu() + kJob);
}

}  // namespace hakios::mali
