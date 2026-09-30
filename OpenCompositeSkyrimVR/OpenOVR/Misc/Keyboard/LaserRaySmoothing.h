#pragma once

#include <openxr/openxr.h>

namespace oovr_laser_smoothing {

// Each OCU renderer owns a separate raw-ray stream. Keeping the consumer in
// the key prevents a keyboard/menu handoff from filtering an already-filtered
// ray while still giving both surfaces identical tuning.
enum class Consumer {
	Menu,
	Keyboard,
	KeyboardTarget
};

// Filters a calibrated ray in-place. Returns false for invalid/non-finite
// input. Repeated calls for the same consumer, hand, reference space, and
// OpenXR sample time return the cached result without advancing the filter.
bool Filter(Consumer consumer, int side, XrSpace referenceSpace, XrTime sampleTime,
    XrVector3f& origin, XrVector3f& direction);

// Invalid tracking resets only the affected stream. Session teardown resets
// every stream so coordinates from a destroyed session can never leak into a
// replacement session.
void Reset(Consumer consumer, int side, XrSpace referenceSpace);
void ResetAll();

}
