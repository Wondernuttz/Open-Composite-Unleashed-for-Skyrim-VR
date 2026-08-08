# API layer discovery — what was tested

Covers the `xrlayers/` discovery in `DrvOpenXR::DiscoverApiLayers` and
`ReadApiLayerManifest`. Read alongside `API-LAYERS.md`.

Split into what has actually been executed and what has not, because the two are easy to conflate
and the second list is the one that matters before shipping.

## Executed

### Build

`cmake --build C:\ocub2 --config Release` — the C++ targets build clean, no new warnings. The
`RuntimeSwitcher` C# project fails on a missing Newtonsoft.Json package; that is pre-existing and
unrelated.

### Manifest parsing and validation

`tests/ApiLayerManifestSelfTest.cpp` runs the fixtures in `tests/xrlayers-fixtures/` through the
same vendored jsoncpp OCU uses, applying the same predicates in the same order as
`ReadApiLayerManifest`. Excluded from normal builds, alongside the existing
`OCURuntimeSemanticsSelfTest`:

```
cmake --build <build-dir> --config Release --target OCUApiLayerManifestSelfTest
<build-dir>\tests\Release\OCUApiLayerManifestSelfTest.exe
```

It exits non-zero on any mismatch. The production code is `static` inside `DrvOpenXR.cpp`, so the
predicates and the two containment helpers are **copied** rather than linked — if the production
code changes and this stops agreeing with it, the tables below are what need revisiting.

Every fixture behaved as its filename claims:

| Fixture | Result |
|---|---|
| `01-valid-reference` | parsed, relative path, proceeds to containment + existence |
| `02-malformed-json` | rejected — not valid JSON |
| `03-missing-fields` | rejected — missing `library_path` |
| `04-no-api-layer-node` | rejected — no `api_layer` object |
| `05-missing-dll` | parsed, then rejected at the existence check |
| `06-escapes-game-root` | parsed, then rejected by containment |
| `07-absolute-outside` | parsed, then rejected by containment |
| `08-bare-filename` | rejected — bare filename |
| `09-empty-name` | rejected — empty name |
| `10-wrong-types` | rejected — `name` is a number, `library_path` an array |
| `11-empty-file` | rejected — not valid JSON |

The point of 02, 10 and 11 is that jsoncpp does not throw on any of them, so the validation has to
be explicit. 10 in particular passes an `isNull()` check and only fails on `isString()`.

### Containment

`CanonicalPath` + `PathIsWithin` are copied verbatim into the self-test and exercised against a
synthetic game folder. All ten cases pass:

| Case | Expected | Got |
|---|---|---|
| `<root>\xrlayers\.\layer.dll` | inside | inside |
| `<root>\xrlayers\..\Data\SKSE\Plugins\mod.dll` (the SKSE hybrid layout) | inside | inside |
| deep subdirectory | inside | inside |
| lowercase drive letter, mixed case elsewhere | inside | inside |
| forward slashes | inside | inside |
| `..\..\..\Windows\System32\version.dll` | outside | outside |
| absolute `C:\Windows\System32\version.dll` | outside | outside |
| sibling with a shared prefix, `C:\Game\SkyrimVROther\x.dll` | outside | outside |
| the root itself, not a child of it | outside | outside |
| the root's parent | outside | outside |

The sibling-prefix and SKSE-hybrid rows are the two that matter. The first is the classic
string-prefix bug — without the directory-boundary check, `C:\Game\SkyrimVROther` would count as
inside `C:\Game\SkyrimVR`. The second confirms the containment rule does not break the case it was
most likely to break, since that path only lands inside the game folder after `..\` is resolved.

## Not executed

**Nothing has been run against the game.** Everything below is unverified.

- **A real launch.** The expected log for a stock install is one `manifest ... -> layer ...` line,
  one `enabling ...`, and `1 of 1 enabled`. Dropping the fixture manifests into the live
  `xrlayers/` folder alongside it would exercise every rejection path in one launch and is the
  cheapest way to confirm the log table in `API-LAYERS.md`.
- **Multiple layers in one chain.** Ordering is by sorted filename and the nesting rule was read
  out of the loader, not observed. Two real layers have never been loaded at once.
- **`enableApiLayers=false`.** The off switch has not been exercised. Low risk — it is one
  branch around the whole discovery call.
- **Mod-manager virtual paths.** `library_path` resolution and the containment check both run
  in-process, so a VFS that hooks file APIs should serve them — but `GetFullPathNameA` is pure
  string manipulation and does not touch the filesystem, so a virtual path canonicalises fine
  either way. What is untested is whether `GetFileAttributesA` and the loader's later
  `LoadLibrary` agree with it. This is the most likely place for a surprise.
- **An elevated process.** The `PlatformUtilsGetSecureEnv` path is documented from the loader
  source, never reproduced.
- **A layer whose manifest is valid but whose DLL fails to load** (missing dependency, wrong
  architecture). The loader should reject it before enumeration so OCU never names it, but that
  ordering has not been observed.

## Fixtures are inert

`tests/xrlayers-fixtures/` is not on any search path and nothing in the build reads it. The
fixtures only do anything if copied into a live `xrlayers/` folder, and `01-valid-reference.json`
names a DLL that does not exist, so even that one only reaches the existence-check rejection
unless you supply a real DLL.
