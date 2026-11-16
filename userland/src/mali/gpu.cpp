// Buffers, submissions and waits over a Transport (mali.hpp).
#include <cerrno>
#include <cstring>

#include "mali.hpp"

namespace hakios::mali {

const char* exceptionName(uint32_t status) {
    // Linux's drivers/gpu/drm/panfrost/panfrost_device.c, panfrost_exception_infos (the low byte)
    switch (status & 0xff) {
    case 0x00: return "NOT_STARTED";      // (OK: the GPU never wrote the header)
    case 0x01: return "DONE";
    case 0x02: return "INTERRUPTED";
    case 0x03: return "STOPPED";
    case 0x04: return "TERMINATED";
    case 0x08: return "ACTIVE";
    case 0x40: return "JOB_CONFIG_FAULT";
    case 0x41: return "JOB_POWER_FAULT";
    case 0x42: return "JOB_READ_FAULT";
    case 0x43: return "JOB_WRITE_FAULT";
    case 0x44: return "JOB_AFFINITY_FAULT";
    case 0x48: return "JOB_BUS_FAULT";
    case 0x50: return "INSTR_INVALID_PC";
    case 0x51: return "INSTR_INVALID_ENC";
    case 0x52: return "INSTR_TYPE_MISMATCH";
    case 0x53: return "INSTR_OPERAND_FAULT";
    case 0x54: return "INSTR_TLS_FAULT";
    case 0x55: return "INSTR_BARRIER_FAULT";
    case 0x56: return "INSTR_ALIGN_FAULT";
    case 0x58: return "DATA_INVALID_FAULT";
    case 0x59: return "TILE_RANGE_FAULT";
    case 0x5a: return "ADDR_RANGE_FAULT";
    case 0x5b: return "IMPRECISE_FAULT";
    case 0x60: return "OOM";
    case 0x61: return "OOM_AFBC";
    case 0x80: return "DELAYED_BUS_FAULT";
    case 0x88: return "GPU_SHAREABILITY_FAULT";
    case 0x89: return "SYS_SHAREABILITY_FAULT";
    case 0x8a: return "GPU_CACHEABILITY_FAULT";
    default: break;
    }
    if ((status & 0xf8) == 0xc0) return "TRANSLATION_FAULT";
    if ((status & 0xf8) == 0xc8) return "PERMISSION_FAULT";
    if ((status & 0xf8) == 0xd0) return "TRANSTAB_BUS_FAULT";
    if ((status & 0xf8) == 0xd8) return "ACCESS_FLAG_FAULT";
    if ((status & 0xf0) == 0xe0) return "ADDRESS_SIZE_FAULT";
    if ((status & 0xf0) == 0xe8) return "MEMORY_ATTRIBUTES_FAULT";
    return "UNKNOWN";
}

const Model* modelOf(uint64_t productId) {
    // pan_props.c's v7 entries (pan_arch: product id >> 12 == 7)
    static const Model models[] = {
        {0x7090, "Mali-G51", 16384},
        {0x7093, "Mali-G31", 16384},
        {0x7211, "Mali-G76", 16384},
        {0x7212, "Mali-G52", 16384},
        {0x7402, "Mali-G52 r1", 16384},
    };
    for (const Model& m : models) {
        if (m.productId == productId) return &m;
    }
    return nullptr;
}

Bo::~Bo() { owner_.release(*this); }

void Gpu::release(Bo& bo) {
    if (bo.cpu_ != nullptr) {
        t_->unmap(bo.handle_, bo.cpu_, bo.size_);
    }
    DrmGemClose close = {bo.handle_, 0};
    (void)t_->call(kGemClose, &close, sizeof(close));
}

int Gpu::param(uint32_t which, uint64_t* value) {
    PanfrostGetParam p = {which, 0, 0};
    int error = t_->call(kGetParam, &p, sizeof(p));
    if (error == 0) {
        *value = p.value;
    }
    return error;
}

std::unique_ptr<Bo> Gpu::newBo(uint64_t size, std::string* why) {
    if (size == 0 || size > (256ull << 20)) {
        if (why) *why = "a buffer of " + std::to_string(size) + " bytes (HakiosMali takes 1 byte to 256 MB)";
        return nullptr;
    }
    PanfrostCreateBo create = {static_cast<uint32_t>(size), 0, 0, 0, 0};
    int error = t_->call(kCreateBo, &create, sizeof(create));
    if (error != 0) {
        if (why) *why = "CREATE_BO of " + std::to_string(size) + " bytes: " + std::strerror(error);
        return nullptr;
    }
    uint64_t mapped = size;                             // the kernel's pages (round_page_64): it says how many
    PanfrostGetBoOffset offset = {create.handle, 0, 0};
    error = t_->call(kGetBoOffset, &offset, sizeof(offset));
    if (error != 0 || offset.offset != create.offset || offset.offset == 0) {
        if (why) *why = "GET_BO_OFFSET disagrees with CREATE_BO's address";
        DrmGemClose close = {create.handle, 0};
        (void)t_->call(kGemClose, &close, sizeof(close));
        return nullptr;
    }
    PanfrostMmapBo mmap = {create.handle, 0, 0};
    error = t_->call(kMmapBo, &mmap, sizeof(mmap));
    void* cpu = error == 0 ? t_->map(create.handle, &mapped) : nullptr;
    if (cpu == nullptr) {
        if (why) *why = "the buffer cannot be mapped (MMAP_BO " + std::string(std::strerror(error ? error : EFAULT)) + ")";
        DrmGemClose close = {create.handle, 0};
        (void)t_->call(kGemClose, &close, sizeof(close));
        return nullptr;
    }
    return std::unique_ptr<Bo>(new Bo(*this, create.handle, mapped, create.offset, static_cast<uint8_t*>(cpu)));
}

int Gpu::submit(uint64_t jc, bool fragment, const std::vector<const Bo*>& uses, Submission* out) {
    std::vector<uint32_t> handles;
    for (const Bo* bo : uses) {
        handles.push_back(bo->handle());
    }
    // every word written through the write-combined mappings out before the GPU reads them
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
    PanfrostSubmit s = {};
    s.jc = jc;
    s.bo_handles = reinterpret_cast<uint64_t>(handles.data());
    s.bo_handle_count = static_cast<uint32_t>(handles.size());
    s.requirements = fragment ? kJdReqFs : 0;
    int error = t_->call(kSubmit, &s, sizeof(s));
    if (error == 0) {
        if (s.jc == 0) {
            return EIO;                  // HakiosMali gives back the job's number in jc; 0: it was not queued
        }
        out->seq = s.jc;
        out->fragment = fragment;
    }
    return error;
}

std::unique_ptr<Bo> Gpu::surfaceBo(uint32_t surfaceId, HakiosSurface* info, std::string* why) {
    HakiosSurface s = {};
    s.surface = surfaceId;
    int error = t_->call(kHakiosSurface, &s, sizeof(s));
    if (error != 0 || s.handle == 0) {
        if (why) *why = "the surface's memory as a buffer: " + std::string(std::strerror(error ? error : ENOENT));
        return nullptr;
    }
    PanfrostGetBoOffset offset = {s.handle, 0, 0};
    error = t_->call(kGetBoOffset, &offset, sizeof(offset));
    uint64_t mapped = s.size;
    void* cpu = error == 0 ? t_->map(s.handle, &mapped) : nullptr;
    if (cpu == nullptr) {
        if (why) *why = "the surface's buffer cannot be mapped";
        DrmGemClose close = {s.handle, 0};
        (void)t_->call(kGemClose, &close, sizeof(close));
        return nullptr;
    }
    *info = s;
    return std::unique_ptr<Bo>(new Bo(*this, s.handle, mapped, offset.offset, static_cast<uint8_t*>(cpu)));
}

int Gpu::show(const void* pixels, uint32_t width, uint32_t height, uint32_t rowBytes) {
    HakiosShow s = {reinterpret_cast<uint64_t>(pixels), width, height, rowBytes, 0};
    return t_->call(kHakiosShow, &s, sizeof(s));
}

int Gpu::wait(const Submission& s, uint64_t timeoutUs) {
    HakiosWait w = {s.seq, static_cast<int64_t>(timeoutUs * 1000)};
    return t_->call(kHakiosWait, &w, sizeof(w));
}

}  // namespace hakios::mali
