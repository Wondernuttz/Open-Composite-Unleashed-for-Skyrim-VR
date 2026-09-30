# OCU Laser Integration Exception

This exception is effective for the Covered Laser Code distributed with
this notice.

## Additional permission under GNU GPL version 3, section 7

For the Covered Laser Code defined below, the approving copyright holders
grant the following additional permission, to the extent of the copyrights
they own or are authorized to license:

You may use, reproduce, modify, incorporate, link, combine, and distribute
the Covered Laser Code, in source or object form, under terms of your
choice, including proprietary terms, solely as part of a Qualifying OCU
Integration as defined below. For that qualifying use, these approving
copyright holders do not require the integrating software to be licensed
under the GNU GPL or its source code to be disclosed solely because it
contains, modifies, links to, or uses the Covered Laser Code.

A "Qualifying OCU Integration" is software in which the Covered Laser Code
is used exclusively to enable actual interoperability with Open Composite
Unleashed (OCU). The functionality using that code must exchange input,
output, or control with OCU, directly or through an adapter, and must
require OCU to provide that integrated functionality. Examples include
using OCU laser input to interact with a third-party user interface, or
adapting the covered laser rendering and hit-testing code for an OCU
integration. Development and testing of that integration, including test
harnesses that simulate OCU, are also permitted under this exception.

The integrating project may have other functionality that operates without
OCU, provided that functionality does not use the Covered Laser Code under
this exception. Merely bundling OCU, mentioning compatibility, or adding an
unrelated or nominal OCU connection does not qualify otherwise independent
reuse of the Covered Laser Code. An optional OCU mode does not extend this
permission to non-OCU modes that also use the Covered Laser Code.

Standalone reuse, extraction into a general-purpose laser library, and use
in a separate project without the qualifying OCU integration receive no
additional permission under this exception. Those uses remain governed by
the applicable GNU GPL terms unless separately licensed. This condition
limits only the additional permission; it does not restrict any rights
already available under the GNU GPL, including private use and modification.

Qualifying integration may use static linking, dynamic linking, callbacks,
or other interoperability mechanisms. This exception does not itself grant
permission to copy or link other portions of OCU under non-GPL terms.
Redistributors must preserve the limits of this exception and may not
grant broader non-GPL rights to the Covered Laser Code under it.

Copies or substantial portions of the Covered Laser Code must retain the
applicable copyright notices and this permission, including its scope,
limitations, and disclaimer. For object-only distributions, these notices
may be supplied in accompanying license documentation. Preserve any
separate third-party notices and comply with their licenses.

This permission does not relicense Open Composite Unleashed as a whole.
All portions outside the Covered Laser Code remain subject to their
existing licenses. No permission is granted on behalf of a copyright
holder who has not authorized it. Dependencies, included headers, linked
libraries, and upstream code do not become covered merely because the
Covered Laser Code uses them.

This notice does not revoke rights validly granted under a separate or
previous license.

THE COVERED LASER CODE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE APPROVING COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE COVERED LASER
CODE OR ITS USE OR OTHER DEALINGS IN IT.

## Scope

Paths are relative to the repository root. The Covered Laser Code
is the material controlled by the approving copyright holders in these
files, in the source distribution accompanying this notice:

- `OpenCompositeSkyrimVR/OpenOVR/Misc/Keyboard/VRMenuLaser.h`
- `OpenCompositeSkyrimVR/OpenOVR/Misc/Keyboard/VRMenuLaser.cpp`
- `OpenCompositeSkyrimVR/OpenOVR/Misc/Keyboard/LaserRaySmoothing.h`
- `OpenCompositeSkyrimVR/OpenOVR/Misc/Keyboard/LaserRaySmoothing.cpp`
- `OpenCompositeSkyrimVR/OpenOVR/Misc/Keyboard/LaserTextureAtlas.h`
- `OpenCompositeSkyrimVR/OpenOVR/Misc/Keyboard/LaserTextureAtlas.cpp`
- `OpenCompositeSkyrimVR/OpenOVR/Misc/Keyboard/LaserDotTexture.h`
- `OpenCompositeSkyrimVR/OpenOVR/Misc/LaserCalibration.h`
- `OpenCompositeInput- Skyrim SKSE/OpenCompositeInput/src/LaserMenuHit.inl`
- `OpenCompositeInput- Skyrim SKSE/OpenCompositeInput/src/MCMLaser.inl`

This list does not include the entire SKSE plugin, `Main.cpp`, the keyboard
implementation, OpenComposite's input/runtime implementation, or unrelated
rendering and foveation APIs. Calling or including one of those components
does not extend this exception to it. Any additional interface or source
section must be explicitly identified and approved before being covered.

The listed files include laser rendering, calibration, smoothing, textures,
and menu hit-testing adapters. Integrations with third-party user interfaces
qualify when they meet the OCU integration requirements above. Third-party
UI libraries and their API headers retain their own applicable licenses;
this exception grants no rights on their copyright holders' behalf.
