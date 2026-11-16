// The Transport to the kernel's HakiosMali service (Darwin only): every request a struct method call whose selector
// is the request's number, every buffer's memory the client memory of the type that is its handle. Elsewhere there is
// no such service, and openKernelMali says so.
#include "mali.hpp"

#if !defined(__APPLE__)
namespace hakios::mali {
std::unique_ptr<Transport> openKernelMali(unsigned, std::string* why) {
    if (why) *why = "HakiosMali is a Darwin kernel's service";
    return nullptr;
}
}  // namespace hakios::mali
#else
#include <cerrno>
#include <unistd.h>

#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_time.h>

namespace hakios::mali {

namespace {

int errnoOf(kern_return_t kr) {
    switch (kr) {
    case KERN_SUCCESS: return 0;
    case kIOReturnNoMemory: return ENOMEM;
    case kIOReturnNotFound: return ENOENT;
    case kIOReturnBadArgument: return EINVAL;
    case kIOReturnTimeout: return ETIMEDOUT;
    case kIOReturnUnsupported: return ENOTSUP;
    case kIOReturnNotReady: return EAGAIN;
    case kIOReturnVMError: return EFAULT;
    default: return EIO;
    }
}

class KernelMali : public Transport {
public:
    explicit KernelMali(io_connect_t connection) : c_(connection) { mach_timebase_info(&tb_); }
    ~KernelMali() override { IOServiceClose(c_); }

    int call(uint32_t request, void* data, size_t size) override {
        size_t out = size;
        return errnoOf(IOConnectCallStructMethod(c_, request, data, size, data, &out));
    }
    void* map(uint32_t handle, uint64_t* size) override {
        mach_vm_address_t where = 0;
        mach_vm_size_t got = 0;
        if (IOConnectMapMemory64(c_, handle, mach_task_self(), &where, &got, kIOMapAnywhere) != KERN_SUCCESS ||
            got < *size) {
            if (where != 0) {
                IOConnectUnmapMemory64(c_, handle, mach_task_self(), where);
            }
            return nullptr;
        }
        *size = got;
        return reinterpret_cast<void*>(where);
    }
    void unmap(uint32_t handle, void* address, uint64_t) override {
        IOConnectUnmapMemory64(c_, handle, mach_task_self(), reinterpret_cast<mach_vm_address_t>(address));
    }
    uint64_t nowUs() override { return mach_absolute_time() * tb_.numer / tb_.denom / 1000; }

private:
    io_connect_t c_;
    mach_timebase_info_data_t tb_;
};

}  // namespace

std::unique_ptr<Transport> openKernelMali(unsigned waitSeconds, std::string* why) {
    io_service_t service = 0;
    // HakiosMaliStart registers the service once mtk_mali.c's bring-up reaches step 6, on a thread of its own: early
    // in a boot the program may be first
    for (unsigned waited = 0; (service = IOServiceGetMatchingService(0, IOServiceMatching("HakiosMali"))) == 0 &&
                              waited < waitSeconds * 4; waited++) {
        usleep(250000);
    }
    if (service == 0) {
        if (why) *why = "no HakiosMali service after " + std::to_string(waitSeconds) + " s (boot-arg mali=1? the GPU "
                        "up to step 6 of 6?)";
        return nullptr;
    }
    io_connect_t connection = 0;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) {
        if (why) *why = "IOServiceOpen of HakiosMali: " + std::to_string(kr);
        return nullptr;
    }
    return std::unique_ptr<Transport>(new KernelMali(connection));
}

}  // namespace hakios::mali
#endif
