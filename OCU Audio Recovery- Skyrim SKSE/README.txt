OCU Audio Recovery - experimental Skyrim VR SKSE component

This component is not included in the current OCU test packages. It remains
isolated from OpenCompositeInput and the OpenXR runtime and disabled by default.
The endpoint watcher and diagnostic logging are retained,
but automatic recovery does not call Skyrim's audio manager: the synchronous
XAudio reset tested after Virtual Desktop disconnect can deadlock Skyrim VR.

Files:
  Data\SKSE\Plugins\OCUAudioRecovery.dll
  Data\SKSE\Plugins\OCUAudioRecovery.ini

Requirements:
  Skyrim VR 1.4.15
  SKSEVR
  VR Address Library

The Configurator intentionally leaves "Recover audio after headset reconnect"
disabled until a non-blocking XAudio recovery path has been verified. No OCU
input, rendering, keyboard, or controller code is modified by this component.
