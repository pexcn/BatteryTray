#pragma once

#include "win32.h"

#include <utility>

namespace bt {

// Win32 resources differ in invalid value and release function, and their
// releasers have return types unique_ptr deleters cannot swallow cleanly, so a
// tiny owner keyed on a traits type stays shorter than the unique_ptr dance.
template <typename T, typename Traits>
class unique_handle {
public:
    unique_handle() noexcept = default;
    explicit unique_handle(T value) noexcept : value_(value) {}

    unique_handle(unique_handle&& other) noexcept
        : value_(std::exchange(other.value_, Traits::invalid())) {}

    unique_handle& operator=(unique_handle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.value_, Traits::invalid()));
        }
        return *this;
    }

    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;

    ~unique_handle() { reset(); }

    [[nodiscard]] T get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != Traits::invalid(); }

    T release() noexcept { return std::exchange(value_, Traits::invalid()); }

    void reset(T value = Traits::invalid()) noexcept {
        if (value_ != Traits::invalid()) {
            Traits::close(value_);
        }
        value_ = value;
    }

private:
    T value_ = Traits::invalid();
};

struct icon_traits {
    static HICON invalid() noexcept { return nullptr; }
    static void close(HICON value) noexcept { DestroyIcon(value); }
};

template <typename T>
struct gdi_object_traits {
    static T invalid() noexcept { return nullptr; }
    static void close(T value) noexcept { DeleteObject(value); }
};

struct dc_traits {
    static HDC invalid() noexcept { return nullptr; }
    static void close(HDC value) noexcept { DeleteDC(value); }
};

struct regkey_traits {
    static HKEY invalid() noexcept { return nullptr; }
    static void close(HKEY value) noexcept { RegCloseKey(value); }
};

struct kernel_handle_traits {
    static HANDLE invalid() noexcept { return nullptr; }
    static void close(HANDLE value) noexcept { CloseHandle(value); }
};

struct file_traits {
    static HANDLE invalid() noexcept { return INVALID_HANDLE_VALUE; }
    static void close(HANDLE value) noexcept { CloseHandle(value); }
};

// COM interfaces release themselves rather than going through a free function,
// so they need their own owner rather than another traits type.
template <typename T>
class com_ptr {
public:
    com_ptr() noexcept = default;

    com_ptr(com_ptr&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}

    com_ptr& operator=(com_ptr&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    com_ptr(const com_ptr&) = delete;
    com_ptr& operator=(const com_ptr&) = delete;

    ~com_ptr() { reset(); }

    [[nodiscard]] T* get() const noexcept { return value_; }
    T* operator->() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

    // For the out parameter of a creation call, which always writes a fresh
    // reference into an otherwise empty pointer.
    [[nodiscard]] T** put() noexcept {
        reset();
        return &value_;
    }

    [[nodiscard]] void** put_void() noexcept { return reinterpret_cast<void**>(put()); }

    void reset() noexcept {
        if (value_) {
            value_->Release();
            value_ = nullptr;
        }
    }

private:
    T* value_ = nullptr;
};

using unique_icon = unique_handle<HICON, icon_traits>;
using unique_font = unique_handle<HFONT, gdi_object_traits<HFONT>>;
using unique_bitmap = unique_handle<HBITMAP, gdi_object_traits<HBITMAP>>;
using unique_dc = unique_handle<HDC, dc_traits>;
using unique_regkey = unique_handle<HKEY, regkey_traits>;
using unique_kernel_handle = unique_handle<HANDLE, kernel_handle_traits>;
using unique_file = unique_handle<HANDLE, file_traits>;

} // namespace bt
