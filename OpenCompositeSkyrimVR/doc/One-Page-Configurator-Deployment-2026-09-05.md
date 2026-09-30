# One-page configurator / MO2 deployment

Updated `C:\SkyrimVRmods\mods\OCU - DAPA and Eye Tracking TEST`.
The disabled older configurator mod and Nexus staging were not updated.

The installed EXE was launched from that exact folder, and Video inspected at
the current 1266 x 1025 window size. The main Video page has no vertical or
horizontal scrollbar: upscaling, DAPA, post-AA, mip bias, both foveation profiles,
eye-tracked ring rates, effective cap readout and Save are all visible together.
Existing Advanced panels remain available. Rate/size explanations use tooltips;
font sizes were not reduced to make the page fit.

The standalone test build saved and reloaded the custom values correctly. The
installed copy was inspected without changing or saving the user's settings.
It is left open on Video. Runtime deployment remains managed by MO2 Root Builder.

## Verification

- Configurator publish and runtime build: pass.
- Configurator migration/rate persistence tests: pass.
- Gaze/rate tests and 360 production density-mask shader cases: pass.
- 36 payload files SHA-256 verified; only EXE, root runtime DLL and eye-rate
  documentation differed. This report was added after verification.
- All 93 pre-existing files outside that update were verified unchanged.
- No DAPA timing, CSX binary, shader cache or live game-root changes made.
- No in-headset or actual AMD-device performance claim is made.

SHA-256:

```text
Configurator: 6BF3D877EA1B8D51797EF4E2B16983BEF3ECED1B216FAAC49A0FE6C1359A5A49
Runtime:      65242394CDF5FC596EC56EC87929DF7CD7A2169803B3416F7940F6B4D74B5989
Preserved INI: DE70D5E9147329B68717668280E4009C6BFBC7B10A1E8C2B6D8B60777CDB3074
```

Previous changed binaries and source snapshot are recoverable from:
`C:\Users\borja\Desktop\OCU Nexus Staging Archives\One-page-configurator-20260905`.
The deployment script is stored there as `deploy-mo2.ps1` (not inside the mod).
