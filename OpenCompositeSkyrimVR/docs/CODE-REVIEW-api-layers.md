# Code review — the OpenComposite changes made for headless startup

Reviewed 2026-08-08 against the working tree the feature was developed in, range `367be7a^` →
working tree. 14 files, +639 / −11. That tree is gone; the reviewed work lives on this branch.

---

## Resolution (2026-08-08)

All findings actioned. The two commits were squashed into one — "Discover OpenXR API layers in
xrlayers/, and publish a small C ABI", the first commit of this branch — which is the PR.

| | Outcome |
|---|---|
| **F1** depth extension | **Removed entirely.** `XR_KHR_composition_layer_depth` is no longer enabled and `g_depthLayerAvailable` is gone; both `hasDepth` sites are back to their original form. OCU's depth behaviour is now bit-identical to before the feature. |
| **F2** non-participating owners | **Fixed on the layer side, the safer of the two options.** Rather than surgery at 8 `xrCreateSwapchain` sites in working OCU render code, `real::CreatePassthroughSwapchain` now hands runtime images only to a chain the app is rebuilding *at our request* — matched against a shape seen destroyed since the last `InvalidateSwapchains`. A first-time keyboard/laser chain has no destroyed predecessor, so it keeps the layer-owned chain and its copy, which is always correct. `compositor.h` and `OpenCompositeInterface.h` now name the participating owners exhaustively instead of asserting coverage they didn't have. |
| **F3** failed rebuild not sticky | **Fixed.** Both providers advance `m_swapchainGeneration` only after the chains exist, and clear `m_ready` on failure — which is what stops SpaceWarp's cached `m_info` handles reaching `xrEndFrame`. |
| **F4** `USE_SYSTEM_OPENXR` | **Withdrawn — I had this wrong.** `OCCore` already referenced the vendored jsoncpp include path unconditionally, and `BaseClientCore.cpp` already uses `Json::Value`. jsoncpp is a pre-existing project-wide dependency and `USE_SYSTEM_OPENXR` was already broken; the new line follows the established pattern rather than introducing anything. No change made. |
| **F5** `XR_API_LAYER_PATH` matching | **Validated as reachable, and fixed.** The reachable case is a user who set the variable by hand to debug a layer — exactly the person who'd type different casing — which would prepend a second copy of the same folder and insert every layer in it twice. Now split on `;`, canonicalised, compared case-insensitively, and read with a correctly-sized buffer so a long value can't be silently overwritten. |
| **F6** indentation | Fixed (the lines were reverted with F1). |
| **F7** test duplication | Left as-is — acknowledged in the file, and the fixtures are good. |
| **F8** stale commit messages | Gone with the squash; the new message documents `enableApiLayers` only. |

Verified: OCU builds clean (no new warnings), the layer builds clean, `OpenComposite_GetInterface`
is still exported from `vrclient_x64.dll` (dumpbin), and `OCUApiLayerManifestSelfTest` passes all
11 fixtures and 10 containment cases.

Not run: anything against a real headset. The migration path itself is unchanged by this work, but
the passthrough narrowing in F2 has not been exercised live.

The findings below are the original review, kept as the record.

Two commits are already made (`367be7a`, `d90753f`); the rest is uncommitted working tree. This
review treats the whole range as one prospective pull request, because that is how a reviewer
upstream would see it.

---

## 1. What the Virtual HMD mod is, in three paragraphs

Skyrim VR refuses to start without a headset: it asks the VR stack "is there an HMD?" before it
opens a window, and quits on "no". The MGON modlist takes ~15 minutes to load, so that entire load
has to happen with the Quest on your face.

`XR_APILAYER_OCU_virtual_hmd` is a standard OpenXR **API layer** — a DLL the loader inserts between
the application and the runtime. When no headset is present it answers that question itself and
then impersonates a complete VR system: a HMD with a name, resolution and refresh rate recorded
from a previous real session; two eye views; real D3D11 textures nobody looks at; a frame clock
ticking at the right rate (the only thing stopping the GPU rendering flat-out for 15 minutes); a
stationary head pose; and controllers reported *present but not connected*. A background thread
polls the runtime every 1.5 s; when a headset appears the layer builds a real session on Skyrim's
own D3D11 device, replays everything the app configured while faking (action bindings, reference
spaces), warms the real session with blank frames, and swaps over — keeping every handle the
application is holding byte-identical. If anything fails it tears the real side down and goes back
to faking rather than returning an error, because OpenComposite aborts the process on any XR error.

The layer is deliberately independent of OpenComposite: it does not name it, and nothing in it
knows Virtual Desktop exists. **The OCU changes under review exist to meet it halfway**, and are
the subject of the rest of this document.

---

## 2. Inventory of the OCU changes

Three separable pieces. They are currently one range; **they should be three pull requests.**

### A. API-layer discovery (`enableApiLayers`, default on)

`DrvOpenXR.cpp` gains ~245 lines. Any `*.json` in an `xrlayers/` folder next to the OCU DLL is
parsed with the loader's own vendored jsoncpp, validated, and — if the loader independently
reported the layer — named in `xrCreateInstance`. `XR_API_LAYER_PATH` is prepended before the
first `xrEnumerateApiLayerProperties`, which is the only moment that works.

Validation rejects: unopenable file, bad JSON, missing/empty `api_layer.name` or `library_path`,
a bare filename with no separator, an unresolvable path, a DLL outside the game folder, and a DLL
that does not exist. Every rejection logs a reason and skips; nothing aborts.

**This is the good part of the change.** OCU knows nothing about the Virtual HMD layer specifically —
a manifest plus the DLL it names is the whole contract. The one genuinely fatal mistake available
here (naming a layer the loader did not find → `XR_ERROR_API_LAYER_NOT_PRESENT` → dead game) is
guarded by intersecting against `availableLayers` (`DrvOpenXR.cpp:531`). The three loader caveats
in the header comment are hard-won and correct.

### B. The `OpenComposite_GetInterface` C ABI

`OpenCompositeInterface.h` (new, 105 lines) plus an export in `compositor.cpp`. One function today:
`InvalidateSwapchains()` — "discard your swapchains and rebuild on the next submit". Implemented as
a global atomic generation counter that each swapchain owner compares against its own stored copy.

This exists so the layer can retire the per-frame eye copy after migration: it hands OCU the
runtime's own textures and asks OCU to rebuild onto them. Without it the layer copies ~54 MB/frame
and holds ~160 MB of duplicate VRAM.

The versioning design (append-only fields, `structSize` from the *provider*, consumer asks for the
*lowest* version it needs, `OPENCOMPOSITE_HAS`) is correct and covers all four skew directions. The
header is vendored into the layer at `skse/virtual-hmd/src/OpenCompositeInterface.h`; I
diffed the two — **identical apart from the intended "vendored copy" banner.** No drift today.

### C. Riders, unrelated to headless

- `XR_KHR_composition_layer_depth` is now actually enabled (`DrvOpenXR.cpp:504`).
- `noProfileLogged[2]` in `XrBackend` — de-spams "No interaction profile detected".
- `OCUApiLayerManifestSelfTest` + 11 fixtures + two docs.

---

## 3. Verdict

**Not pull-request ready as one range. Piece A is close. Piece B has a correctness gap. Piece C
contains a behaviour change that does not belong in this PR at all.**

Nothing here is dangerous *for this modlist today* — the failure modes are degradation, not
crashes, and the discovery path is provably inert when `xrlayers/` is absent. But three findings
below would be flagged by any careful reviewer, and two of them are real defects rather than style.

| | |
|---|---|
| Safe by default when the layer is absent | **Yes** — verified, see §5 |
| Changes pre-existing behaviour | **Yes, one item** — F1, and it is ungated |
| Ready for production in this modlist | **Yes, with F2/F3 accepted as known limits** |
| Ready to submit upstream | **No** — split into three PRs and fix F2–F5 |
| Comment volume | **Roughly 2× what the surrounding code uses.** See §6 |

---

## 4. Findings

### F1 — HIGH — Enabling `XR_KHR_composition_layer_depth` is an unrelated, ungated behaviour change

`DrvOpenXR/DrvOpenXR.cpp:504-508`, `XrBackend.cpp:820`, `XrBackend.cpp:1578`

Before this change OCU chained `XrCompositionLayerDepthInfoKHR` onto its projection views *without
enabling the extension*. That is a spec violation, and runtimes were entitled to ignore the struct —
in practice most did. After this change, on any runtime that advertises the extension, **the runtime
will start consuming that depth buffer for reprojection and positional timewarp.**

That is a strict correctness fix on paper. In practice it means every existing user's frame
composition changes on the first run of the new build, driven by depth data that has never been
validated against a runtime that actually reads it — reversed-Z convention, `nearZ`/`farZ`
population, and the `DepthLayerValid()` resolution gate have all only ever been exercised against
runtimes that threw the struct away. If any of that is wrong, the symptom is warping/shimmer on
moving geometry, and it will be blamed on the headless work.

It is also simply out of scope: nothing about headless startup requires it.

**Do:** move it to its own commit, put it behind an ini key defaulting to `false` for one release,
and validate against a runtime that honours the extension. The *other* half of the same hunk —
requiring `g_depthLayerAvailable` before setting `hasDepth` — is an unambiguous improvement and can
stay.

*(For the record, `g_depthLayerAvailable`'s naming and placement match the adjacent
`g_spaceWarpAvailable` exactly. Not a finding.)*

---

### F2 — HIGH — Three swapchain owners never answer `InvalidateSwapchains`, and the doc comment says they do

`OpenOVR/Compositor/compositor.h:53-66` claims:

> "there are several independent swapchain owners (each eye, overlays, the keyboard, the menu laser)
> … Each owner compares against its own stored copy, so they need no coordination"

Only three code paths actually read `SwapchainGeneration()`:

- `dx11compositor.cpp:4249` (eyes, and overlays that are `DX11Compositor` instances)
- `ASWProvider.cpp:800`
- `SpaceWarpProvider.cpp:147`

These call `xrCreateSwapchain` directly and **never rebuild**:

| Site | Chains |
|---|---|
| `OpenOVR/Misc/Keyboard/VRKeyboard.cpp:1266, 1352, 1414, 1478` | keyboard, laser, target dot, console |
| `OpenOVR/Misc/Keyboard/VRMenuLaser.cpp:78, 140, 209` | beam, dot, debug quad |
| `OpenOVR/Reimpl/BaseOverlay.cpp:1831` | `s_trailChain` |

(`dx12compositor`, `glcompositor`, `vkcompositor` also don't — irrelevant for Skyrim VR, relevant
if this is pitched upstream as a general mechanism.)

**Why it matters, concretely.** The layer's `CreatePassthroughSwapchain` (`Real.cpp:1583`) hands
runtime-owned textures to *any* colour-format swapchain created while live — it filters on depth
format, not on purpose. So if the keyboard or menu laser is first opened *after* a migration, its
chains become passthrough chains holding the real session's textures. `Real.cpp` is explicit that a
passthrough chain pins the session until the app destroys it, and the only way to make that happen
is `InvalidateSwapchains`. These owners will never respond.

Failure mode at the next retirement (second migration, or a Virtual Desktop restart):
`OutstandingPassthroughCount()` never reaches zero → the retiring session is never destroyed →
permanent VRAM leak, `g_retiring` stays set, and `CreatePassthroughSwapchain` returns false for
everything afterwards, so the copy path comes back and never leaves. It logs (`Real.cpp:526`) and
does not crash — but it is a silent one-way degradation of the exact feature the ABI exists to
deliver.

**Do:** either add the four-line generation check to `VRKeyboard`, `VRMenuLaser` and
`BaseOverlay::s_trailChain`, or — cheaper and safer — narrow `CreatePassthroughSwapchain` to the
chains OCU actually rebuilds (eye-sized, non-overlay), and correct the `compositor.h` comment to
list what really participates. Right now the comment is the most misleading thing in the diff,
because it reads as an audited list.

---

### F3 — MEDIUM — A failed provider rebuild is not sticky and never retries

`DrvOpenXR/ASWProvider.cpp:799-829`, `DrvOpenXR/SpaceWarpProvider.cpp:147-178`

Both providers advance `m_swapchainGeneration` **before** attempting the re-creates:

```cpp
m_swapchainGeneration = current;      // committed here
…destroy old chains…
if (!CreateOutputSwapchain(...)) return false;   // …but this can fail
```

On the next frame the generation now matches, so `RebuildSwapchainsIfInvalidated()` returns `true`
immediately and execution falls straight through into the submit path.

- **ASW:** `m_outputSwapchain` is `XR_NULL_HANDLE`, `m_ready` is still true.
  `SubmitWarpedOutput` calls `xrAcquireSwapchainImage(XR_NULL_HANDLE, …)`. The runtime returns
  `XR_ERROR_HANDLE_INVALID`, which is caught and logged five times — then silence. ASW is dead for
  the session with no further diagnostic.
- **SpaceWarp:** worse. If the MV chain rebuilds but the depth chain fails, the function returns
  before the repoint loop, so `m_info[eye].motionVectorSubImage.swapchain` still names the
  **destroyed** MV chain. `IsReady()` is unaffected by the failure, and `XrBackend.cpp:1171` chains
  `GetLayerInfo(i)` on `IsReady() && app_layer` alone — it never consults `SubmitFrame`'s return
  value. A destroyed handle reaches `xrEndFrame`, hitting `OOVR_FAILED_XR_SOFT_ABORT`
  (`XrBackend.cpp:1247`), which is a hard abort with a message box when `StopOnSoftAbort` is set.

**Do:** advance `m_swapchainGeneration` only on full success, and set `m_ready = false` on failure
so the state is consistent either way. Two lines each.

---

### F4 — MEDIUM — `USE_SYSTEM_OPENXR` no longer builds

`CMakeLists.txt:231`, `CMakeLists.txt:637-651`

`${XrDir}` is only set inside the `if (NOT OpenXR_FOUND)` branch (`CMakeLists.txt:143`). Both new
uses reference it unconditionally, so with a system OpenXR they expand to `/src/external/jsoncpp/…`
and `#include <json/json.h>` in `DrvOpenXR.cpp:16` fails outright.

There is a second, quieter issue underneath it: the jsoncpp symbols resolve today only because the
three `lib_json` sources happen to be compiled into the vendored `OpenXR` static lib
(`CMakeLists.txt:178-180`) and leak out of it. That is an internal implementation detail of the
loader build, not a supported dependency, and it will disappear the day anyone links a real
`openxr_loader`.

**Do:** guard both blocks on `NOT OpenXR_FOUND`, or add the three jsoncpp sources to `DrvOpenXR`
explicitly so the dependency is stated rather than inherited.

---

### F5 — MEDIUM — `XR_API_LAYER_PATH` is matched by case-sensitive substring

`DrvOpenXR/DrvOpenXR.cpp` (the `alreadyListed` block, ~line 287)

```cpp
&& std::string(existing).find(layerDir) != std::string::npos;
```

Three distinct ways this misbehaves:

1. **False positive.** `layerDir` = `…\Game\xrlayers` is a substring of an existing
   `…\Game\xrlayers2` entry → we conclude it is already listed, never set the variable, and **no
   layers are discovered** — while logging "already on XR_API_LAYER_PATH", which points a
   debugger in exactly the wrong direction.
2. **False negative → double insertion.** The comparison is case-sensitive and does not normalise.
   A pre-existing entry differing only in case, in trailing separator, in `/` vs `\`, or an 8.3
   short path, does not match, so we prepend the same folder again. Per the layer's own
   `README.md` finding 2, **the loader deduplicates layer manifests by neither name nor path** —
   the layer is inserted into the chain twice, both copies share the DLL's globals, and the second
   silently severs the first. That finding was learned the hard way and this code can reproduce it.
3. **Silent truncation.** `existing[4096]`: if `XR_API_LAYER_PATH` is longer,
   `existingLen >= sizeof(existing)`, so the append is skipped and
   `SetEnvironmentVariableA` **overwrites** the variable with only `layerDir` — destroying another
   tool's search path with no log line at all.

Practically unreachable in a normal install, but (2) is the one catastrophic failure this code is
otherwise carefully written to avoid, and the fix is small: split on `;`, compare
case-insensitively after `CanonicalPath` normalisation, and log when the buffer is too small.

---

### F6 — LOW — Broken indentation

`DrvOpenXR/XrBackend.cpp:1579` — two tabs and four spaces where six tabs are needed. A visible
paste artefact in a hunk a reviewer will look at.

### F7 — LOW — The self-test duplicates production logic verbatim

`tests/ApiLayerManifestSelfTest.cpp` copies `CanonicalPath`, `PathIsWithin` and the validation
ladder, and the file header openly says the duplication is the point. It is a defensible
compromise, but the two containment helpers have no dependencies at all — lifting them into a
three-function header both sides include would let the test cover the real code with no loss.
As written, the test proves the fixtures agree with a *copy* of the rules.

The 11 fixtures themselves are good coverage: malformed JSON, missing node, missing fields, wrong
types, empty file, empty name, bare filename, missing DLL, `..`-escape, absolute-outside.

### F8 — LOW — Commit messages describe ini keys that no longer exist

`367be7a` documents `headlessStartup` and `headlessSwapchainHandover=false`. Neither exists in the
tree — they were generalised into `enableApiLayers`, and the handover kill switch was dropped
entirely. Anyone reading `git log` to find the escape hatch will not find one. Reword on rebase,
and consider whether the handover really should have no off switch: it is the piece with the
subtlest failure mode (F2) and the one most likely to want disabling in a bug report.

---

## 5. Does it change pre-existing behaviour?

Audited each change for "what happens to a user who never installs the layer":

| Change | Inert without the layer? |
|---|---|
| `DiscoverApiLayers()` at startup | **Yes.** `GetModuleHandleEx` + one `GetFileAttributes`; missing folder logs and returns. |
| `XR_API_LAYER_PATH` set | **Yes** — only written when the folder exists *and* holds manifests. |
| `layers[]` → `std::vector` | **Yes.** Old code: `count = size − 1` for the MSVC dummy. New: `layers.size()`. Same values in both `XR_VALIDATION_LAYER_PATH` states. |
| `Compositor::InvalidateSwapchains` / generation | **Yes.** Counter stays 0; `invalidated` is always false; the added branch never fires. |
| `OpenComposite_GetInterface` export | **Yes.** A new export nobody calls. |
| Provider `RebuildSwapchainsIfInvalidated` | **Yes.** Returns true on the first comparison. |
| `noProfileLogged` | Log-only, and a clear improvement — this was writing a line every frame with controllers asleep. |
| **`XR_KHR_composition_layer_depth` enabled** | **NO — see F1.** The one genuine behaviour change. |

One deliberate, documented side effect worth putting in release notes: setting `XR_API_LAYER_PATH`
**suppresses the loader's registry search for explicit layers entirely.** While `xrlayers/` exists,
system-wide explicit layers become invisible to Skyrim. Correctly called out in the source comment;
it is not mentioned in `docs/API-LAYERS.md`.

---

## 6. Are the comments too verbose?

**Yes, by roughly a factor of two against the surrounding code — though the problem is repetition,
not depth.**

Measured: `OpenCompositeInterface.h` is 105 lines for one function pointer, ~80 of them comment.
`compositor.h` carries a 20-line block for a 3-line function. `dx11compositor.cpp` carries a 7-line
block for a 5-line change, dropped into a function whose existing comments read
`// Check if existing chain is compatible (compare against OUTPUT dimensions)`.

The specific problem is that **the "a counter, not a flag" rationale is stated six times** — commit
message, `compositor.h`, `dx11compositor.cpp`, `OpenCompositeInterface.h`, `ASWProvider.h`,
`SpaceWarpProvider.h` — in near-identical prose. So is "reads zero for ever unless something asks,
so this is a no-op for everyone else" (three times). A reviewer reading the diff top to bottom hits
the same paragraph repeatedly, which makes the diff feel much larger than it is and buries the
comments that *are* carrying unique information.

There is also an idiom mismatch: `── Section ──` box headers and Doxygen `/** */` blocks are not
how the rest of OCU is written.

**Keep, without hesitation** — these are load-bearing and cost real time to learn:

- The three loader caveats (enumeration caching, `PlatformUtilsGetSecureEnv` and elevation,
  registry suppression) in `DrvOpenXR.cpp`.
- Why naming an undiscovered layer is fatal.
- Why `library_path` must not be a bare filename.
- The skew table in `OpenCompositeInterface.h` and why the consumer asks for the *lowest* version.
- Why the export lives in `compositor.cpp` (static-lib object-file pull-in — verified with dumpbin).
- The `m_info` repoint note in `SpaceWarpProvider.cpp`.

**Cut:** the counter-vs-flag rationale down to one canonical statement in `compositor.h`, with the
other five reduced to `// see Compositor::InvalidateSwapchains`. Same for the no-op-by-default note.
That alone removes ~50 lines and makes the diff read as the small change it actually is.

---

## 7. Suggested shape for submission

1. **PR 1 — API layer discovery.** Piece A, plus the self-test and docs. Fix F4, F5, F7.
   Genuinely useful to OCU on its own merits and mentions no consumer by name.
2. **PR 2 — `OpenComposite_GetInterface` + `InvalidateSwapchains`.** Piece B. Fix F2 and F3
   first — F2 is the one that would be caught in review and is embarrassing because the comment
   asserts the coverage it doesn't have. Restore an ini kill switch (F8).
3. **PR 3 — depth extension.** F1, on its own, gated, with a note that it was previously being
   chained illegally.
4. Rebase-fix F6 and the stale commit messages anywhere in the range.

For this modlist right now: **ship it.** F2 and F3 are latent, degrade rather than crash, and both
are logged when they fire. F1 is the one to actually watch — if reprojection artefacts show up
after this build, that hunk is the first thing to revert.
