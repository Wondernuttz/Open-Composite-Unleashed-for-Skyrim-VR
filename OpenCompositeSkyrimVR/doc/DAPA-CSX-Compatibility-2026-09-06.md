# DAPA CSX accepted-draw compatibility — 6 September 2026

## Code-location discovery and visible failures — 6 September follow-up

Startup first tries the exact contracts below, then inspects the actual chained
CommunityShaders owner for an identical known full function at a different RVA.
The fallback requires a matching PE unwind function boundary/size, the same
image module, executable image memory, and the full machine-code SHA-256 before
rebasing the two verified draw offsets. This is not a wildcard byte scan or a
normalized-displacement matcher: changed instructions, including RIP-relative
operands, still require independent validation. Lee's reference changes are
handled by his new exact contract, not by this fallback.

If mask startup remains unavailable, a once-per-session game-thread message box
appears after new-game/save load with DAPA enabled. It states that body/held-item
correction is inactive while world correction remains enabled and names the log
to collect. Failure output is not behind verbose logging. Hook rejection logs
identify the actual owner module instead of blaming CSX merely because it is
loaded. The notice reports startup failure, not proof of subsequent pixel coverage.

The six-DLL matrix passes with extra code-rebasing and wrong-module/boundary/hash
negative tests. Native map and notification source-contract checks also pass.
Message-box appearance still needs live Skyrim verification. There is no new
per-frame matching, polling or GPU work.

### ENB audit limits

OCU's native engine path already calls the current context DrawIndexed dispatch
instead of retaining/overwriting D3D vtable slots. It uses Skyrim's published
context rather than assuming a proxy's returned immediate context is identical.
The mask still requires matching scene depth and captures/restores D3D state.
Those checks matter for ENB/wrappers, but do not establish compatibility with a
particular ENB binary. An unknown engine-call owner is preserved and rejected,
not routed through CSX's private acceptance contract.

The official ENB VR download index lists 0.495, 0.457 and 0.375. On this audit,
their pages were readable but the linked ZIP requests returned HTTP 403. No ENB
wrapper DLL was found in the searched Downloads/MO2 files. None of these ENB
binaries was installed, executed, patched or qualified. Obtain the official
archives manually for further local binary inspection and live wrapper tests;
do not describe synthetic mutable-dispatch/state tests as all-ENB validation.

## Lee's exact AIO build — 6 September follow-up

The user-supplied `CSX_AIO-VR-main-VR-1595cffd-Release.7z` contains
`CommunityShaders.dll` SHA-256
`B5D674790EED6D1FECB52088478512651FB6EE1455BD513C6B367BF9A3C1D338`.
The previous five-contract adapter rejects this binary. Its accepted-draw
owner is at RVA `10E240`, size `386` hex, with whole-function SHA-256
`81F40849D98FFC1B84B68A6C0C86F5A87106155006A678E9E210C197D14B13B6`.
The native draw sites are `10E2AC` (fast path) and `10E582` (after suppression).

Disassembly shows 226 instructions, the same owner-relative branch structure,
register/stack argument setup, and caller marker `DBDDF9` as PR21. There are
34 raw instruction differences; normalizing RIP-relative references and external
call destinations yields no instruction differences. This is a structural
comparison of the owner, not proof that all referenced CSX functions are identical.
Both accepted draw instructions and the suppression branch were reviewed directly.
The owner moved by `-380` hex relative to PR21, and the changed references also
change its hash. Rejection was an unrecognized layout, not proof of incompatible
draw semantics or an AMD-specific failure.

This adds a sixth exact startup contract. Runtime masking, GPU passes and
unknown-build rejection remain unchanged. Run the complete matrix with all six
DLLs, including this user-supplied fixture. The older deployment and validation
entries below describe previous builds and their validation scope.
Lee's live mask coverage, ghosting and AMD frame times still require verification.

Local validation passed: Release plugin/test build, all six actual DLL mappings
and twelve draw-instruction fixtures, changed-byte rejection and complete rollback,
and both-eye WARP/local NVIDIA hardware mask/depth/state tests. All six original
DLL fixture hashes remained unchanged. These are adapter and synthetic GPU tests,
not execution of Lee's complete CSX rendering pipeline or live AMD qualification.

## Treatid follow-up update

Deployment: both local OCU MO2 folders and the Nexus staging folder/4.3.1 ZIP,
6 September 2026. Settings and shader-provider DLLs remain unchanged. This is
local packaging, not an uploaded Nexus release or a live AMD qualification.

The previously deployed three-contract build rejected both currently published
Treatid release DLLs. The label 3.19 was again insufficient. This follow-up adds
the two exact contracts below, retaining all three existing ones:

| Release asset | DLL SHA-256 | Owner RVA / size | Accepted draw RVAs |
| --- | --- | --- | --- |
| v3.19.0-pr20 / CSX_AIO-2026-09-04T23-29Z.7z | D198742E0694E03BCB0530DA5F86E8B6757FE0B0DB9FBEA3421D4924C2F7C177 | 10EDB0 / 386 hex | 10EE1C, 10F0F2 |
| v3.19.0-pr21 / CSX_AIO-2026-09-05T00-07Z.7z | 5C3F56BA0AA611AA8079AA4499DC8C6629F83689434350BCD11A0F8E0E203C66 | 10E5C0 / 386 hex | 10E62C, 10E902 |

Full disassembly verified original argument setup, the no-bridge fast path,
the suppression predicate skipping the final draw, and the 0xDBDDF9 engine
caller marker. Whole-function hash verification remains mandatory. No wildcard
instruction scan, version-only acceptance, D3D vtable hook, or extra GPU pass
was introduced. Unknown owners report the address relative to CSX and explicitly
state that body/held-item correction is inactive while world correction remains.

Release validation should invoke `tests/RunCsxCompatibilityMatrix.ps1` in the
SKSE plugin source with `-TestExecutable` pointing to DapaCsxDrawTest.exe and
`-DllPaths` supplying the five retained binaries. Its `--matrix` mode requires
every supported contract exactly once and checks that fixture files are unchanged.
Do not count a successful single-DLL run as a complete compatibility result.
No automatic downloads or mod updates are performed by the runner.

Validation completed: Release build; all five actual DLL mappings and ten draw
instruction fixtures; every-byte mutation rejection; complete code restoration;
both-eye WARP/hardware mask tests with normal/reversed biased D24/D32 depth.
Negative tests verified rejection of missing and duplicate matrix fixtures.
Hardware tests ran on the local NVIDIA GPU, not AMD. No live Skyrim run with
Treatid's build has been performed. Lee's frame-time regression remains undiagnosed.

Long-term compatibility requires a versioned accepted-draw notification contract
with the shader providers so OCU need not patch implementation addresses. This
candidate does not pretend that interface already exists, and no upstream change
or message has been made. Newly rebuilt DLLs still need independent qualification.

## Cause and scope

The original adapter accepted only the Paintball test DLL's owner at RVA
0x105BD0. Regular CSX DLLs expose the same VR menu direct-draw hook at other
addresses. If that owner is chained at Skyrim's 0xDBDDF3 mesh call, rejecting it
aborts body-mask installation, leaving world correction unchanged.

Version strings cannot identify the contract: the regular and Paintball DLLs
both report 3.19.0.0. Startup now selects one of three independently verified
contracts, matching the owner address AND SHA-256 of its entire function.

| Actual binary | DLL SHA-256 | Owner RVA / size | Accepted draw RVAs |
| --- | --- | --- | --- |
| CSX 3.18.0.0 | BCDECB9906E1CB443726BF3ACB2EA8D9DA577DA9105CC06CE1A45C10CD7BA971 | E01A0 / 356 hex | E020C, E04B2 |
| Regular CSX 3.19.0.0 | 04CBC257643630798BAC9534C176A1A1B9074286AAC03B5D380809902975B9EA | F5460 / 384 hex | F54CC, F57A0 |
| Paintball CSX 3.19.0.0 | C192935E9A509835C6B71A562D48614D1DD23FA077E8F2622BBE051BDC95AFA5 | 105BD0 / 384 hex | 105C3C, 105F10 |

These are exact binaries, not a promise to support every build with those
version strings. Each has a fast-path native DrawIndexedInstanced and a second
native draw after the suppression predicate. Disassembly confirms the original
six-argument ABI at both calls, the suppressed path skipping the second draw,
and the bridge's engine caller marker 0xDBDDF9. The callbacks observe only those
accepted draws; no suppression predicate or original owner entry is replaced.

Full-function validation and contract selection occur only at startup. The
per-frame callback, ownership classification, mask replay and warp are unchanged.
The startup log names the selected contract even with verbose logging off.

## Verification

- Release SKSE plugin and DapaCsxDrawTest built successfully. The existing
  Main.cpp NiTArray size-conversion warning remains unrelated to this change.
- Actual DLL mappings (no entry-point/import execution): all three contracts
  selected correctly, both sites installed, complete function bytes restored.
- Unknown owner offsets, wrong contracts, and a mutation at every byte of each
  owner function were rejected.
- All six instruction fixtures passed six-argument ABI, owned/unowned draw and
  rollback tests. Existing engine tests passed all 17 sites and owner chaining.
- WARP and hardware GPU tests passed both eyes, suppression/redirect isolation,
  scene colour/state preservation, normal/reversed D24S8/D32 and biased depth.
- HIGGS and SpellWheel ownership regression tests passed.

## Remaining live verification

Deployed on 6 September to both local OCU MO2 folders and the Nexus staging
folder/ZIP, with shipping capture disabled and settings preserved. Not yet
tested in Skyrim with these older binaries. For each build,
check the named adapter startup line and increasing player mask coverage, then
test strafing/stick-turning with body, equipped geometry, HIGGS objects and
SpellWheel. Check CSX menu capture/suppression remains correct. Unknown owners
continue to fail closed; do not remove validation to accommodate another build.

Open Shaders 2.11.0 was separately inspected from Alandtse's published
CommunityShaders-2026-09-04T04-49Z.7z. Its DLL hash is
C81DDF331E03FDEF5753D9703D04AE50B467720C97D2BC6476F63EDDC970B458.
Its Upscaling.cpp at that tag does not contain this CSX direct-draw hook, and the
binary does not contain the same two-call candidate pattern. That is NOT an
end-to-end compatibility result: no speculative Open Shaders adapter was added.
An unchained engine draw uses the existing engine path; any other owner still
requires separate verification.

No CSX, HIGGS or SpellWheel binaries or MO2 settings were changed. Deployment
includes the logging cleanup, and the Nexus ZIP was verified against every
staged file. Git publication was separately authorized by the user; this does
not constitute an uploaded Nexus release or a live headset compatibility test.
