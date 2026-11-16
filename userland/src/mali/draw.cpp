// DrawPass (draw.hpp).
#include <cmath>
#include <cstring>
#include <vector>

#include "draw.hpp"
#include "shaders_v7.h"
#include "v7.h"

namespace hakios::mali {

std::unique_ptr<DrawPass> DrawPass::make(Device& device, const Target& t, const hm_program& p, const DrawInput& in,
                                         std::string* why) {
    auto no = [why, &p](const std::string& s) {
        if (why) *why = std::string(p.name) + ": " + s;
        return std::unique_ptr<DrawPass>();
    };
    const struct hm_v7_sizes* sz = hm_v7_sizes();
    // the programs' viewport is 64x64 (tools/bishaders.c)
    if (t.bo == nullptr || t.width != 64 || t.height != 64 || t.format != Format::RGBA8Unorm)
        return no("a draw goes into a 64x64 RGBA8Unorm target only (the programs' viewport)");
    uint64_t base = t.bo->gpu() + t.offset;
    if ((base & 63) != 0 || (t.rowBytes & 63) != 0 || t.rowBytes < 256 ||
        t.offset + static_cast<uint64_t>(t.rowBytes) * 63 + 256 > t.bo->size())
        return no("the target is not 64-byte aligned rows inside its buffer (pan_layout.c, v7)");
    if (in.vertexCount < 3 || in.vertexCount % 3 != 0 || in.vertexCount > 48)
        return no("a vertex count that is not a whole number of triangles, 3 to 48");
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
    for (uint32_t i = 0; i < p.vs_push_count; i++) {
        if (p.vs_push[i].ubo != 0 || p.vs_push[i].offset + 4u > in.uniformSize)
            return no("the vertex shader pushes a word the uniform data does not hold");
    }
    for (uint32_t i = 0; i < p.fs_push_count; i++) {
        if (p.fs_push[i].ubo != 0 || p.fs_push[i].offset + 4u > in.uniformSize)
            return no("the fragment shader pushes a word the uniform data does not hold");
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

    std::unique_ptr<DrawPass> pass(new DrawPass(device));
    pass->target_ = t;
    pass->texture_ = p.texture_count ? in.texture : nullptr;
    pass->cmd_ = device.gpu().newBo(kSize, why);
    if (!pass->cmd_) return nullptr;
    pass->heap_ = device.tilerHeap(why);
    if (pass->heap_ == nullptr) return nullptr;
    uint8_t* cpu = pass->cmd_->cpu();
    uint64_t gpu = pass->cmd_->gpu();
    memset(cpu, 0, kSize);

    // the shaders, their Renderer State with their addresses merged in, and the target's Blend descriptor
    memcpy(cpu + kVsCode, p.vs_code, p.vs_size);
    memcpy(cpu + kFsCode, p.fs_code, p.fs_size);
    uint32_t rsd[16];
    memcpy(rsd, p.vs_rsd, sizeof(rsd));
    rsd[0] |= static_cast<uint32_t>(gpu + kVsCode);
    rsd[1] |= static_cast<uint32_t>((gpu + kVsCode) >> 32);
    memcpy(cpu + kVsRsd, rsd, sizeof(rsd));
    memcpy(rsd, in.blendOver ? p.fs_rsd_blended : p.fs_rsd, sizeof(rsd));
    rsd[0] |= static_cast<uint32_t>(gpu + kFsCode);
    rsd[1] |= static_cast<uint32_t>((gpu + kFsCode) >> 32);
    memcpy(cpu + kFsRsd, rsd, sizeof(rsd));
    memcpy(cpu + kFsRsd + sz->renderer_state, in.blendOver ? p.blend_over : p.blend_off, sz->blend);

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

    // the uniforms: buffer 0, its table, and each stage's pushed words
    if (in.uniformSize != 0) {
        memcpy(cpu + kUniforms, in.uniforms, in.uniformSize);
        hm_v7_uniform_buffer(cpu + kUbos, gpu + kUniforms, in.uniformSize);
    }
    const uint8_t* u = static_cast<const uint8_t*>(in.uniforms);
    for (uint32_t i = 0; i < p.vs_push_count; i++) memcpy(cpu + kVsPush + 4 * i, u + p.vs_push[i].offset, 4);
    for (uint32_t i = 0; i < p.fs_push_count; i++) memcpy(cpu + kFsPush + 4 * i, u + p.fs_push[i].offset, 4);

    // texture 0 and sampler 0 (GENX(panfrost_new_texture), panfrost_create_sampler_state)
    if (p.texture_count != 0) {
        hm_v7_surface_with_stride(cpu + kSurface, in.texture->gpu() + in.textureOffset, static_cast<int32_t>(in.textureRowBytes), 0);
        hm_v7_texture_2d(cpu + kTexture, hm_texture_format_rgba8, hm_texture_swizzle_identity, in.textureWidth,
                         in.textureHeight, gpu + kSurface);
        hm_v7_sampler(cpu + kSampler, in.nearest);
    }
    hm_v7_viewport(cpu + kViewport, 0, 0, 63, 63, 0.0f, 1.0f);
    hm_v7_local_storage(cpu + kLocalStorage);
    hm_v7_tiler_heap(cpu + kHeapDesc, pass->heap_->gpu(), static_cast<uint32_t>(pass->heap_->size()));
    hm_v7_tiler_context(cpu + kTilerCtx, gpu + kHeapDesc, 64, 64, hm_v7_hierarchy_mask(64, 64, device.tilerLevels()));

    struct hm_v7_draw vertex = {};
    vertex.state = gpu + kVsRsd;
    vertex.attributes = gpu + kAttributes;
    vertex.attribute_buffers = gpu + kAttributeBuffers;
    vertex.varyings = gpu + kVaryings;
    vertex.varying_buffers = gpu + kVaryingBuffers;
    vertex.thread_storage = gpu + kLocalStorage;
    vertex.push_uniforms = p.vs_push_count ? gpu + kVsPush : 0;
    vertex.uniform_buffers = in.uniformSize ? gpu + kUbos : 0;
    hm_v7_vertex_job(cpu + kVertexJob, 1, gpu + kTilerJob, in.vertexCount, &vertex);
    struct hm_v7_draw fragment = {};
    fragment.state = gpu + kFsRsd;
    fragment.position = gpu + kPositions;
    fragment.viewport = gpu + kViewport;
    fragment.varyings = p.fs_in_count ? gpu + kVaryings + p.vs_out_count * sz->attribute : 0;
    fragment.varying_buffers = gpu + kVaryingBuffers;
    fragment.thread_storage = gpu + kLocalStorage;
    fragment.push_uniforms = p.fs_push_count ? gpu + kFsPush : 0;
    fragment.uniform_buffers = in.uniformSize ? gpu + kUbos : 0;
    fragment.textures = p.texture_count ? gpu + kTexture : 0;
    fragment.samplers = p.sampler_count ? gpu + kSampler : 0;
    hm_v7_tiler_job(cpu + kTilerJob, 2, 1, 0, in.vertexCount, gpu + kTilerCtx, &fragment);

    struct hm_v7_fb fb = {};
    fb.width = fb.height = 64;
    fb.bound_max_x = fb.bound_max_y = 63;
    fb.sample_locations = device.samplePositions().gpu();
    fb.tiler = gpu + kTilerCtx;
    hm_v7_tile_size(4, device.tileBufferBudget(), &fb.tile_size, &fb.cbuf_allocation);
    fb.rt_count = 1;
    struct hm_v7_rt rt = {};
    rt.format = HM_RT_RGBA8_UNORM;
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
    hm_v7_fragment_job(cpu + kFragmentJob, 1, 0, fbd, 0, 0, 3, 3);
    return pass;
}

int DrawPass::submit(Submission* draw, Submission* frame) {
    std::vector<const Bo*> uses = {cmd_.get(), target_.bo, &device_.samplePositions(), heap_};
    if (texture_ != nullptr) uses.push_back(texture_);
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

}  // namespace hakios::mali
