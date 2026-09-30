// msvc_compat.cpp
// Scalar fallback stubs for MSVC 14.40+ vectorized STL intrinsics.
//
// In MSVC 14.40 (VS 2022 17.10+), several STL helper functions were moved
// exclusively to msvcp140.dll and are no longer in libcpmt.lib for /MT builds.
// CommonLibSSE compiled with newer MSVC still generates external references
// to these symbols. Providing scalar implementations here resolves the link errors.

#include <cstddef>
#include <cstring>
#include <cstdint>

extern "C" {

__declspec(noinline)
std::size_t __std_find_first_of_trivial_pos_1(
    const void* haystack, std::size_t haystackLen,
    const void* needles,  std::size_t needlesLen) noexcept
{
    if (needlesLen == 0 || haystackLen == 0) return haystackLen;
    const auto* h = static_cast<const unsigned char*>(haystack);
    const auto* n = static_cast<const unsigned char*>(needles);
    for (std::size_t i = 0; i < haystackLen; ++i) {
        for (std::size_t j = 0; j < needlesLen; ++j) {
            if (h[i] == n[j]) return i;
        }
    }
    return haystackLen;
}

__declspec(noinline)
std::size_t __std_find_last_of_trivial_pos_1(
    const void* haystack, std::size_t haystackLen,
    const void* needles,  std::size_t needlesLen) noexcept
{
    if (needlesLen == 0 || haystackLen == 0) return haystackLen;
    const auto* h = static_cast<const unsigned char*>(haystack);
    const auto* n = static_cast<const unsigned char*>(needles);
    for (std::size_t i = haystackLen; i-- > 0;) {
        for (std::size_t j = 0; j < needlesLen; ++j) {
            if (h[i] == n[j]) return i;
        }
    }
    return haystackLen;
}

__declspec(noinline)
const void* __std_find_last_trivial_1(
    const void* first, const void* last, std::uint8_t val) noexcept
{
    const auto* f = static_cast<const unsigned char*>(first);
    const auto* l = static_cast<const unsigned char*>(last);
    while (l != f) {
        --l;
        if (*l == val) return l;
    }
    return last;
}

} // extern "C"
