#pragma once
#include <openxr/openxr.h>

// ABI declarations for the pinned SDK, which predates this ratified extension.
// https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrBindingModificationsKHR.html
// Registered extension 121, structure offset 0: 1000000000 + 120*1000.
#ifndef XR_KHR_binding_modification
#define XR_KHR_binding_modification 1
#define XR_KHR_BINDING_MODIFICATION_EXTENSION_NAME "XR_KHR_binding_modification"
constexpr XrStructureType XR_TYPE_BINDING_MODIFICATIONS_KHR = static_cast<XrStructureType>(1000120000);
struct XrBindingModificationBaseHeaderKHR {
    XrStructureType type;
    const void* next;
};
struct XrBindingModificationsKHR {
    XrStructureType type;
    const void* next;
    uint32_t bindingModificationCount;
    const XrBindingModificationBaseHeaderKHR* const* bindingModifications;
};
#endif
