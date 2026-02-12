# OpenComposite Configurator

A graphical settings editor for the Open Composite XR mod. This tool lets you configure `opencomposite.ini` without manually editing text files. It works with both **Skyrim VR** and **Fallout 4 VR**.

The app is fully self-contained — just run the EXE. No additional software or runtimes need to be installed.

---

## How to Use

### Step 1: Launch the App

Double-click `OpenCompositeConfigurator.exe`. A dark-themed settings window will appear.

### Step 2: Select Your Game

Use the **Game** dropdown in the top-left corner to choose either **Skyrim VR** or **Fallout 4 VR**. This matters because Skyrim VR has additional settings that Fallout 4 VR does not use. When you switch to Fallout 4 VR, the Skyrim-only section will disappear.

### Step 3: Browse to Your INI Folder

Click the **Browse** button and navigate to the folder where your `opencomposite.ini` file lives (or where you want to create one).

- **If you use Mod Organizer 2 (MO2):** Browse to your mod's `root\` folder. For example: `C:\SkyrimVRmods\mods\Open Composite XR\root`
- **If you installed the mod manually:** Browse to your game's main directory. For example: `C:\Program Files (x86)\Steam\steamapps\common\SkyrimVR`

The tool will load any existing settings from `opencomposite.ini` in that folder. If no INI file exists yet, it will use sensible defaults.

### Step 4: Change Settings

Adjust whatever settings you want (see the full breakdown below).

### Step 5: Save

Click the green **Save INI** button at the bottom. The settings are written to `opencomposite.ini` in the folder you selected. The status bar will confirm where the file was saved.

If you ever want to discard your changes and reload from the file, click **Reload**.

---

## Settings Explained

### VR Keyboard Shortcut

Open Composite XR includes a VR keyboard that can inject real Windows keystrokes into the game. This is how you type console commands (press tilde ~ to open the console, then type commands), name your character, or name items at a workbench in Fallout 4 VR.

The keyboard shortcut lets you open this keyboard at any time during gameplay by pressing a button or combination of buttons on your VR controllers.

#### Enable Controller Shortcut

**What it does:** Turns the entire keyboard shortcut feature on or off.

When enabled, you can summon the VR keyboard by pressing the configured button(s) in the configured way (double tap, hold, etc.). When disabled, the keyboard can only be opened when the game itself requests text input (like the character naming screen).

**Default:** Enabled

#### Button Selection

**What it does:** Choose which physical button(s) on your controllers activate the keyboard shortcut.

You can select buttons in two ways:
- **Click directly on the controller image** — click on the button you want on the picture and it will highlight with a yellow circle
- **Check the boxes** to the right of the image under Left Hand / Right Hand columns

**Available buttons (per hand):**
- **Stick Click** — pressing down on the thumbstick (like clicking a mouse button, but on the stick)
- **X / Y Buttons** (left controller) — the two face buttons on the left controller
- **A / B Buttons** (right controller) — the two face buttons on the right controller
- **Grip** — the side squeeze button on each controller
- **Trigger** — the index finger trigger on each controller

**Button combinations:** You can select MORE than one button. When multiple buttons are selected, you must press ALL of them at the same time to trigger the shortcut. For example, selecting both "Left Grip" and "Right Grip" means you squeeze both grips simultaneously.

This is useful for preventing accidental activations — a two-button combo is much harder to trigger by accident than a single button.

**Default:** Left Stick Click

#### Tap Count

**What it does:** Controls HOW you press the button(s) to activate the shortcut.

- **x1 Hold** — Press and HOLD the button(s) for the configured duration. Good for preventing accidental triggers since you have to intentionally hold the button down.
- **x2** — Double-tap the button(s) quickly (like double-clicking a mouse). This is the default and works well for most people.
- **x3** — Triple-tap the button(s). Even less likely to trigger accidentally, but takes longer to activate.
- **x4** — Quadruple-tap. Maximum protection against accidental triggers.

**Default:** x2 (double tap)

#### Timing (ms)

**What it does:** Controls the speed/timing of the button press, measured in milliseconds.

- **For tap modes (x2, x3, x4):** This is the maximum time allowed between taps. If you tap too slowly (longer than this value between taps), it resets and doesn't count. Lower values = you have to tap faster. Higher values = more forgiving timing.
- **For hold mode (x1):** This is the minimum time you must hold the button(s) down before it activates. Lower values = activates sooner. Higher values = you have to hold longer.

**Default:** 500ms (half a second)

**Recommended range:** 300-800ms. If you find the shortcut triggers accidentally, increase the timing or switch to a higher tap count. If it feels unresponsive, decrease the timing.

---

### General Settings

These settings apply to both Skyrim VR and Fallout 4 VR.

#### Supersampling

**What it does:** Multiplies the internal rendering resolution of the game. This is the single biggest quality vs. performance knob.

- **1.0** = Native headset resolution (default). The game renders at exactly the resolution your headset expects.
- **Higher than 1.0** (e.g. 1.5) = Supersampling. The game renders at a higher resolution and then downscales to your headset. Makes everything look sharper, reduces aliasing (jagged edges), and text becomes much easier to read. However, this is very GPU-intensive — going from 1.0 to 1.5 roughly doubles the number of pixels rendered.
- **Lower than 1.0** (e.g. 0.7) = Undersampling. The game renders at a lower resolution and upscales. Everything looks blurrier but performance improves significantly. Use this if you're struggling to hit a stable framerate.

**Default:** 1.0

**Note:** This stacks with any supersampling set in your VR runtime (SteamVR, Oculus, etc.). If you set 1.5 here AND 1.5 in SteamVR, you're effectively rendering at 2.25x — which will destroy your framerate. Set one or the other, not both.

#### Render Custom Hands

**What it does:** Controls whether Open Composite draws its own 3D controller models in the game.

- **Enabled:** Open Composite renders grey controller models so you can see where your hands are.
- **Disabled:** No controller models are drawn by Open Composite.

**When to disable:** If you use **VRIK** (which gives you a full body and visible hands) or any other mod that provides its own hand/controller models, you should disable this. Having two sets of hand models overlapping looks bad.

**Default:** Enabled

#### Enable Haptics

**What it does:** Controls whether the game can make your controllers vibrate.

VR games use vibration (haptic feedback) to simulate things like weapon impacts, picking up objects, and UI interactions. Disabling this turns off all vibration.

**When to disable:** If vibration annoys you, drains your controller battery too fast, or if a specific game sends overly aggressive vibration that feels uncomfortable.

**Default:** Enabled

#### Haptic Strength

**What it does:** Controls how strong the vibration is, on a scale from 0.00 (off) to 1.00 (maximum).

**Default:** 0.10 (very gentle)

**Tip:** The default of 0.10 is quite subtle. If you can barely feel vibrations, try increasing this to 0.3-0.5. If vibrations feel too aggressive, lower it.

#### Enable Hidden Mesh Fix

**What it does:** Uses the headset's "hidden area mesh" to skip rendering pixels that you'll never see (the corners of each eye that are outside the lens's visible area).

This is a free performance optimization — it reduces GPU work without any visual difference because those pixels were invisible anyway. There is essentially no reason to disable this unless you experience a specific visual glitch.

**Default:** Enabled

#### Invert Using Shaders

**What it does:** Uses a shader pass to flip the rendered image vertically.

Some combinations of graphics API and headset can result in the game appearing upside-down. If your game looks upside-down, enable this. Otherwise, leave it off.

**Default:** Disabled

#### DX10 Mode

**What it does:** Forces the game to use DirectX 10 instead of DirectX 11.

This is a compatibility fallback. Only enable this if the game crashes on startup and you've exhausted other troubleshooting options. DX10 mode may disable some visual features.

**Default:** Disabled

---

### Skyrim VR Settings

These settings only appear when **Skyrim VR** is selected. They are specific to how Skyrim VR handles input, controller tracking, and certain mod compatibility features.

#### Enable Input Smoothing

**What it does:** Smooths out thumbstick movement input by averaging multiple frames of stick position data.

Without smoothing, thumbstick input is read directly each frame, which can feel twitchy or jerky — especially at lower framerates or with controllers that have slightly noisy analog sticks. With smoothing enabled, the game averages several frames of stick data together, producing smoother movement.

**Trade-off:** Smoother movement at the cost of slightly more input lag (delay between moving the stick and seeing the result in-game).

**Default:** Disabled

#### Smoothing Window

**What it does:** Controls how many frames of thumbstick input are averaged together when input smoothing is enabled. Only relevant if "Enable input smoothing" is checked.

- **Low values (1-3):** Minimal smoothing, minimal lag. Feels responsive but may still be slightly twitchy.
- **Medium values (4-7):** Good balance of smoothness and responsiveness. **Recommended starting point: 5.**
- **High values (8+):** Very smooth but noticeably laggy. Movement will feel sluggish and floaty.

**Default:** 5

#### Enable Controller Smoothing

**What it does:** Smooths the physical position and rotation tracking of your controllers using a "One Euro" noise filter.

This is different from input smoothing — this affects where your HANDS appear in 3D space, not thumbstick movement. If your virtual hands shake or jitter (common in some tracking environments with reflective surfaces or poor lighting), enabling this can stabilize them.

**Trade-off:** Reduces hand jitter but adds a tiny amount of tracking latency. Most people won't notice the latency.

**Default:** Disabled

#### Disable Trigger Touch Events

**What it does:** Ignores "trigger touch" events — the signal sent when your finger lightly rests on the trigger without actually pulling it.

Touch controllers (Quest, Rift) have capacitive sensors that detect when your finger is touching the trigger even if you're not pressing it. Some games interpret this as an input, which can cause unintended actions (like weapons being semi-drawn, or UI elements activating when you don't want them to).

**When to enable:** If you notice the game reacting to your trigger finger when you're not intentionally pressing it.

**Default:** Disabled

#### Disable Trackpad Emulation

**What it does:** Stops Open Composite from emulating a trackpad using the thumbstick.

Some older VR games were designed for Vive wand controllers that have a large circular trackpad. Open Composite can emulate that trackpad using the thumbstick on Touch/Quest controllers. If this emulation causes input problems (wrong movement, menu navigation issues), disabling it may help.

**Default:** Disabled

#### VRIK Knuckles Trackpad Support

**What it does:** Enables a compatibility mode for using VRIK (the full-body mod for Skyrim VR) with Valve Index / Knuckles controllers.

Index controllers have both a thumbstick and a small trackpad. VRIK uses the trackpad for certain gestures. This setting ensures the trackpad data is passed through correctly.

**When to enable:** Only if you use VRIK with Index/Knuckles controllers AND experience issues with VRIK hand poses or gestures.

**Default:** Disabled

#### Left / Right Dead Zone

**What it does:** Sets a dead zone radius for each thumbstick, on a scale from 0.00 (no dead zone) to 1.00 (entire stick is dead zone).

A dead zone is a small area around the center of the thumbstick where movement is ignored. This prevents "stick drift" — where the game thinks you're pushing the stick slightly even when you're not touching it. Stick drift causes your character to slowly walk on their own.

- **0.00:** No dead zone. Any tiny stick movement registers as input. You may experience drift.
- **0.05-0.15:** Small dead zone. Fixes minor drift while keeping the stick very responsive.
- **0.20-0.30:** Moderate dead zone. Fixes noticeable drift but you'll have to push the stick further before movement starts.

**Default:** 0.00

**Tip:** If your character slowly walks forward/sideways on their own when you're not touching the sticks, increase the dead zone for the affected stick. Start at 0.10 and increase until the drift stops.

#### Keyboard Default Text

**What it does:** Pre-fills the VR keyboard with this text whenever it opens for text input (like the character naming screen).

If you leave this blank, the keyboard opens with an empty text field and you can type whatever you want. If you set it to something (e.g. "Dovahkiin"), that text will appear pre-filled every time the keyboard opens.

**Default:** Empty (blank)

---

## INI File Format

The configurator reads and writes a standard `opencomposite.ini` file. Here's what the file looks like:

```ini
supersampleRatio=1.0
renderCustomHands=true
enableInputSmoothing=true
inputWindowSize=3
disableTriggerTouch=false

[keyboard]
shortcutEnabled=true
shortcutButton=left_grip+right_grip
shortcutMode=double_tap
shortcutTiming=500
```

Settings at the top of the file (before any `[section]` header) are general settings. The `[keyboard]` section contains the VR keyboard shortcut settings.

**The configurator preserves comments and settings it doesn't recognize.** If you've manually added custom settings or comments to your INI file, they won't be deleted when you save through the configurator.

---

## Troubleshooting

**"The app won't start"** — This is a self-contained application that should work on any 64-bit Windows 10 or Windows 11 system. If Windows SmartScreen blocks it, click "More info" then "Run anyway" — the app is not signed but is safe.

**"I saved but nothing changed in-game"** — Settings are only read when the game starts. You need to fully close and relaunch the game for INI changes to take effect.

**"I don't see my opencomposite.ini"** — If you browsed to a folder and the status says "No opencomposite.ini found", that's fine — the configurator will create one when you click Save. Just make sure you browsed to the correct folder (the one containing `openvr_api.dll`).

**"I use MO2 and changes aren't applying"** — Make sure you're editing the INI in the mod's `root\` folder, NOT in the MO2 overwrite folder or the game directory. The `root\` folder is what MO2 deploys to the game directory at runtime.

**"My game is upside-down"** — Enable "Invert using shaders" in General Settings.

**"My character slowly walks on their own"** — You have thumbstick drift. Increase the dead zone for the affected stick (left or right) under Skyrim VR Settings. Start with 0.10.

**"The keyboard shortcut keeps triggering accidentally"** — Use a button combination (select two or more buttons), increase the tap count to x3 or x4, or increase the timing value.
