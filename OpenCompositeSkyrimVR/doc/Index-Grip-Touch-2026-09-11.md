# Index grip touch for HIGGS

Deathbooper reported on September 10 that Index hand animation responded to grip
touch but HIGGS only grabbed after squeezing or using the trigger. HIGGS was set
to `GripInputMethod=2`; enabling VRIK trackpad mode did not help.

OCU created a grip float action and a separate force-based click action, but no
legacy grip-touch action. It never populated the Grip touched bit HIGGS needs.

The fix creates a separate boolean `legacy-left/right-grip-touch` action. Only
the Valve Index profile suggests `input/squeeze/value` for it. Squeeze presses
retain `input/squeeze/force`. OpenXR supplies the device-specific scalar-to-boolean
threshold and hysteresis; OCU does not invent a `/squeeze/touch` component or
reuse a force press as touch.

For the active physical Index profile, the current active touch action populates
OpenVR Grip and Axis2 touched bits. It bypasses peak-hold input smoothing so an
open hand releases immediately. Other controller profiles leave grip touch
unbound. Inactive actions and a switch away from Index cannot keep it touched.
Neither swapped thumbsticks, controller pictures, nor VRIK trackpad options
control this route.

HIGGS already supports `GripInputMethod=0` (Auto: touch on Index), `1` (Press),
and `2` (Touch). Leave the user's choice intact. No HIGGS API or INI rewrite is
needed. Its `AllowGripPressWhileUsingTouchInput` policy still controls whether
the game may receive a squeeze press while HIGGS is holding with touch.

The configurator explains automatic HIGGS touch when an Index grip is selected
in Bindings. The VRIK trackpad tooltip also explains that the two are independent.

## Validation

Release runtime and configurator builds passed. The source-extracted OpenXR mock
compiles current production action creation, legacy binding suggestions,
controller-state translation, and input smoothing. It passed 591 checks across
both hands, 16 combinations of smoothing/stick swap/VRIK/disabled-trackpad options,
touch versus force, immediate release, inactive input, reconnect and profile
changes. Existing input-session recovery/trackpad tests passed as well.

Run: `python tests/IndexGripTouchMock.py --build-dir <scratch directory>`.
This mock supplies already-thresholded runtime samples; it does not validate the
physical Index sensor thresholds or load HIGGS inside Skyrim.

## Headset test

With HIGGS GripInputMethod=0 or 2 and Index controllers, touch the grip to pick up
an object, open the hand to drop it, and repeat for both hands. Test squeeze while
holding with the user's HIGGS press policy. Check normal grip press with method 1,
then repeat touch grabs with input dropout protection enabled and after a
controller reconnect. VRIK trackpad mode should not change touch-grab behavior.

## Evidence

- [Discord report](https://discord.com/channels/1200438688182714439/1480697068455723184/1547814113676234783)
- [VRIK workaround did not help](https://discord.com/channels/1200438688182714439/1480697068455723184/1547826370711658517)
- [OpenXR Index profile, section 6.4.9](https://registry.khronos.org/OpenXR/specs/1.0-khr/html/xrspec.html#_valve_index_controller_profile)
- [OpenXR suggested bindings and boolean conversion, section 11.4](https://registry.khronos.org/OpenXR/specs/1.0-khr/html/xrspec.html#input-suggested-bindings)
- Installed HIGGS `higgs_vr.ini`, lines 503–518, documents its Auto/Press/Touch behavior.
