# Eye-ring editor and eye presets

Updated 2026-09-12. The moving-eye presets apply the complete size/rate profile:

| Preset | Center size | Middle boundary | Center rate | Middle rate | Outer rate |
| --- | --- | --- | --- | --- | --- |
| Quality | 0.30 | 0.65 | 1x1 | 1x2 | 2x2 |
| Balanced | 0.22 | 0.45 | 1x1 | 2x2 | 4x2 |
| Performance (default) | 0.20 | 0.40 | 1x1 | 2x2 | 4x2 |
| Aggressive | 0.20 | 0.40 | 1x1 | 2x2 | 4x4 |

Every preset enables custom eye rates and turns the eye half-rate cap off, so
its requested rates take effect. Aggressive replaces the former Normal label.
A preset is recognized only when both boundaries, all three requested rates,
the custom-rate switch and the cap match. Other combinations appear as Custom.
Selecting a preset preserves eye enablement, backend, debug and axis choices;
it does not change fixed foveation or DAPA. These profiles are tuning starting
points, not claims of equal image quality or performance on every headset.

The eye-ring popup previews the profile and its effective rates. Apply copies
the draft back to the Configurator page; Cancel discards popup edits. The
Configurator's Save action persists the settings. Restart the game to load
them. The preview is a schematic of the eye image, not a headset capture or
an FPS estimate. Red debug outlines in the headset remain a separate option.

Video now shows the green eye button and a read-only profile summary. All
moving-eye adjustments live in the popup; fixed fallback controls remain on
Video. Backend and half-rate direction are shared and edited in the popup.

The eye button uses the same rounded, glowing green action-button theme as
Save, with an animated neon eye. `Fixed VRS / fallback` supports both headsets
without eye tracking (for example, Meta Quest 3) and fallback when eye tracking
becomes unavailable. It uses the selected rendering backend; on supported
non-NVIDIA hardware that can be Density Mask. The label does not change the
saved enable switch or automatically alter the user's chosen settings.

The popup uses the supplied PugDragon photo, center-cropped without stretching
inside a square eye-texture preview. The square makes the boundaries circular;
this UI change does not alter headset projection or runtime ring geometry.
There are three quality zones separated by two circular boundaries. A
boundary of 0.20 spans 20% of this square's width (a radius of 10%).

Move the pointer over the photo to position the illustrative gaze. The image
and its sampling grid stay fixed while the quality zones move. The detail
checkbox compares the photo with and without sample-density reduction. This
illustrates the selected effective rates; it is not a VRS/RDM reconstruction
simulation. Disabled eye tracking and the effects-only backend show the photo
at full detail. Original photo data is embedded unchanged, and preview-only
controls do not add runtime settings. No fourth custom ring is added.

Fresh runtime/Configurator profiles and a full reset use Performance. Explicit saved ring sizes,
rates, custom-rate switches and caps remain authoritative. Legacy radius
keys still migrate to both profiles. If an older configuration explicitly
sets `vrsCompatibilityMode` but has no separate eye cap, the moving-eye cap
still inherits that value. A new configuration with neither cap key starts
with the fixed cap on and the eye cap off. The explicit eye cap wins
regardless of key order. Older tuned profiles without an explicit custom-rate
switch retain legacy sampling, including their chosen half-rate axis and RDM
checkerboard arrangement and formerly implicit half-rate cap. Selecting an
eye preset writes the explicit custom-rate switch to opt into its three
specified ring rates.

A saved 0.20/0.40, 1x1 / 2x2 / 4x4 profile stays unchanged and now appears
as Aggressive. The earlier 0.20/0.40, 1x1 / 2x1 / 2x2 profile stays Custom.
Partially specified older profiles also retain their former unspecified
rates. Choosing any preset explicitly applies its complete profile; upgrading
does not replace a saved profile with the new default.

Equivalent Performance default settings (the existing `[vrs]` layout also works):

```ini
vrsEyeInnerRadius=0.20
vrsEyeMidRadius=0.40
vrsEyeCustomRates=true
vrsEyeInnerRate=1x1
vrsEyeMidRate=2x2
vrsEyeOuterRate=4x2
vrsEyeCompatibilityMode=false
```

VRS rates describe hardware shading footprints on eligible NVIDIA draws.
RDM uses the corresponding sampling density while preserving complete pixel
quads; its appearance and cost are not identical. Protected terrain,
foliage, UI and other exact-pixel passes can remain full rate regardless of
the ring preview.

This change does not enable gaze-driven DLSS/FSR crop processing. That is a
separate renderer integration requiring moving-region history validation.
