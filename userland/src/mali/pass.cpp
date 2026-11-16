// The device, the cleared frame and the Write Value job (pass.hpp).
#include <cerrno>
#include <cmath>
#include <cstring>

#include "pass.hpp"
#include "shaders_v7.h"
#include "v7.h"

namespace hakios::mali {

namespace {
constexpr uint32_t kHeapBytes = 2u << 20;       // the tiler heap: Panfrost takes 128 MB growable (pan_device.c); a
                                                // frame without primitives puts nothing in it
}

int Device::init(std::string* why) {
    if (gpu_.param(kParamGpuProdId, &productId_) != 0) {
        if (why) *why = "GET_PARAM GPU_PROD_ID failed";
        return -1;
    }
    model_ = modelOf(productId_);
    if (model_ == nullptr) {
        if (why) {
            char b[120];
            snprintf(b, sizeof(b), "the GPU is product %#llx, not a Bifrost v7 one of pan_props.c's table: these are v7 "
                     "descriptors", static_cast<unsigned long long>(productId_));
            *why = b;
        }
        return -1;
    }
    uint64_t features = 0;
    if (gpu_.param(kParamTilerFeatures, &features) != 0) {
        if (why) *why = "GET_PARAM TILER_FEATURES failed";
        return -1;
    }
    tilerLevels_ = (features >> 8) & 0xf;      // panfrost_query_tiler_features
    if (tilerLevels_ < 2) {
        if (why) *why = "TILER_FEATURES says " + std::to_string(tilerLevels_) + " hierarchy levels (Panfrost asserts >= 2)";
        return -1;
    }
    samplePositions_ = gpu_.newBo(hm_v7_sizes()->sample_positions, why);
    if (!samplePositions_) {
        return -1;
    }
    hm_v7_sample_positions(samplePositions_->cpu());
    return 0;
}

const Bo* Device::tilerHeap(std::string* why) {
    if (!tilerHeap_) {
        tilerHeap_ = gpu_.newBo(kHeapBytes, why);
    }
    return tilerHeap_.get();
}

std::unique_ptr<ClearPass> ClearPass::make(Device& device, const Target& t, const float rgba[4],
                                           const ClearOptions& o, std::string* why) {
    auto no = [why](const std::string& s) {
        if (why) *why = s;
        return std::unique_ptr<ClearPass>();
    };
    if (t.bo == nullptr) return no("the target has no buffer");
    if (t.width == 0 || t.height == 0 || t.width > 4096 || t.height > 4096)
        return no("a target of " + std::to_string(t.width) + "x" + std::to_string(t.height) + " (1 to 4096 a side)");
    if (t.rowBytes < 4 * t.width) return no("a row of " + std::to_string(t.rowBytes) + " bytes for " +
                                            std::to_string(t.width) + " pixels of 4 bytes");
    uint64_t base = t.bo->gpu() + t.offset;
    // pan_layout.c: on v7 a linear RGBA8 image's offset and row stride are both 64-byte aligned (format_minimum_alignment)
    if ((base & 63) != 0) return no("the target's first pixel is not 64-byte aligned (pan_layout.c, v7)");
    if ((t.rowBytes & 63) != 0) return no("the target's row of " + std::to_string(t.rowBytes) +
                                          " bytes is not a multiple of 64 (pan_layout.c, v7)");
    uint64_t span = static_cast<uint64_t>(t.rowBytes) * (t.height - 1) + 4ull * t.width;
    if (t.offset > t.bo->size() || span > t.bo->size() - t.offset)
        return no("the target's " + std::to_string(t.height) + " rows do not fit in its buffer");
    for (int i = 0; i < 4; i++) {
        if (!std::isfinite(rgba[i])) return no("a clear colour that is not a finite number");
    }
    uint32_t lastTx = (t.width - 1) >> 4, lastTy = (t.height - 1) >> 4;
    uint32_t maxTx = o.maxTx == ~0u ? lastTx : o.maxTx, maxTy = o.maxTy == ~0u ? lastTy : o.maxTy;
    if (o.minTx > maxTx || o.minTy > maxTy || maxTx > lastTx || maxTy > lastTy)
        return no("tiles outside the frame's (a TILE_RANGE_FAULT: emit_fragment_job clamps them)");

    std::unique_ptr<ClearPass> pass(new ClearPass(device));
    pass->target_ = t;
    pass->cmd_ = device.gpu().newBo(4096, why);
    if (!pass->cmd_) return nullptr;
    if (o.withTiler) {
        pass->heap_ = device.tilerHeap(why);
        if (pass->heap_ == nullptr) return nullptr;
    }
    uint8_t* cpu = pass->cmd_->cpu();
    uint64_t gpu = pass->cmd_->gpu();
    memset(cpu, 0, 0x200);

    uint64_t tiler = 0;
    if (o.withTiler) {
        hm_v7_tiler_heap(cpu + kHeapDesc, pass->heap_->gpu(), static_cast<uint32_t>(pass->heap_->size()));
        hm_v7_tiler_context(cpu + kTilerCtx, gpu + kHeapDesc, t.width, t.height,
                            hm_v7_hierarchy_mask(t.width, t.height, device.tilerLevels()));
        tiler = gpu + kTilerCtx;
    }

    struct hm_v7_fb fb = {};
    fb.width = t.width;
    fb.height = t.height;
    fb.bound_min_x = o.boundMinX;
    fb.bound_min_y = o.boundMinY;
    fb.bound_max_x = o.boundMaxX == ~0u ? t.width - 1 : o.boundMaxX;
    fb.bound_max_y = o.boundMaxY == ~0u ? t.height - 1 : o.boundMaxY;
    fb.sample_locations = device.samplePositions().gpu();
    fb.tiler = tiler;
    hm_v7_tile_size(4, device.tileBufferBudget(), &fb.tile_size, &fb.cbuf_allocation);
    fb.rt_count = 1;
    struct hm_v7_rt rt = {};
    rt.format = t.format == Format::BGRA8Unorm ? HM_RT_BGRA8_UNORM : HM_RT_RGBA8_UNORM;
    rt.base = base;
    rt.row_stride = t.rowBytes;
    rt.internal_buffer_offset = 0;
    rt.write_enable = true;
    rt.clear = true;
    rt.clean_pixel_write = o.cleanPixelWrite;
    rt.dithering = true;        // pan_prepare_rt: "cfg->dithering_enable = true" for every written target
    hm_v7_pack_clear_rgba8(rgba, rt.clear_words);
    const char* refused = nullptr;
    uint64_t fbd = hm_v7_framebuffer(cpu + kFbd, gpu + kFbd, &fb, &rt, &refused);
    if (fbd == 0) return no(refused);
    hm_v7_fragment_job(cpu + kJob, 1, 0, fbd, o.minTx, o.minTy, maxTx, maxTy);

    uint32_t w = rt.clear_words[0];
    uint8_t r = w, g = w >> 8, b = w >> 16, a = w >> 24;
    if (t.format == Format::BGRA8Unorm) {
        pass->expected_[0] = b, pass->expected_[1] = g, pass->expected_[2] = r, pass->expected_[3] = a;
    } else {
        pass->expected_[0] = r, pass->expected_[1] = g, pass->expected_[2] = b, pass->expected_[3] = a;
    }
    return pass;
}

int ClearPass::submit(Submission* out) {
    std::vector<const Bo*> uses = {cmd_.get(), target_.bo, &device_.samplePositions()};
    if (heap_ != nullptr) {
        uses.push_back(heap_);
    }
    return device_.gpu().submit(jobAddress(), true, uses, out);
}

uint32_t ClearPass::status() const {
    struct hm_v7_job h;
    hm_v7_read_job(cmd_->cpu() + kJob, &h, nullptr);
    return h.exception_status;
}

void ClearPass::dump(FILE* fp) const {
    fprintf(fp, "Fragment job at %#llx:\n", static_cast<unsigned long long>(jobAddress()));
    hm_v7_print_job(fp, cmd_->cpu() + kJob);
    fprintf(fp, "Framebuffer at %#llx:\n", static_cast<unsigned long long>(cmd_->gpu() + kFbd));
    hm_v7_print_framebuffer(fp, cmd_->cpu() + kFbd, 1);
}

std::unique_ptr<TrianglePass> TrianglePass::make(Device& device, const Target& t, const float positions[12],
                                                 const float clear[4], std::string* why) {
    auto no = [why](const std::string& s) {
        if (why) *why = s;
        return std::unique_ptr<TrianglePass>();
    };
    // the shaders' viewport is 64x64 (tools/bishaders.c)
    if (t.bo == nullptr || t.width != 64 || t.height != 64 || t.format != Format::RGBA8Unorm)
        return no("the first triangle draws into a 64x64 RGBA8Unorm target only (the shaders' viewport)");
    uint64_t base = t.bo->gpu() + t.offset;
    if ((base & 63) != 0 || (t.rowBytes & 63) != 0 || t.rowBytes < 256 ||
        t.offset + static_cast<uint64_t>(t.rowBytes) * 63 + 256 > t.bo->size())
        return no("the target is not 64-byte aligned rows inside its buffer (pan_layout.c, v7)");
    for (int i = 0; i < 12; i++) {
        if (!std::isfinite(positions[i])) return no("a vertex position that is not a finite number");
    }
    static_assert(sizeof(hm_tri_vs_code) <= kFsCode - kVsCode, "tri_vs does not fit");
    static_assert(sizeof(hm_tri_fs_code) <= 0x1000 - kFsCode - 128, "tri_fs does not fit (with room for prefetch)");

    std::unique_ptr<TrianglePass> pass(new TrianglePass(device));
    pass->target_ = t;
    pass->cmd_ = device.gpu().newBo(4096, why);
    if (!pass->cmd_) return nullptr;
    pass->heap_ = device.tilerHeap(why);
    if (pass->heap_ == nullptr) return nullptr;
    uint8_t* cpu = pass->cmd_->cpu();
    uint64_t gpu = pass->cmd_->gpu();
    memset(cpu, 0, 0x1000);

    // the shaders and their Renderer State, the shader's address merged in as Panfrost merges a partial RSD
    memcpy(cpu + kVsCode, hm_tri_vs_code, sizeof(hm_tri_vs_code));
    memcpy(cpu + kFsCode, hm_tri_fs_code, sizeof(hm_tri_fs_code));
    uint32_t rsd[16];
    memcpy(rsd, hm_tri_vs_rsd, sizeof(rsd));
    rsd[0] |= static_cast<uint32_t>(gpu + kVsCode);
    rsd[1] |= static_cast<uint32_t>((gpu + kVsCode) >> 32);
    memcpy(cpu + kVsRsd, rsd, sizeof(rsd));
    memcpy(rsd, hm_tri_fs_rsd, sizeof(rsd));
    rsd[0] |= static_cast<uint32_t>(gpu + kFsCode);
    rsd[1] |= static_cast<uint32_t>((gpu + kFsCode) >> 32);
    memcpy(cpu + kFsRsd, rsd, sizeof(rsd));
    memcpy(cpu + kFsRsd + hm_v7_sizes()->renderer_state, hm_tri_fs_blend, sizeof(hm_tri_fs_blend));

    // the vertices: attribute 0 from buffer 0, four floats a vertex (panfrost_emit_vertex_data), and the table's end
    memcpy(cpu + kVertices, positions, 12 * sizeof(float));
    hm_v7_attribute(cpu + kAttribute, 0, hm_tri_attribute_format, 0, true);
    hm_v7_attribute_buffer(cpu + kAttributeBuffers, gpu + kVertices, 16, 48);
    hm_v7_attribute_buffer(cpu + kAttributeBuffers + 16, 0, 0, 0);
    // the varyings (panfrost_emit_varying_descriptor): buffers GENERAL (empty: no varying but the position) and
    // POSITION, four floats a vertex, then the table's end; the vertex shader's one output, gl_Position, a record
    // of the POSITION buffer (pan_varying_index: 1) in the special format; the fragment shader reads none
    hm_v7_attribute(cpu + kVaryings, 1, hm_v7_position_format(), 0, false);
    hm_v7_attribute_buffer(cpu + kVaryingBuffers, 0, 0, 0);
    hm_v7_attribute_buffer(cpu + kVaryingBuffers + 16, gpu + kPositions, 16, 48);
    hm_v7_attribute_buffer(cpu + kVaryingBuffers + 32, 0, 0, 0);
    hm_v7_viewport(cpu + kViewport, 0, 0, 63, 63, 0.0f, 1.0f);
    hm_v7_local_storage(cpu + kLocalStorage);

    // the Tiler Context and heap (jm_emit_tiler_desc)
    hm_v7_tiler_heap(cpu + kHeapDesc, pass->heap_->gpu(), static_cast<uint32_t>(pass->heap_->size()));
    hm_v7_tiler_context(cpu + kTilerCtx, gpu + kHeapDesc, 64, 64, hm_v7_hierarchy_mask(64, 64, device.tilerLevels()));

    struct hm_v7_draw vertex = {};
    vertex.state = gpu + kVsRsd;
    vertex.attributes = gpu + kAttribute;
    vertex.attribute_buffers = gpu + kAttributeBuffers;
    vertex.varyings = gpu + kVaryings;
    vertex.varying_buffers = gpu + kVaryingBuffers;
    vertex.thread_storage = gpu + kLocalStorage;
    hm_v7_vertex_job(cpu + kVertexJob, 1, gpu + kTilerJob, 3, &vertex);
    struct hm_v7_draw fragment = {};
    fragment.state = gpu + kFsRsd;
    fragment.position = gpu + kPositions;
    fragment.viewport = gpu + kViewport;
    fragment.thread_storage = gpu + kLocalStorage;
    hm_v7_tiler_job(cpu + kTilerJob, 2, 1, 0, 3, gpu + kTilerCtx, &fragment);

    // the frame: cleared, then the draw, then written back (GENX(pan_emit_fbd) with the Tiler Context)
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
    hm_v7_pack_clear_rgba8(clear, rt.clear_words);
    const char* refused = nullptr;
    uint64_t fbd = hm_v7_framebuffer(cpu + kFbd, gpu + kFbd, &fb, &rt, &refused);
    if (fbd == 0) return no(refused);
    hm_v7_fragment_job(cpu + kFragmentJob, 1, 0, fbd, 0, 0, 3, 3);
    return pass;
}

int TrianglePass::submit(Submission* draw, Submission* frame) {
    std::vector<const Bo*> uses = {cmd_.get(), target_.bo, &device_.samplePositions(), heap_};
    int error = device_.gpu().submit(cmd_->gpu() + kVertexJob, false, uses, draw);
    if (error == 0) {
        error = device_.gpu().submit(cmd_->gpu() + kFragmentJob, true, uses, frame);
    }
    return error;
}

static uint32_t statusAt(const uint8_t* job) {
    struct hm_v7_job h;
    hm_v7_read_job(job, &h, nullptr);
    return h.exception_status;
}
uint32_t TrianglePass::vertexStatus() const { return statusAt(cmd_->cpu() + kVertexJob); }
uint32_t TrianglePass::tilerStatus() const { return statusAt(cmd_->cpu() + kTilerJob); }
uint32_t TrianglePass::fragmentStatus() const { return statusAt(cmd_->cpu() + kFragmentJob); }

void TrianglePass::dump(FILE* fp) const {
    fprintf(fp, "Vertex job at %#llx:\n", static_cast<unsigned long long>(cmd_->gpu() + kVertexJob));
    hm_v7_print_job(fp, cmd_->cpu() + kVertexJob);
    fprintf(fp, "Tiler job at %#llx:\n", static_cast<unsigned long long>(cmd_->gpu() + kTilerJob));
    hm_v7_print_job(fp, cmd_->cpu() + kTilerJob);
    fprintf(fp, "Fragment job at %#llx:\n", static_cast<unsigned long long>(cmd_->gpu() + kFragmentJob));
    hm_v7_print_job(fp, cmd_->cpu() + kFragmentJob);
    hm_v7_print_framebuffer(fp, cmd_->cpu() + kFbd, 1);
    const float* p = reinterpret_cast<const float*>(cmd_->cpu() + kPositions);
    fprintf(fp, "positions the vertex job wrote: (%g %g %g %g) (%g %g %g %g) (%g %g %g %g)\n", p[0], p[1], p[2], p[3],
            p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]);
}

std::unique_ptr<WriteValue> WriteValue::make(Device& device, const Bo& target, uint64_t offset, uint32_t value,
                                             std::string* why) {
    if ((offset & 3) != 0 || offset + 4 > target.size()) {
        if (why) *why = "a 32-bit write at byte " + std::to_string(offset) + " of a buffer of " +
                        std::to_string(target.size());
        return nullptr;
    }
    std::unique_ptr<WriteValue> job(new WriteValue(device));
    job->target_ = &target;
    job->cmd_ = device.gpu().newBo(4096, why);
    if (!job->cmd_) return nullptr;
    memset(job->cmd_->cpu(), 0, hm_v7_sizes()->write_value_job);
    hm_v7_write_value_job(job->cmd_->cpu(), 1, 0, target.gpu() + offset, value);
    return job;
}

int WriteValue::submit(Submission* out) {
    return device_.gpu().submit(cmd_->gpu(), false, {cmd_.get(), target_}, out);
}

uint32_t WriteValue::status() const {
    struct hm_v7_job h;
    hm_v7_read_job(cmd_->cpu(), &h, nullptr);
    return h.exception_status;
}

void WriteValue::dump(FILE* fp) const {
    fprintf(fp, "Write Value job at %#llx:\n", static_cast<unsigned long long>(cmd_->gpu()));
    hm_v7_print_job(fp, cmd_->cpu());
}

}  // namespace hakios::mali
