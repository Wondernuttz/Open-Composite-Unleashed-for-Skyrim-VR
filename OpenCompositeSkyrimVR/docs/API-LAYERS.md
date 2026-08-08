# Writing an API layer for OCU

OCU discovers OpenXR API layers from an `xrlayers/` folder next to `openvr_api.dll` and enables
each one it finds. This is how a mod gets code in between OCU and the runtime without patching
either. See `DrvOpenXR::DiscoverApiLayers`.

The whole contract is **a manifest and the DLL it names**. OCU never opens your DLL, never learns
what else it might be, and has no compile-time knowledge of any layer.

## The manifest

Drop a `.json` in `xrlayers/`:

```json
{
    "file_format_version": "1.0.0",
    "api_layer": {
        "name": "XR_APILAYER_YOURMOD_thing",
        "library_path": ".\\XR_APILAYER_YOURMOD_thing.dll",
        "api_version": "1.0",
        "implementation_version": "1",
        "description": "What it does"
    }
}
```

Standard OpenXR explicit-layer format — the loader parses it, not us. OCU reads only `name` and
`library_path`, to log what it found and to check the DLL before naming it.

### Where library_path may point

Absolute, or relative to the manifest. Two rules on top of the loader's:

- **It must resolve inside the game folder** — the directory holding the running `.exe`, or any
  subdirectory of it. A manifest that climbs out with `..\` or names an absolute path elsewhere is
  logged and skipped. This keeps a layer inside the mod manager's view, keeps an uninstall
  complete, and stops a stray manifest pulling in a DLL from anywhere on the machine. It's a
  sanity boundary, not a security one — a junction would defeat it, and nothing here is defending
  against a hostile manifest.
- **It must be a path, not a bare filename.** `"thing.dll"` would send the loader to the normal
  DLL search order, which reaches well outside the game folder and can't be confined. Write
  `".\\thing.dll"`.

Within those rules the DLL does **not** have to live in `xrlayers/`. It can be anywhere under the
game folder, which is what makes the next section possible.

## The DLL

Export `xrNegotiateLoaderApiLayerInterface`. Validate the `XrNegotiateLoaderInfo` and
`XrNegotiateApiLayerRequest` structs the loader hands you (sizes and versions — reject anything
you don't recognise), then fill in `layerInterfaceVersion`, `layerApiVersion`,
`getInstanceProcAddr` and `createApiLayerInstance`. `libs/openxr-sdk/src/loader/api_layer_interface.cpp`
is what will be calling you; Khronos' `XR_APILAYER_LUNARG_api_dump` is the smallest complete
example in the wild.

Your `xrCreateApiLayerInstance` gets an `XrApiLayerCreateInfo` carrying the next layer's
`xrGetInstanceProcAddr`. Resolve everything you intend to intercept from it up front and pass
through anything you don't.

## Being an SKSE plugin as well

Nothing about a layer implies a game. But because `library_path` may point anywhere under the game
folder, one DLL can be both an OpenXR layer and an SKSE plugin — useful when the layer needs live
game state. Keep the DLL where SKSE requires it and put only the manifest in `xrlayers/`:

```
Data\SKSE\Plugins\YourMod.dll        the only DLL
xrlayers\YourMod.json                "library_path": "..\\Data\\SKSE\\Plugins\\YourMod.dll"
```

That relative path assumes OCU sits in the game root, which is true for Skyrim VR. Check the OCU
log if it doesn't load.

The two contracts are disjoint exports, so nothing collides, and Windows maps the DLL once — both
halves share one set of globals with no IPC. SKSE loads it long before OCU creates its OpenXR
instance, so by the time your layer negotiates, your plugin half is already initialised. The
loader's `LoadLibrary` just bumps the refcount; `DllMain` runs once.

Pitfalls, worst first:

- **Threading.** Your layer's frame calls run on whatever thread OCU is on, which is the game's
  render path — not the main game thread. Reading a stable object's scalar fields is mild;
  *mutating* anything, touching the Papyrus VM, using the game's allocator, or walking structures
  the main thread can free (inventories, extra-data lists, active-effect and process chains) is
  not. Have your game-side code publish a small plain-data snapshot and have the layer read the
  latest one. Copy values — floats, form IDs — never pointers to objects the game may free.
- **Don't stall the frame.** The budget is ~11 ms at 90 Hz and a missed frame reads as judder.
  Short locks are fine; OCU's own layers take one on every entry point. Nothing that does I/O,
  allocates, or waits on another thread's progress. Use `try_lock` and fall back to last-known-good
  so a game-thread hitch can't stall rendering.
- **The plugin half may never run.** If SKSE rejects your version data, or the user has no SKSE,
  the OpenXR loader still loads the DLL and your layer still runs — with no game data. It has to
  work standalone and degrade to doing nothing.
- **Static-link your dependencies.** A dependency DLL sitting next to an SKSE plugin is not found,
  and SKSE fails with error 126 writing no log. The OpenXR loader has a fallback SKSE doesn't
  (`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`), so the layer half can load while the plugin half silently
  didn't — a bug report that makes no sense.
- **Confirm you got one module, not two.** If the manifest path and SKSE's path don't resolve
  identically you could get two mapped copies with two sets of globals, silently. Set a global in
  the plugin half and log it from `xrNegotiateLoaderApiLayerInterface`; given the load order it
  must be set.
- **Unloading.** The loader calls `FreeLibrary` when the instance is destroyed. With SKSE holding
  a reference the module stays mapped, which is what you want if you installed any hooks — but
  don't rely on it if the plugin half didn't run.

## Ordering

Manifests are sorted by filename. Earlier files sit **nearer the application**, later ones
**nearer the runtime** (the loader chains in reverse array order —
`libs/openxr-sdk/src/loader/loader_instance.cpp`). Prefix filenames with `10-`, `20-` if you care
where you land. A layer that impersonates a runtime wants to be last.

## When it doesn't load

Everything fails soft. OCU only ever names layers that `xrEnumerateApiLayerProperties` reported,
so a broken third-party manifest can't stop the game starting — naming a layer the loader didn't
find is the one thing that would be fatal (`XR_ERROR_API_LAYER_NOT_PRESENT` from
`xrCreateInstance`). It can't be silent either; the OCU log says which of these happened:

| Log line | Meaning |
|---|---|
| `no xrlayers folder at ...` | Nothing installed. Normal. |
| `... exists but holds no manifests` | Empty folder. |
| `manifest X -> layer 'Y' (path)` | Found, parsed, DLL present and in bounds. |
| `is not valid JSON` | Malformed or empty manifest. |
| `has no api_layer.name / api_layer.library_path` | Missing fields, or present with the wrong type. |
| `has an empty name or library_path` | Fields present but blank. |
| `gives library_path '...' as a bare filename` | Needs a path — write `.\\thing.dll`. |
| `points at ..., which is outside the game folder` | Containment check refused it. |
| `names ..., which does not exist` | Manifest shipped without its DLL. |
| `'Y' was not picked up by the loader` | Manifest was fine, loader still rejected it. Usually a bad `api_version`, or the elevation problem below. |
| `enabling 'Y'` | It's in the chain. |
| `N of M enabled` | Summary. |

Two environment traps worth knowing:

- The loader reads `XR_API_LAYER_PATH` through `PlatformUtilsGetSecureEnv`, which returns nothing
  for a **high-integrity process**. If the game or the mod manager runs elevated, nothing is
  discovered — the symptom is manifests logged but no layer enabled.
- Setting that variable **suppresses the loader's registry search for explicit layers** entirely.
  System-installed explicit layers are invisible while `xrlayers/` exists.

Turn the whole mechanism off with `enableApiLayers=false` in `opencomposite.ini`.

## Testing

`tests/xrlayers-fixtures/` holds a manifest per rejection path. See `docs/API-LAYERS-TESTING.md`.
