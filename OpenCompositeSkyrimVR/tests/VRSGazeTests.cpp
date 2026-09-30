#include "OpenOVR/Compositor/VRSGaze.h"
#include "OpenOVR/Compositor/VRSPattern.h"
#include "OpenOVR/Misc/EyeGaze.h"
#include "OpenOVR/Misc/FoveationProfiles.h"
#include "OpenOVR/Misc/FoveationRates.h"

#include <cmath>
#include <cstdio>

static int failures = 0;

static void Check(bool condition, const char* message)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

static bool Near(float a, float b, float tolerance = 0.0001f)
{
	return std::fabs(a - b) <= tolerance;
}

static void CheckGeometryCoverage()
{
	using namespace ocu_vrs_gaze;
	constexpr float radians = 3.14159265358979323846f / 180.0f;
	unsigned samples = 0;
	// Synthetic geometry coverage, not measurements of individual headsets.
	// Parallel projection and driver-adjustable cant must use runtime geometry;
	// asymmetric/wide FOV must not assume that straight ahead is texture center.
	const float fovs[][4] = {{-1.1f, 1.1f, 1.0f, -1.0f},
	    {-2.4f, 1.1f, 1.3f, -0.9f}, {-1.1f, 2.4f, 1.3f, -0.9f}};
	for (float cant : {0.0f, 2.0f, 10.0f, 15.0f}) {
		for (float side : {-1.0f, 1.0f}) {
			const float eyeYaw = side * cant * radians;
			for (float yawDegrees : {-40.0f, -20.0f, 0.0f, 20.0f, 40.0f}) {
				for (float pitchDegrees : {-25.0f, 0.0f, 25.0f}) {
					const float yaw = yawDegrees * radians, pitch = pitchDegrees * radians;
					const float x = std::sin(yaw) * std::cos(pitch);
					const float y = std::sin(pitch);
					const float z = -std::cos(yaw) * std::cos(pitch);
					for (const auto& fov : fovs) {
						Center projected{};
						Check(ProjectViewSpace(x, y, z, 0.0f, -std::sin(eyeYaw / 2.0f),
						    0.0f, std::cos(eyeYaw / 2.0f), fov[0], fov[1], fov[2], fov[3], projected),
						    "parallel/canted/asymmetric geometry accepts valid gaze");
						// Independent angular reference rather than repeating quaternion math.
						const float expectedX = std::clamp((std::tan(yaw - eyeYaw) - fov[0]) /
						    (fov[1] - fov[0]), 0.02f, 0.98f);
						const float expectedY = std::clamp((fov[2] - std::tan(pitch) /
						    std::cos(yaw - eyeYaw)) / (fov[2] - fov[3]), 0.02f, 0.98f);
						Check(Near(projected.x, expectedX) && Near(projected.y, expectedY),
						    "both-eye projection matches angular reference throughout gaze range");
						++samples;
					}
				}
			}
		}
	}
	for (float hz : {60.0f, 72.0f, 90.0f, 120.0f, 144.0f}) {
		bool hasPrevious = false;
		Center smoothed{};
		for (bool valid : {false, false, true, true, false, true}) {
			const auto mode = SelectMode(true, false, valid, false);
			if (mode != Mode::EyeTracked) {
				hasPrevious = false;
				continue;
			}
			const Center target{0.8f, 0.25f};
			smoothed = Smooth(smoothed, target, 1.0f / hz, hasPrevious);
			if (!hasPrevious)
				Check(Near(smoothed.x, target.x) && Near(smoothed.y, target.y),
				    "gaze resumes immediately after startup/tracking loss at every refresh rate");
			hasPrevious = true;
		}
	}
	std::printf("Geometry matrix: %u projections; startup/loss recovery at 60/72/90/120/144 Hz checked\n", samples);
}

static void CheckRateCapProfiles()
{
	using namespace ocu_foveation;
	auto area = [](Rate rate) { const auto d = Dimensions(rate); return d.x * d.y; };
	unsigned profiles = 0;
	for (bool horizontal : {false, true}) {
		const Rate half = horizontal ? Rate::X2x1 : Rate::X1x2;
		for (bool tracked : {false, true}) {
			Check(ResolveRates(tracked, false, true, horizontal, {}) ==
			    RingRates{Rate::X1x1, half, half}, "default cap makes middle and outer equally half-rate");
		}
		for (unsigned inner = 0; inner < 7; ++inner)
		for (unsigned mid = 0; mid < 7; ++mid)
		for (unsigned outer = 0; outer < 7; ++outer) {
			const RingRates requested{static_cast<Rate>(inner), static_cast<Rate>(mid), static_cast<Rate>(outer)};
			const auto capped = ResolveRates(true, true, true, horizontal, requested);
			Check(area(capped.inner) <= 2 && area(capped.mid) <= 2 && area(capped.outer) <= 2,
			    "effective custom cap covers center, middle and outer for every combination");
			Check(area(capped.inner) <= area(requested.inner) && area(capped.mid) <= area(requested.mid) &&
			    area(capped.outer) <= area(requested.outer), "cap never reduces any ring's requested density");
			if (area(requested.inner) <= area(requested.mid) && area(requested.mid) <= area(requested.outer))
				Check(area(capped.inner) <= area(capped.mid) && area(capped.mid) <= area(capped.outer),
				    "cap cannot reverse a progressively coarser requested profile");
			Check(ResolveRates(true, true, true, horizontal, capped) == capped, "applying cap twice does not alter axes or density");
			Check(ResolveRates(true, true, false, horizontal, requested) == requested,
			    "uncapped custom combinations preserve intentional ring choices");
			++profiles;
		}
		const auto intentionallyFinerOuter = ResolveRates(true, true, true, horizontal,
		    {Rate::X1x1, Rate::X2x2, Rate::X1x1});
		Check(intentionallyFinerOuter == RingRates{Rate::X1x1, half, Rate::X1x1},
		    "cap preserves an explicitly full-rate outer ring rather than silently coarsening it");
		const auto axisChange = ResolveRates(true, true, true, horizontal,
		    {Rate::X1x1, Rate::X2x4, Rate::X4x2});
		Check(axisChange == RingRates{Rate::X1x1, Rate::X1x2, Rate::X2x1},
		    "different custom axes may remain different despite equal capped density");
	}
	std::printf("Rate cap matrix: %u custom profiles; every ring capped; ordered densities never reversed\n", profiles);
}

int main()
{
	using namespace ocu_vrs_gaze;
	Center center{};
	CheckGeometryCoverage();
	CheckRateCapProfiles();

	const auto eyeDefault = ocu_foveation::Resolve(true, -1, -1, -1, -1, -1, -1);
	const auto fixedDefault = ocu_foveation::Resolve(false, -1, -1, -1, -1, -1, -1);
	Check(Near(eyeDefault.inner, 0.2f) && Near(eyeDefault.mid, 0.4f) &&
	    Near(fixedDefault.inner, 0.7f) && Near(fixedDefault.mid, 0.85f),
	    "new installations use Normal gaze radii and preserve fixed defaults");
	for (bool tracked : {false, true}) {
	    const auto legacy = ocu_foveation::Resolve(tracked, 0.63f, 0.83f, -1, -1, -1, -1);
	    Check(Near(legacy.inner, 0.63f) && Near(legacy.mid, 0.83f),
	        "legacy explicit sizes are preserved for both modes");
	}
	for (bool gazeValid : {false, true}) {
	    const auto selectedMode = SelectMode(true, true, gazeValid, false);
	    const auto selected = ocu_foveation::Resolve(selectedMode == Mode::EyeTracked,
	        0.63f, 0.83f, 0.75f, 0.9f, 0.45f, 0.65f);
	    Check(Near(selected.inner, gazeValid ? 0.45f : 0.75f),
	        "valid gaze selects smaller explicit profile; gaze loss selects explicit fixed profile");
	}
	const auto repaired = ocu_foveation::Resolve(true, -1, -1, -1, -1, 4.0f, 0.2f);
	Check(Near(repaired.inner, 1.0f) && Near(repaired.mid, 1.0f),
	    "profile bounds and ring ordering are sanitized");

	Check(ocu_eye_gaze::IsSampleTimeUsable(1000000000, 0),
	    "a valid pose with runtime sample time unavailable is accepted");
	Check(ocu_eye_gaze::IsSampleTimeUsable(1000000000, 850000000),
	    "a runtime-clamped gaze sample is accepted");
	Check(ocu_eye_gaze::IsSampleTimeUsable(1000000000, 100000000),
	    "sample time metadata does not invalidate an otherwise valid pose");
	Check(ocu_eye_gaze::IsSampleTimeUsable(1000000000, 1050000000),
	    "a runtime-predicted gaze sample is accepted");
	Check(!ocu_eye_gaze::IsSampleTimeUsable(0, 0),
	    "an invalid requested display time is rejected");

	Check(SelectMode(true, false, false, false) == Mode::Off,
	    "Auto without valid gaze stays off instead of falling back to Fixed");
	Check(SelectMode(true, false, true, false) == Mode::EyeTracked,
	    "Auto with valid gaze selects eye tracking");
	Check(SelectMode(false, true, false, false) == Mode::Fixed,
	    "explicit Fixed works without eye tracking");
	Check(SelectMode(true, true, false, false) == Mode::Fixed,
	    "explicit Fixed is the fallback only when both choices are enabled");
	Check(SelectMode(true, true, true, false) == Mode::EyeTracked,
	    "eye tracking takes priority when both choices are enabled");

	using ocu_vrs_pattern::Level;
	using ocu_vrs_pattern::SelectLevel;
	Check(ocu_vrs_pattern::TileCount(8448, 16) == 528 &&
	        ocu_vrs_pattern::TileCount(4608, 16) == 288,
	    "Galaxy XR stereo target produces the required 528x288 VRS atlas");
	Check(ocu_vrs_pattern::TileCount(8449, 16) == 529,
	    "partial edge tiles are rounded up instead of left uncovered");
	float eyeU = 0.0f;
	float eyeV = 0.0f;
	Check(ocu_vrs_pattern::NormalizeInEyeRegion(2112.0f, 2304.0f,
	          0, 0, 4224, 4608, eyeU, eyeV) && Near(eyeU, 0.5f) && Near(eyeV, 0.5f),
	    "left atlas half maps to left-eye normalized coordinates");
	Check(ocu_vrs_pattern::NormalizeInEyeRegion(6336.0f, 2304.0f,
	          4224, 0, 4224, 4608, eyeU, eyeV) && Near(eyeU, 0.5f) && Near(eyeV, 0.5f),
	    "right atlas half maps independently to right-eye normalized coordinates");
	Check(!ocu_vrs_pattern::NormalizeInEyeRegion(5000.0f, 2304.0f,
	          0, 0, 4224, 4608, eyeU, eyeV),
	    "a right-eye atlas pixel cannot contaminate the left-eye pattern");
	Check(SelectLevel(0.50f, 0.60f, 0.80f, true) == Level::Full,
	    "compatibility pattern keeps the fovea full-rate");
	Check(SelectLevel(0.70f, 0.60f, 0.80f, true) == Level::Half,
	    "compatibility pattern uses half-rate outside the fovea");
	Check(SelectLevel(1.20f, 0.60f, 0.80f, true) == Level::Half,
	    "compatibility pattern never reaches 2x2");
	Check(SelectLevel(0.70f, 0.60f, 0.80f, false) == Level::Half,
	    "performance pattern retains its middle half-rate ring");
	Check(SelectLevel(0.90f, 0.60f, 0.80f, false) == Level::Quarter,
	    "performance pattern retains opt-in 2x2 shading");
	Check(SelectMode(true, true, true, true) == Mode::Off,
	    "menus force VRS off");

	Check(Project(0.0f, 0.0f, -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, center),
	    "forward gaze projects");
	Check(Near(center.x, 0.5f) && Near(center.y, 0.5f),
	    "forward gaze maps to optical center");

	Check(Project(0.5f, 0.25f, -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, center),
	    "offset gaze projects");
	Check(Near(center.x, 0.75f) && Near(center.y, 0.375f),
	    "OpenXR +X/+Y maps right/up in texture coordinates");

	Check(!Project(0.0f, 0.0f, 0.1f, -1.0f, 1.0f, 1.0f, -1.0f, center),
	    "backward gaze rejected");
	Check(!Project(NAN, 0.0f, -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, center),
	    "non-finite gaze rejected");

	Direction eyeLocal{};
	Check(ToEyeLocal(0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, eyeLocal),
	    "parallel eye orientation transforms");
	Check(Near(eyeLocal.x, 0.0f) && Near(eyeLocal.y, 0.0f) && Near(eyeLocal.z, -1.0f),
	    "parallel eye orientation leaves shared gaze unchanged");

	const float cantHalfAngle = 5.0f * 3.14159265358979323846f / 180.0f;
	const float cantSin = std::sin(cantHalfAngle);
	const float cantCos = std::cos(cantHalfAngle);
	Center leftCanted{};
	Center rightCanted{};
	Direction leftLocal{};
	Direction rightLocal{};
	Check(ProjectViewSpace(0.0f, 0.0f, -1.0f,
	          0.0f, cantSin, 0.0f, cantCos,
	          -1.0f, 1.0f, 1.0f, -1.0f, leftCanted, &leftLocal),
	    "left canted eye projects shared forward gaze");
	Check(ProjectViewSpace(0.0f, 0.0f, -1.0f,
	          0.0f, -cantSin, 0.0f, cantCos,
	          -1.0f, 1.0f, 1.0f, -1.0f, rightCanted, &rightLocal),
	    "right canted eye projects shared forward gaze");
	Check(leftLocal.x > 0.0f && rightLocal.x < 0.0f,
	    "inverse per-eye cant moves the shared gaze in opposite local directions");
	Check(leftCanted.x > 0.5f && rightCanted.x < 0.5f,
	    "canted eyes receive distinct per-eye foveation centers");
	Check(!ToEyeLocal(0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, eyeLocal),
	    "degenerate eye orientation rejected");

	Center leftFixation{};
	Center rightFixation{};
	Check(ProjectViewSpacePoint(0.0f, 0.0f, -2.0f,
	          -0.032f, 0.0f, 0.0f, 0.0f, cantSin, 0.0f, cantCos,
	          -1.0f, 1.0f, 1.0f, -1.0f, leftFixation),
	    "VIEW-space fixation point projects through the left eye pose");
	Check(ProjectViewSpacePoint(0.0f, 0.0f, -2.0f,
	          0.032f, 0.0f, 0.0f, 0.0f, -cantSin, 0.0f, cantCos,
	          -1.0f, 1.0f, 1.0f, -1.0f, rightFixation),
	    "VIEW-space fixation point projects through the right eye pose");
	Check(leftFixation.x > 0.5f && rightFixation.x < 0.5f,
	    "fixation projection includes both eye cant and IPD");
	Check(!ProjectViewSpacePoint(NAN, 0.0f, -2.0f,
	          0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
	          -1.0f, 1.0f, 1.0f, -1.0f, center),
	    "non-finite fixation point rejected");

	Center target{ 0.8f, 0.2f };
	Center first = Smooth({}, target, 1.0f / 90.0f, false);
	Check(Near(first.x, target.x) && Near(first.y, target.y),
	    "first sample is not delayed");
	Center next = Smooth({ 0.5f, 0.5f }, target, 1.0f / 90.0f, true);
	Check(Near(next.x, target.x) && Near(next.y, target.y), "saccade reaches the new fixation in one frame");
	for (float hz : {15.0f, 30.0f, 45.0f, 60.0f, 90.0f, 120.0f, 144.0f}) {
		Center prior{0.5f, 0.5f};
		for (int i = 1; i <= 100; ++i) {
			const Center pursuit{0.5f + 0.2f * std::sin(i * .03f), 0.5f + 0.1f * std::cos(i * .03f)};
			prior = Smooth(prior, pursuit, 1.0f / hz, true);
			Check(std::hypot(prior.x - pursuit.x, prior.y - pursuit.y) <= .00201f,
			    "pursuit stays within the lag budget at variable real-frame rates");
		}
	}
	const Center noisy = Smooth({.5f, .5f}, {.501f, .5f}, 1.0f / 90, true);
	Check(noisy.x > .5f && noisy.x < .501f, "small tracker noise is still filtered");

	{
		using namespace ocu_foveation;
		for (unsigned r = 0; r < 7; ++r) {
			const auto rate = static_cast<Rate>(r);
			Check(ParseRate(RateName(rate)) == rate, "rate name roundtrip");
			for (bool horizontal : {false, true}) {
				const auto size = Dimensions(CapHalf(rate, horizontal));
				Check(size.x * size.y <= 2, "compatibility caps all rates to half density");
				const RingRates requested{rate, rate, rate};
				Check(ResolveRates(true, true, false, horizontal, requested) == requested,
				    "uncapped eye profile preserves every explicit rate");
				Check(ResolveRates(false, true, false, horizontal, requested) ==
				    ResolveRates(false, false, false, horizontal, {}), "custom rates never leak to fixed fallback");
			}
		}
		Check(ParseRate("8x8") == Rate::X1x1 && ParseRate("") == Rate::X1x1, "invalid rate defaults full detail");
		Check(CapHalf(Rate::X4x2, false) == Rate::X2x1 &&
		    CapHalf(Rate::X2x4, true) == Rate::X1x2, "anisotropic cap preserves requested axis");
	}
	if (failures == 0)
		std::puts("OCU VRS gaze tests PASS");
	return failures == 0 ? 0 : 1;
}
