# CSX / OCU Accepted Scene Draw API

**Revision:** Draft 1, 6 September 2026

**Status:** Proposal for Treatid; OCU consumer implemented locally against the accompanying header and mock provider. Not an existing or agreed CSX API. No upstream changes or live-provider qualification have been performed.

## 1. Objective and scope

Replace OCU's private CSX instruction patches with a versioned notification of
draws actually submitted to Skyrim VR's main scene. DAPA uses those draws to
create a player-owned depth mask, excluding player/body/held-item pixels from
world-locomotion correction. OCU already classifies player ancestry, equipped
geometry, HIGGS-held roots and Spell Wheel objects. CSX need not implement those
ownership rules or DAPA's motion/warp algorithm.

Initial platform: Windows x64, Skyrim VR 1.4.15, D3D11 immediate-context rendering.
This contract does not promise ENB integration, frame generation, transparent
surface reconstruction, motion prediction, or compatibility with every renderer.

**Success:** OCU selects the negotiated API and installs no engine mesh-call,
geometry-vtable or private CSX accepted-draw patches for its player mask.

## 2. What Treatid needs to implement

1. Expose the versioned function table in the accompanying `CSXAcceptedDrawAPI.h`.
2. Deliver accepted scene draws synchronously, with their actual geometry and
   live rendering state, including CSX's no-interception fast path.
3. Provide an isolated replay operation valid inside that notification.
4. Guarantee registration, resource lifetimes and quiescent unregistration.
5. Advertise capabilities only when their complete semantics are implemented.

CSX's existing enhanced API can carry this functionality instead of a new
export. The proposed names below are a concrete interoperable starting point.
If your existing interface offers an equivalent contract, share its header and
we will adapt OCU's discovery layer rather than request duplicate APIs.

## 3. Discovery and binary ABI

Canonical declaration: `OpenCompositeInput- Skyrim SKSE/OpenCompositeInput/include/CSXAcceptedDrawAPI.h`.

Proposed unmangled export from `CommunityShaders.dll`:

```cpp
extern "C" __declspec(dllexport)
const CSXAcceptedDrawAPI::API* __cdecl CSX_GetAcceptedDrawAPI(
    uint32_t requestedVersion, uint32_t minimumTableSize);
```

- Use the requested ABI version exactly. Return null for unsupported requests.
- v1 uses default MSVC x64 alignment, not packed structs. `API` is 32 bytes;
  `Draw` is 88 bytes; `Arguments` is 24 bytes. Header static assertions enforce this.
- The boundary uses fixed-width integers, borrowed pointers and function pointers;
  no STL containers, C++ references, exceptions, cross-module allocation/free,
  or ownership-transferring smart pointers.
- `structSize` allows trailing additions. Existing field layout/semantics must
  not change within v1. Consumers accept larger structs but ignore trailing data.
- The returned table, functions and module must remain valid while registered.
  They are immutable during a subscription. Hot provider unload is unsupported.
- Make the API ready by SKSE `DataLoaded` (or, at latest, the consumer's first
  successful renderer initialization). OCU retries unavailable renderer resources
  at game load but does not hot-switch an active legacy installation to API mode.

## 4. Required coverage and capabilities

OCU currently requires all six `RequiredCapabilities` bits:

| Capability | Required guarantee |
| --- | --- |
| FullSceneIndexedCoverage | Every accepted main-scene indexed and indexed-instanced lighting/effect draw is observable, including unmodified/fast-path draws. |
| NativeVRGeometry | Each event identifies the exact live Skyrim VR `RE::BSGeometry` for that submitted draw. |
| PostDrawLiveState | Notification occurs after original command submission and before its bound geometry/state is changed. |
| IsolatedReplay | Replay bypasses ordinary scene capture, suppression, recursive notifications and normal scene-draw accounting. |
| QuiescentUnregister | Successful unregistration guarantees no queued, running or future callback for that subscription. |
| PackedStereoDepth | Events describe the canonical single-sample, single-array-slice 2D packed-stereo scene depth supported by OCU's current consumer. |

The two accepted calls in `VRMenuBridgeDirectDrawHook` are the immediate
motivation: the fast-path draw and the final draw after suppression. **Those two
calls alone are not enough to claim full coverage** if other main-scene draws
bypass them. The initial consumer intentionally requires full coverage to avoid
simultaneously replaying geometry through API and legacy hooks. A partial API
will be declined; a hybrid route needs a separately agreed, disjoint coverage
contract before implementation.

Do not claim coverage for deferred command lists, texture arrays, separate-eye
depth textures or a different geometry ABI merely because basic callbacks work.

## 5. Exact callback timing and event meaning

For each accepted original main-scene draw:

1. CSX completes its suppression, redirect, shader and geometry decisions.
2. CSX issues the original D3D draw exactly once.
3. CSX invokes observers synchronously on that same rendering thread, **before**
   changing its context state or ending the geometry's lifetime.
4. Observers optionally replay into their private outputs and restore state.
5. CSX continues normally.

"After draw" means after CPU command submission, not GPU completion. No flush,
fence wait, staging readback, capture file or additional present is required.
A notification after the entire higher-level hook returns is insufficient if
CSX already restored/replaced the relevant state.

- Suppressed original draws MUST NOT produce an accepted-scene event.
- Shadow, reflection and UI-capture draws must be omitted or correctly tagged;
  OCU ignores all non-`MainScene` events. Never describe a redirected capture as
  an accepted main-scene draw merely because the originating object is a player.
- If CSX splits a draw, emit one event per actual submitted draw. Counts and
  geometry identity must describe that draw. Do not merge unrelated owners into
  one identity. Batched draws with inseparable mixed ownership are unsupported.
- `drawId` is nonzero and strictly increasing in notification order for original
  draws on the single rendering thread for the provider's process lifetime.
  Do not reset it on load, menu changes or recenter. No event for a replay.
- All event/context/geometry/depth pointers are borrowed until callback return.
  OCU must not retain geometry or the replay token. Resources used beyond that
  callback require explicit COM ownership, independent of the event's lifetime.
- The bound IA buffers/layout/topology, VS/HS/DS/GS and their inputs/constants,
  viewport/scissor and rasterizer state must still correspond to the original
  draw. An opaque geometry token is not equivalent to the specified native pointer.
- `Indexed` encodes `instanceCount=1` and `startInstance=0`; the six argument fields
  otherwise describe native D3D11 indexed/instanced arguments exactly, including
  signed `baseVertex`. Zero-sized draws need not be notified.

## 6. Isolated replay contract

`draw.replay(draw.replayToken)` is callable at most once by each observer during
that observer's callback, on the same context/thread. The token is opaque and
may be null. The provider must guard token validity/lifetime and recursive use.

OCU temporarily installs its own R32 depth-mask render target, pixel shader,
depth-read state, blend state and scene-depth SRV. Replay MUST honor that current
temporary output state while reusing the original geometry/rasterization inputs.
It must not reinstall CSX's original colour outputs over OCU's mask output.

Replay issues only the requested geometry command. It MUST NOT:

- notify observers again;
- re-enter menu capture/suppression, temporal-history updates or ordinary draw counters;
- call the full high-level scene renderer or issue the original scene draw again;
- wait for the GPU or change resource lifetimes behind the consumer.

Return `Success` only when the replay command was submitted. Other results must
not be reported as successful mask coverage. OCU restores its modified state
even after a non-success return. Both parties MUST keep C++ exceptions inside
their own module; the replay function must not throw across the boundary.

Callbacks are serial. Before invoking the next observer the provider must see
the same native state, assuming the preceding observer met its restoration duty.
The original draw must proceed normally even when there are no observers or an
observer declines to replay.

## 7. Depth and stereo requirements

`sceneDepth` identifies the canonical original-scene depth represented by the
currently bound DSV, not a menu capture, shadow map, unresolved multisample surface
or post-upscale replacement. Current OCU additionally verifies it matches its
published scene-depth bridge resource and the live DSV resource. A mismatch is
rejected, not reinterpreted as compatible depth.

v1's OCU implementation supports a single-sample, one-slice 2D packed-stereo
resource. Its existing projection/depth bridge remains in use; this proposal
does not replace it or assume new per-eye transforms exist in CSX's API.
Resizes must be reflected by the live event resource and OCU's bridge consistently.
OCU validates visible depth and preserves the original rasterizer bias.

The current depth-based mask is not a complete solution for transparent or
decorative surfaces that contribute no useful scene depth. Do not claim that
the callback itself fixes those materials. A future resource/coverage-mask API
may address that separately.

## 8. Registration and shutdown

- `registerObserver(observer,user,&subscription)` returns `Success` with a nonzero
  unique subscription, or failure with zero and no retained callback.
- Callbacks may start during registration, but OCU deliberately ignores them
  until registration completes and initialization is ready.
- Registration/unregistration are initialization/lifecycle operations, never
  called from an accepted-draw callback. The provider must safely synchronize
  its callback list without holding a lock across callbacks that could deadlock
  replay or normal renderer operations.
- `unregisterObserver(subscription)` is quiescent on success. The caller keeps
  its callback code and user data alive if unregistration fails.
- OCU installs a process-lifetime client; hot SKSE DLL unloading is unsupported.
  Its explicit stop path disables delivery before unregistering and retains an
  unsuccessful subscription for cleanup retry. Never unload a provider or client
  with callbacks still registered.

## 9. OCU selection and failure behavior

1. Query the proposed export on the loaded `CommunityShaders.dll`.
2. Validate version, table size, every required capability and function pointer.
3. Register; on success, enable the API mask path and return before installing
   any legacy mesh/geometry/CSX patch.
4. If missing/incompatible/registration fails, try the existing verified legacy
   route. Do not run both mask routes simultaneously.
5. If neither route starts, keep world correction unchanged, log the failure and
   show the existing once-per-session player/body-correction warning after load.

Successful registration is not proof of useful player mask coverage. OCU logs
the first successfully replayed player draw separately and retains its rejection
counters. Live validation must check this and the actual both-eye mask output.

## 10. Performance and acceptance tests

Disabled provider cost should be a cheap subscriber-presence check. No per-draw
heap allocation, file logging, cross-thread queue, CPU/GPU wait or readback.
With OCU registered, non-player draws are classified but not replayed. Player
replay uses the existing mask path; the API is not a zero-cost guarantee.
Measure CPU/GPU frame time rather than claiming an unmeasured improvement.

Provider and consumer acceptance checklist:

- Fast path and bridge path: original draw once, event once, replay only when requested.
- Suppression and redirected capture: no main-scene mask event.
- Indexed/instanced arguments, including nonzero offsets and negative base vertex.
- Body/armor, equipped weapons, HIGGS objects and Spell Wheel; lighting/effect geometry.
- Non-player/nested/replay draws do not inherit player ownership or recurse.
- Both eyes; main-scene depth and colour unchanged by replay; changed D3D state restored.
- Resize/load/recenter; resource identity/layout mismatch is rejected safely.
- Missing/wrong API, missing capability, registration failure, quiescent unsubscribe.
- With API selected, no OCU private CSX, engine-mesh or geometry-vtable hooks installed.
- Existing six-DLL legacy regression matrix still passes without the provider export.
- Live NVIDIA and AMD runs with the real provider; no claim based solely on mock tests.

## 11. Current deliverables and remaining agreement

- Shared header: `include/CSXAcceptedDrawAPI.h` in the SKSE project.
- Consumer: `src/DapaCsxApi.h`; player-mask integration in `src/DapaPlayerMask.inl`.
- Mock test: `tests/DapaCsxApiTest.cpp`, explicit CMake target `DapaCsxApiTest`.

The mock verifies consumer-side negotiation, partial-capability refusal,
registration, accepted/indexed delivery, malformed/non-scene rejection,
duplicates, defensive recursion suppression, unregistration/reconnect and ABI
exception containment. It does not establish the provider's actual shader state,
coverage, GPU output or quiescence implementation. Existing synthetic mask GPU
tests remain separate. Real CSX integration and headset qualification are pending.

Before freezing v1, agree on the export versus your existing API transport,
full-scene hook coverage, native geometry identity, isolated replay implementation,
and canonical depth/packed-stereo semantics. No remote PR is required to review
this proposal; repository publication remains separately authorized.
