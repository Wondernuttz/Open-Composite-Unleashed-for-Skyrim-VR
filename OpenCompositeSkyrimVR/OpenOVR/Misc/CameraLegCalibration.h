#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

// Live, controller-driven trim for monocular camera feet. This deliberately
// sits between the camera network stream and the public OCU-NET2/3 poses:
// physical Vive/Tundra trackers never enter this path, and locomotion reads
// can request the untrimmed camera pose so calibration cannot bias gait.
namespace CameraLegCalibration {

// Poll the two physical controllers and update the live calibration state.
// Safe to call more than once per render frame; the implementation rate-limits
// itself to one update per millisecond.
void Tick();

// Apply the saved camera-foot correction. footIndex is 0=left, 1=right.
// alignment rotates camera/body coordinates into the OpenXR playspace.
void Apply(int footIndex, glm::vec3& position, glm::vec3& velocity,
    glm::quat& orientation, const glm::quat& alignment,
    const glm::vec3& bodyRoot, bool bodyRootValid, bool grounded);

// True only while the user is actively editing. The game-facing input paths
// use this to suppress locomotion, turning and action buttons while the raw
// controller state remains available to this calibration tool.
bool CapturesInput();

} // namespace CameraLegCalibration
