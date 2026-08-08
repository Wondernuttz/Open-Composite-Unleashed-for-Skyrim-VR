# Writing an API layer for OCU

OCU looks for OpenXR API layers in an `xrlayers/` folder next to `openvr_api.dll` and enables each
one it finds. This is how a mod gets code in between OCU and the VR runtime without patching
either.

The whole contract is **a manifest and the DLL it names**. OCU never opens your DLL and knows
nothing about it at compile time.

Turn the whole mechanism off with `enableApiLayers=false` in `opencomposite.ini`.

## The manifest

Drop a `.json` file in `xrlayers/`:

```json
{
    "file_format_version": "1.0.0",
    "api_layer": {
        "name": "XR_APILAYER_YOURMOD_thing",
        "library_path": ".\\XR_APILAYER_YOURMOD_thing.dll",
        "api_version": "1.0",
        "implementation_version": "1",
        "description": "What it does",
        "ocu_priority": 100
    }
}
```

- **`file_format_version`** — version of the manifest format itself; use `"1.0.0"`.
- **`name`** — your layer's name, which must match the one your DLL reports to the loader. By
  convention `XR_APILAYER_<VENDOR>_<thing>`.
- **`library_path`** — where your DLL is, absolute or relative to this manifest. See the rules
  below.
- **`api_version`** — the OpenXR version your layer targets; `"1.0"` unless you need something
  newer.
- **`implementation_version`** — your own build number, for your own use.
- **`description`** — a short human-readable line; it shows up in the OCU log.
- **`ocu_priority`** — optional, an integer that decides where you sit in the chain. Defaults to
  `100`. See [Ordering](#ordering).

Everything except `ocu_priority` is the standard OpenXR explicit-layer format, and the OpenXR
loader is what parses it. OCU reads only `name`, `library_path` and `ocu_priority`.

### Where library_path may point

Absolute, or relative to the manifest. Two extra rules:

- **It must be inside the game folder** — the folder holding the running `.exe`, or below it. A
  path that climbs out with `..\`, or an absolute path somewhere else, is skipped and logged. This
  keeps your layer inside the mod manager's view and makes uninstalling complete.
- **It must be a path, not a bare filename.** `"thing.dll"` sends the loader off to the normal
  Windows DLL search, which reaches outside the game folder. Write `".\\thing.dll"`.

Your DLL does **not** have to sit in `xrlayers/`. Anywhere under the game folder works, which is
what makes the SKSE trick below possible.

## The DLL

Export `xrNegotiateLoaderApiLayerInterface`. Check the `XrNegotiateLoaderInfo` and
`XrNegotiateApiLayerRequest` structs the loader gives you — reject sizes or versions you don't
recognise — then fill in `layerInterfaceVersion`, `layerApiVersion`, `getInstanceProcAddr` and
`createApiLayerInstance`.

Your `xrCreateApiLayerInstance` receives an `XrApiLayerCreateInfo` carrying the next layer's
`xrGetInstanceProcAddr`. Resolve everything you want to intercept from it up front, and pass
through everything you don't.

Khronos' `XR_APILAYER_LUNARG_api_dump` is the smallest complete example in the wild.

## Ordering

Layers are ordered by `ocu_priority`, lowest first. **Low sits nearer the game, high sits nearer
the runtime.** The default is `100`, so a layer that says nothing lands in the middle and you can
go either side of it without editing anyone else's manifest.

| Priority | Where you land | Good for |
|---|---|---|
| `0`–`50` | Nearest the game | Seeing calls before anything else has altered them |
| `100` | Default | No particular preference |
| `150`–`200` | Nearest the runtime | Impersonating or replacing runtime behaviour |

Layers with the same priority are ordered by filename, so the result is always the same from run to
run. The OCU log lists the final order.

## Being an SKSE plugin as well

Because `library_path` may point anywhere under the game folder, one DLL can be both an OpenXR
layer and an SKSE plugin — useful when your layer needs live game state. Keep the DLL where SKSE
wants it and put only the manifest in `xrlayers/`:

```
Data\SKSE\Plugins\YourMod.dll        the only DLL
xrlayers\YourMod.json                "library_path": "..\\Data\\SKSE\\Plugins\\YourMod.dll"
```

That relative path assumes OCU sits in the game root, which it does for Skyrim VR.

The two contracts are separate exports, so nothing collides, and Windows maps the DLL once — both
halves share one set of globals with no IPC needed. SKSE loads it well before OCU starts OpenXR, so
your plugin half is already running by the time your layer negotiates.

Pitfalls, worst first:

- **Threading.** Your layer's per-frame calls run on the game's render thread, not the main game
  thread. Reading a stable object's numbers is mild; *changing* anything, touching the Papyrus VM,
  using the game's allocator, or walking structures the main thread can free (inventories,
  extra-data lists, active effects) is not. Have your game-side code publish a small plain-data
  snapshot and have the layer read the most recent one. Copy values, never pointers.
- **Don't stall the frame.** You have about 11 ms at 90 Hz, and a missed frame looks like judder.
  Short locks are fine. Nothing that does file I/O, allocates, or waits on another thread. Use
  `try_lock` and fall back to the last known good value.
- **The plugin half may never run.** If SKSE rejects your version data, or the user has no SKSE,
  the OpenXR loader still loads your DLL and your layer still runs — with no game data. It has to
  work standalone and do nothing gracefully.
- **Static-link your dependencies.** A dependency DLL next to an SKSE plugin is not found, and SKSE
  fails with error 126 and writes no log. The OpenXR loader has a fallback SKSE doesn't, so your
  layer half can load while your plugin half silently didn't.
- **Confirm you got one module, not two.** If the manifest path and SKSE's path don't resolve to
  the same file you get two mapped copies with two sets of globals, silently. Set a global in the
  plugin half and log it from `xrNegotiateLoaderApiLayerInterface` — it must already be set.
- **Unloading.** The loader calls `FreeLibrary` when the OpenXR instance is destroyed. With SKSE
  holding a reference the module stays mapped, which is what you want if you installed hooks — but
  don't rely on it if the plugin half didn't run.

## Asking OCU to rebuild its swapchains

`openvr_api.dll` exports one function for layers:

```c
typedef void(__cdecl* PFN_OCU_InvalidateSwapchains)(void);

auto fn = (PFN_OCU_InvalidateSwapchains)GetProcAddress(
    GetModuleHandleW(L"openvr_api.dll"), "OCU_InvalidateSwapchains");
if (fn)
    fn();
```

It tells OCU to throw its swapchains away and create new ones on its next frame. You need this if
you have changed what the images in those swapchains mean, because OpenXR fixes a swapchain's
images for its lifetime — the only way to give an application different ones is to make it ask
again.

A null result from `GetProcAddress` means an OCU too old to have it. That is not an error; handle
it by doing whatever you would do without OCU.

Three things to know:

- It takes effect on OCU's next frame, not immediately.
- Safe to call from any thread, and safe to call early, before OCU has finished starting.
- **It does not reach every swapchain OCU owns.** The game's eye buffers, overlays, and the ASW and
  space warp paths rebuild. The keyboard, the menu laser, and the overlay trail chain do not — so
  don't give those chains images you will need back later.

## When it doesn't load

Everything fails soft. OCU only names layers the loader actually reported, so a broken manifest
can't stop the game starting. The OCU log always says what happened — look for lines starting
`API layers:`. It will name your manifest and the reason: bad JSON, missing `name` or
`library_path`, a bare filename, a DLL that doesn't exist, or a path outside the game folder.

Two environment traps worth knowing:

- The OpenXR loader ignores `XR_API_LAYER_PATH` in an **elevated process**. If the game or the mod
  manager runs as administrator, nothing is discovered. The symptom is manifests listed in the log
  but no layer enabled.
- Setting that variable **switches off the loader's registry search for explicit layers.** Any
  system-installed explicit layer is invisible while an `xrlayers/` folder exists.
