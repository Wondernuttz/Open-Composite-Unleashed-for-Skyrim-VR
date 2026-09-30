#pragma once

#include <cstdint>

namespace ocu_eye_gaze {

// XR_EXT_eye_gaze_interaction defines zero as "sample time unavailable" and
// explicitly permits clamped, predicted, or interpolated sample times. The
// timestamp is diagnostic metadata, not a validity gate for a valid pose.
inline bool IsSampleTimeUsable(std::int64_t displayTime, std::int64_t sampleTime)
{
	(void)sampleTime;
	return displayTime > 0;
}

} // namespace ocu_eye_gaze
