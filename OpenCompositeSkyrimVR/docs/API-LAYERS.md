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
        "description": "What it does"
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

This is exactly the standard OpenXR explicit-layer format — the manifest schema Khronos defines,
parsed by the OpenXR loader. OCU invents no keys of its own and reads only `name` and
`library_path`, for its own checks and logging. A manifest that satisfies the loader satisfies OCU,
and one carrying keys OCU does not know about is used as it stands rather than rejected.

One manifest per layer. If two manifests in `xrlayers/` carry the same `name`, OCU accepts the
first and logs the one it skipped: the loader would otherwise put your layer in the chain twice,
as two mapped copies sharing one set of globals, the second quietly breaking the first.

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

**Read this before you design around chain position: OCU cannot control it, and there is no setting
for it.**

The reason is in the OpenXR loader rather than in OCU:

- `xrCreateInstance` takes a list of layer names. The loader uses that list purely as a yes/no
  membership test — for each manifest it discovered, "did the application name this one?"
- It then builds the chain in the order it discovered the manifest *files*, which is the order the
  filesystem listed the folder in. The order the application gave is never read.

So the thing that decides where your layer sits is **the filename of your manifest**, because that
is what the folder listing sorts on. `10-yours.json` will sit nearer the game than `20-theirs.json`.

That is an observation, not a promise. Nothing in the OpenXR specification says the loader orders by
discovery, and nothing says a directory listing is sorted at all — it is true of the current loader
on NTFS, which is every install this has been run on. Do not build anything that breaks if it stops
being true.

OCU adds nothing on top: it does not rename your manifest, reorder anything, or read any key that
would let you ask. What it does do is print the order you are actually going to get, game-first,
whenever more than one layer is installed. Look for `the chain will be, game first` in the OCU log.

With one layer installed — the normal case — none of this matters. With two that genuinely must be
ordered relative to each other, the two authors have to agree on their filenames between themselves;
there is no mechanism here that will do it for them.

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

OCU exports one function for layers:

```c
typedef void(__cdecl* PFN_OCU_InvalidateSwapchains)(void);

// Ask every loaded module, rather than naming one. OCU is built as vrclient_x64.dll and is only
// called openvr_api.dll because of where it gets deployed, so a fixed name is the fragile way.
PFN_OCU_InvalidateSwapchains FindOcuInvalidate()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
        return NULL;

    PFN_OCU_InvalidateSwapchains fn = NULL;
    MODULEENTRY32W me = { sizeof(me) };
    if (Module32FirstW(snap, &me)) {
        do {
            fn = (PFN_OCU_InvalidateSwapchains)GetProcAddress(me.hModule, "OCU_InvalidateSwapchains");
        } while (!fn && Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return fn;   // NULL is fine — see below
}
```

Resolve it once and cache the result; do not walk the module list per frame.

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

Everything fails soft — a broken layer cannot stop the game starting. Two separate mechanisms,
because a manifest and a DLL fail at different moments:

- **A bad manifest** is rejected during discovery, before OCU names anything. The log names your
  file and the reason: bad JSON, missing `name` or `library_path`, a bare filename, a DLL that
  doesn't exist, a path outside the game folder, or a duplicate `name`.
- **A DLL that won't load** cannot be caught by any check. The loader does not open it until
  `xrCreateInstance`, and there it can fail for reasons a manifest can't reveal — a 32-bit build, a
  truncated download, or a dependency you forgot to static-link. If that happens OCU logs it,
  retries once with every discovered layer dropped, and carries on. The game starts; your layer
  simply isn't there. `enableApiLayers=false` in `opencomposite.ini` stops OCU trying at all.

Either way the OCU log is the place to look — every line starts `API layers:`.

Two environment traps worth knowing:

- The OpenXR loader ignores `XR_API_LAYER_PATH` in an **elevated process**. If the game or the mod
  manager runs as administrator, nothing is discovered. The symptom is manifests listed in the log
  but no layer enabled.
- Setting that variable **switches off the loader's registry search for explicit layers.** Any
  system-installed explicit layer is invisible to the game while OCU has set it. OCU only sets it
  once at least one manifest has parsed successfully, so a folder of nothing but broken manifests
  does not cost you your system layers — but one working layer does.
