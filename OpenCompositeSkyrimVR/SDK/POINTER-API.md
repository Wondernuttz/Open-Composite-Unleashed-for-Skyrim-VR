# Shared menu pointer API

`OCUPointerAPI.h` defines two optional exports from the loaded OCU runtime:
`OCU_GetPointerStateV1` and `OCU_UpdatePointerLeaseV1`. They are independent
of the unchanged `OCU_GetEffectFoveationV1` interface. Consumers discover
both exports from the same loaded `openvr_api.dll` or `vrclient_x64.dll`.
They must retain their normal pointer path when these exports are absent.

The ABI uses fixed-width values and copied snapshots. Calls do not wait for
tracking, create graphics resources, run callbacks or transfer allocation
ownership. Invalid versions and buffers leave the output untouched.
Consumers serialize their own calls and use a process-unique nonzero token.

## Consumer sequence

1. While its menu is visible, a consumer submits a zero-hand observer lease
   with zero session/frame IDs, desired tracking origin and aim pitch trim.
2. At the next normal frame tracking update, OCU publishes both physical
   hands in metres in the game's selected OpenVR tracking origin. Each ray
   already includes OCU origin/rotation calibration, the requested local
   pitch trim and OCU ray smoothing. The snapshot states the applied trim.
3. Read one snapshot per application frame. Reject stale QPC data, the wrong
   origin/trim or a changed session epoch. Use the same unmodified ray to
   intersect the menu and position its cursor.
4. Renew the lease against that exact snapshot's session and frame, claiming
   the desired hands. A hand can stay claimed while dragging outside the
   menu. Set its hit flag and hit distance only when it should render a beam.
   The response reports actual ownership after arbitration.
5. Only owned hands may produce menu clicks. Raw trigger state and a
   monotonically increasing transition sequence come from internal physical
   input, bypassing the game-facing mask and legacy frame-count smoothing.
   Inactive or unfocused actions cannot confirm a physical release.
   Consumers must cancel pending
   presses and drags when ownership or valid data is lost, and arm a newly
   acquired hand only after release. The sequence counts sampled edges;
   pulses entirely between OpenXR input samples cannot be recovered.
6. Send flags zero when the menu closes. A matching token can always release
   its lease even if its snapshot is now stale.

The lease lasts at most 100 ms and must be renewed. Hit distance is positive
and at most `MaximumHitDistanceMeters`. Pitch trim is finite within -90 to
90 degrees. Changing pitch invalidates old rays until a new calibrated
publication arrives. Tracking loss, recenter, origin changes, session
shutdown and lease expiry revoke ownership. No endpoint from a previous
publication is drawn against a newer ray.

An accepted hit retains its exact ray publication for rendering when the
next tracking frame arrives before layer submission. Input queries still
return the latest tracking frame. The retained publication must remain
fresh within 100 ms and loses rendering ownership immediately on tracking
loss, keyboard preemption, session changes or lease expiry. A renewal
without hit flags hides the beam while retaining valid drag ownership.

## Input and display ownership

An existing native menu/console trigger hold keeps its hand until release.
An accepted OCU keyboard request immediately preempts both external hands,
and a visible keyboard retains that priority. Ownership
loss guards the physical trigger until a confirmed release, including a
press newer than the last published sample. Both the game-facing legacy
controller API and OCU's internal menu consumers respect this guard; the
publisher reads physical input directly. This prevents a click or drag
from being replayed into a newly exposed menu.

The snapshot also publishes the atlas's nominal idle/pressed beam and dot
core colors as straight sRGB RGBA. Both physical hands use this same style.
The beam and dot intentionally differ in brightness; the dot has additional
soft halos. Consumers use these values without premultiplying twice, and
apply their normal sRGB-to-linear conversion if their render target needs it.
Atlas generation and publication use the same central palette definition.

OCU draws its existing 3 mm beam using the exact published ray and the
consumer's hit distance. OCU omits its dot for these hands; the consumer
owns the cursor. Native OCU menu and console beams skip externally owned
hands. With only the external menu open, publication and beam submission
remain active independently of Skyrim's native menu state. Consumer code
must not apply a second pitch correction or ray/cursor filter.

## Validation

Configure the runtime using the root `BUILDING.md`, then build and run:

```powershell
cmake --build OpenCompositeSkyrimVR/build --config Release --target OCUPointerStateTest OCUEffectFoveationSelfTest OCOVR
& OpenCompositeSkyrimVR/build/tests/Release/OCUPointerStateTest.exe
& OpenCompositeSkyrimVR/build/tests/Release/OCUEffectFoveationSelfTest.exe
```

The pointer policy test covers bootstrap, token exclusion, native holds,
keyboard handoff, expiry, release guards, raw trigger transitions, stale
endpoints, retained presentation across tracking frames, pitch changes,
argument validation and session epochs. A headset
check should also cover a consumer-only menu, two-hand aiming, rapid clicks,
dragging off the menu, keyboard opening, recenter and disconnect/reconnect.
