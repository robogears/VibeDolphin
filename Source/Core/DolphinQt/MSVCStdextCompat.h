// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Compatibility shim for building the bundled Qt 6.5.1 with the MSVC v14.5x
// toolset (Visual Studio 2026, _MSC_VER 19.5x) or newer.
//
// That STL removed the non-Standard stdext checked/unchecked array iterators
// (formerly warning STL4043). Qt 6.5.1's qcompilerdetection.h unconditionally
// defines, on MSVC:
//     QT_MAKE_CHECKED_ARRAY_ITERATOR(x, N) -> stdext::make_checked_array_iterator(x, size_t(N))
//     QT_MAKE_UNCHECKED_ARRAY_ITERATOR(x)  -> stdext::make_unchecked_array_iterator(x)
// and uses them (e.g. qvarlengtharray.h, qkeysequence_p.h). With the iterators
// gone these fail to compile. For contiguous storage a raw pointer is itself a
// valid (output) iterator, so we reprovide the factory functions to just return
// the pointer.
//
// This header is force-included via pch_qt.h ahead of every Qt header, so the
// names are visible at the point of macro expansion. It is a no-op on non-MSVC.
//
// Remove this once the bundled Qt is updated to a version that no longer uses
// stdext (Qt >= 6.6.1) or the project is built with the v143 toolset.

#if defined(_MSC_VER)

#include <cstddef>

namespace stdext
{
template <typename T>
constexpr T* make_checked_array_iterator(T* ptr, std::size_t /*size*/, std::size_t index = 0)
{
  return ptr + index;
}

template <typename T>
constexpr T* make_unchecked_array_iterator(T* ptr)
{
  return ptr;
}
}  // namespace stdext

#endif
