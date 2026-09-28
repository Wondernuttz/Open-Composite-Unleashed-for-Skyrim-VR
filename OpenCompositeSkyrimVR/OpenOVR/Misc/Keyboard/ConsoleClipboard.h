#pragma once
#include <Windows.h>
#include <algorithm>
#include <string>
#include <string_view>

namespace ConsoleClipboard {
// Keep the complete command bounded before any character is delivered.
inline const wchar_t* Prepare(std::wstring_view input, size_t capacity, std::wstring& output)
{
    output.clear();
    while (!input.empty() && (input.back() == L'\r' || input.back() == L'\n'))
        input.remove_suffix(1);
    if (input.empty()) return L"Clipboard has no text";
    if (input.size() > capacity) return L"Command too long";
    for (wchar_t ch : input) {
        if (ch == L'\r' || ch == L'\n') return L"Copy one command at a time";
        if ((ch < 32 && ch != L'\t') || ch == 127 ||
            (ch >= 0xD800 && ch <= 0xDFFF) || ch == 0x2028 || ch == 0x2029)
            return L"Unsupported clipboard character";
    }
    output.assign(input);
    std::replace(output.begin(), output.end(), L'\t', L' ');
    return nullptr;
}

inline const wchar_t* Read(HWND owner, size_t capacity, std::wstring& output)
{
    output.clear();
    if (!OpenClipboard(owner)) return L"Clipboard busy - try again";
    struct Close { ~Close() { CloseClipboard(); } } close;
    const HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) return L"Clipboard has no text";
    const SIZE_T bytes = GlobalSize(handle);
    if (bytes < sizeof(wchar_t) || bytes % sizeof(wchar_t)) return L"Invalid clipboard text";
    const auto data = static_cast<const wchar_t*>(GlobalLock(handle));
    if (!data) return L"Cannot read clipboard";
    struct Unlock { HANDLE handle; ~Unlock() { GlobalUnlock(handle); } } unlock{handle};
    // A large clipboard must not allocate or scan unbounded data on the VR thread.
    const size_t bound = (std::min)(size_t(bytes / sizeof(wchar_t)), size_t(4096));
    size_t length = 0;
    while (length < bound && data[length] != L'\0') ++length;
    if (length == bound) return L"Clipboard text too long or invalid";
    return Prepare(std::wstring_view(data, length), capacity, output);
}
}
