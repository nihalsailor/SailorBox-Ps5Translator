#pragma once

#include "Types.hpp"

namespace Sailor {

constexpr i32 SCE_OK = 0;

// Kernel / Posix errors (Sony 0x80020000 base)
constexpr i32 SCE_KERNEL_ERROR_EPERM          = static_cast<i32>(0x80020001);
constexpr i32 SCE_KERNEL_ERROR_ENOENT         = static_cast<i32>(0x80020002);
constexpr i32 SCE_KERNEL_ERROR_ESRCH          = static_cast<i32>(0x80020003);
constexpr i32 SCE_KERNEL_ERROR_EINTR          = static_cast<i32>(0x80020004);
constexpr i32 SCE_KERNEL_ERROR_EIO            = static_cast<i32>(0x80020005);
constexpr i32 SCE_KERNEL_ERROR_ENXIO          = static_cast<i32>(0x80020006);
constexpr i32 SCE_KERNEL_ERROR_EBADF          = static_cast<i32>(0x80020009);
constexpr i32 SCE_KERNEL_ERROR_ECHILD         = static_cast<i32>(0x8002000A);
constexpr i32 SCE_KERNEL_ERROR_EDEADLK        = static_cast<i32>(0x8002000B);
constexpr i32 SCE_KERNEL_ERROR_ENOMEM         = static_cast<i32>(0x8002000C);
constexpr i32 SCE_KERNEL_ERROR_EACCES         = static_cast<i32>(0x8002000D);
constexpr i32 SCE_KERNEL_ERROR_EFAULT         = static_cast<i32>(0x8002000E);
constexpr i32 SCE_KERNEL_ERROR_EBUSY          = static_cast<i32>(0x80020010);
constexpr i32 SCE_KERNEL_ERROR_EEXIST         = static_cast<i32>(0x80020011);
constexpr i32 SCE_KERNEL_ERROR_EXDEV          = static_cast<i32>(0x80020012);
constexpr i32 SCE_KERNEL_ERROR_ENODEV         = static_cast<i32>(0x80020013);
constexpr i32 SCE_KERNEL_ERROR_ENOTDIR        = static_cast<i32>(0x80020014);
constexpr i32 SCE_KERNEL_ERROR_EISDIR         = static_cast<i32>(0x80020015);
constexpr i32 SCE_KERNEL_ERROR_EINVAL         = static_cast<i32>(0x80020016);
constexpr i32 SCE_KERNEL_ERROR_ENFILE         = static_cast<i32>(0x80020017);
constexpr i32 SCE_KERNEL_ERROR_EMFILE         = static_cast<i32>(0x80020018);
constexpr i32 SCE_KERNEL_ERROR_ENOSPC         = static_cast<i32>(0x8002001C);
constexpr i32 SCE_KERNEL_ERROR_ESPIPE         = static_cast<i32>(0x8002001D);
constexpr i32 SCE_KERNEL_ERROR_EROFS          = static_cast<i32>(0x8002001E);
constexpr i32 SCE_KERNEL_ERROR_EMLINK         = static_cast<i32>(0x8002001F);
constexpr i32 SCE_KERNEL_ERROR_EPIPE          = static_cast<i32>(0x80020020);
constexpr i32 SCE_KERNEL_ERROR_EDOM           = static_cast<i32>(0x80020021);
constexpr i32 SCE_KERNEL_ERROR_ERANGE         = static_cast<i32>(0x80020022);
constexpr i32 SCE_KERNEL_ERROR_EAGAIN         = static_cast<i32>(0x80020023);
constexpr i32 SCE_KERNEL_ERROR_EWOULDBLOCK    = SCE_KERNEL_ERROR_EAGAIN;
constexpr i32 SCE_KERNEL_ERROR_EINPROGRESS    = static_cast<i32>(0x80020024);
constexpr i32 SCE_KERNEL_ERROR_EALREADY       = static_cast<i32>(0x80020025);
constexpr i32 SCE_KERNEL_ERROR_ENOTSOCK       = static_cast<i32>(0x80020026);
constexpr i32 SCE_KERNEL_ERROR_EDESTADDRREQ   = static_cast<i32>(0x80020027);
constexpr i32 SCE_KERNEL_ERROR_EMSGSIZE       = static_cast<i32>(0x80020028);
constexpr i32 SCE_KERNEL_ERROR_ETIMEDOUT      = static_cast<i32>(0x8002003C);

} // namespace Sailor
