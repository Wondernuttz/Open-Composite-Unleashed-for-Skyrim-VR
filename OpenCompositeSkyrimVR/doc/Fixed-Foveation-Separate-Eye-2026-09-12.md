# Fixed foveation with separate upscaled eye outputs

Local test `4.3.7-foveation-geometry-hotfix1`, September 12, 2026.

Two admission checks could prevent foveation even with a valid scene:

- Fixed mode and fixed fallback rejected separate left/right submission
  textures before reaching the existing stereo-depth bridge recovery path.
- RDM required a submitted shared color texture even when the bridge supplied
  a valid stereo scene depth texture. Separate-eye recovery deliberately has
  no shared submitted color, so this also blocked eye-tracked RDM in that path.

Complete submitted eye pairs now reach the existing validated scene-depth
path in fixed, tracked and tracked-to-fixed fallback modes. RDM accepts the
scene depth as its ownership anchor without requiring a shared submitted
color texture. It still requires eligible scene draws and a current depth
ownership guide before applying its mask. One output eye is never treated as
the stereo scene allocation.

Missing eyes, invalid/unavailable bridge depth, menu frames and unsupported
draws retain their existing full-rate behavior. A newly complete pair and
valid bridge can recover at the next real-frame boundary. Shared-atlas fixed
foveation, optical centers, saved radii/rates, DAPA pacing and menu suspension
are unchanged. Existing RDM viewport/layout restrictions still apply.

The geometry fix adds no rendering pass or full-frame image copy. Enabling
RDM on a path that previously skipped it uses RDM's existing mask and resolve
work; net game GPU savings still require measurement.

`Foveation geometry v3` startup/transition messages distinguish an incomplete
eye pair, a shared stereo output, separate outputs with validated scene
depth, and separate outputs without usable scene depth. These describe
geometry availability, not proof that every draw is foveated. Check the RDM
masked/protected draw counters for actual application. Debug rings likewise
visualize the profile, not GPU coverage.

RDM supports 1x1, 1x2, 2x1, 2x2, 2x4, 4x2 and 4x4 custom ring rates. It
preserves complete 2x2 pixel quads; these labels describe sampling density,
not the literal hardware VRS footprint. Custom rates must be enabled and the
eye half-rate cap disabled to use 4x2/4x4 as requested. Protected passes and
partial edge clusters can remain full rate.

The change addresses demonstrated code paths; it does not establish that
these were the only causes of Ni's reported X or Fletch's no-effect report.
Matching tester logs and in-headset retesting remain necessary.
