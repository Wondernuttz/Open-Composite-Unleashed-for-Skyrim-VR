//
// Like xrutil.h but not included in every file by default
//
// Created by ZNix on 15/03/2021.
//

#pragma once

#include "generated/interfaces/vrtypes.h"
#include <Drivers/Backend.h>
#include <glm/mat4x4.hpp>
#include <openxr/openxr.h>
#include <optional>

namespace xr_utils {

void PoseFromSpace(vr::TrackedDevicePose_t* pose, XrSpace space, vr::ETrackingUniverseOrigin origin,
    std::optional<glm::mat4> extraTransform = {}, int device = 2);

// Controller filters are scoped to one tracking/reference-space epoch.
// Clear them before an XrSession replacement so old coordinates cannot bleed
// into the replacement session's controller and in-game hand poses.
void ResetControllerPoseFilters();

}
