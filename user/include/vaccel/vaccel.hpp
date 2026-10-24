// SPDX-License-Identifier: MIT
//
// libvaccel: a small C++17 RAII wrapper over the vaccel ioctl uAPI.
//
// Ownership model:
//   Device   owns the file descriptor (shared, so Buffers and Channels can
//            safely outlive the Device object that created them).
//   Buffer   owns one device-memory allocation and its mmap() view.
//   Channel  owns one hardware channel.
// All three are move-only; destructors release the kernel object, so a
// thrown exception never leaks device resources.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "vaccel.h"   // kernel uAPI (include/uapi/vaccel.h)

namespace vaccel {

// Thrown for any failed ioctl/mmap; code() carries the errno.
class Error : public std::system_error {
public:
    Error(int err, const std::string& what)
        : std::system_error(err, std::generic_category(), what) {}
    int err() const noexcept { return code().value(); }
};

namespace detail {
struct Fd {
    int fd = -1;
    explicit Fd(int f) : fd(f) {}
    ~Fd();
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
using FdPtr = std::shared_ptr<const Fd>;
}  // namespace detail

class Buffer {
public:
    Buffer() = default;
    ~Buffer();
    Buffer(Buffer&& o) noexcept;
    Buffer& operator=(Buffer&& o) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    uint32_t handle() const { return handle_; }
    uint64_t size() const { return size_; }          // bytes reserved
    uint64_t dev_addr() const { return dev_addr_; }
    bool mapped() const { return map_ != nullptr; }

    void* data() const { return map_; }
    template <typename T> T* as() const { return static_cast<T*>(map_); }

    // Drop the handle now (the memory stays reserved while still mapped).
    void reset() noexcept;

private:
    friend class Device;
    detail::FdPtr fd_;
    uint32_t handle_ = 0;
    uint64_t size_ = 0;
    uint64_t dev_addr_ = 0;
    void* map_ = nullptr;
};

struct Fence {
    uint32_t chan = 0;
    uint64_t seq = 0;
};

// A batch of commands executed as one job (one fence).
class CommandBatch {
public:
    CommandBatch& fill32(const Buffer& dst, uint64_t off, uint64_t len, uint32_t value);
    CommandBatch& copy(const Buffer& dst, uint64_t dst_off,
                       const Buffer& src, uint64_t src_off, uint64_t len);
    CommandBatch& add32(const Buffer& dst, uint64_t dst_off,
                        const Buffer& a, uint64_t a_off,
                        const Buffer& b, uint64_t b_off, uint64_t len);
    CommandBatch& delay(uint32_t usec);
    CommandBatch& raw(const vaccel_cmd& cmd);   // for tests: unvalidated command

    const std::vector<vaccel_cmd>& cmds() const { return cmds_; }
    size_t size() const { return cmds_.size(); }
    void clear() { cmds_.clear(); }

private:
    std::vector<vaccel_cmd> cmds_;
};

class Channel {
public:
    Channel() = default;
    ~Channel();
    Channel(Channel&& o) noexcept;
    Channel& operator=(Channel&& o) noexcept;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    uint32_t handle() const { return handle_; }
    uint32_t hw_id() const { return hw_id_; }

    // Throws Error(EAGAIN) when the channel queue is full.
    Fence submit(const CommandBatch& batch, bool notify = false);

    // true = signalled, false = timed out. Negative timeout waits forever.
    bool wait(const Fence& f, std::chrono::nanoseconds timeout = std::chrono::nanoseconds(-1));

    void reset() noexcept;

private:
    friend class Device;
    detail::FdPtr fd_;
    uint32_t handle_ = 0;
    uint32_t hw_id_ = 0;
};

class Device {
public:
    explicit Device(const std::string& path = "/dev/vaccel0", bool nonblock = false);

    vaccel_info info() const;
    Buffer alloc(uint64_t bytes, bool map = true) const;
    Channel create_channel() const;

    // Completion events for VACCEL_SUBMIT_NOTIFY jobs.
    bool wait_events(int timeout_ms) const;               // poll() for POLLIN
    std::vector<vaccel_event> read_events(size_t max = 64) const;

    int fd() const { return fd_->fd; }

private:
    detail::FdPtr fd_;
};

const char* op_name(uint32_t op);

}  // namespace vaccel
