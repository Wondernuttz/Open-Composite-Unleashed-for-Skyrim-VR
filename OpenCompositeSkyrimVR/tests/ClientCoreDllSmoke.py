"""Test a copied OCU DLL without VR initialization; preserve the real OCU log."""
from pathlib import Path
import ctypes, json, os, shutil, subprocess, sys, tempfile, threading, uuid
from concurrent.futures import ThreadPoolExecutor


def game_is_running():
    # Query process names only. A failed query refuses the test/restoration.
    result = subprocess.run([
        'powershell.exe', '-NoProfile', '-NonInteractive', '-Command',
        "$ErrorActionPreference = 'Stop'; @(Get-Process | Where-Object { $_.ProcessName -in @('SkyrimVR', 'Fallout4VR') }).Count"
    ], check=True, capture_output=True, text=True)
    return int(result.stdout.strip()) != 0


def production_log_path():
    # Match logging.cpp's actual Windows known folder, including redirection.
    shell = ctypes.WinDLL('shell32')
    ole = ctypes.WinDLL('ole32')
    shell.SHGetKnownFolderPath.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p,
                                         ctypes.POINTER(ctypes.c_void_p)]
    shell.SHGetKnownFolderPath.restype = ctypes.c_long
    ole.CoTaskMemFree.argtypes = [ctypes.c_void_p]
    folder = ctypes.create_string_buffer(uuid.UUID('FDD39AD0-238F-46AF-ADB4-6C85480369C7').bytes_le)
    result = ctypes.c_void_p()
    status = shell.SHGetKnownFolderPath(folder, 0, None, ctypes.byref(result))
    if status < 0 or not result.value:
        raise RuntimeError(f'Cannot safely resolve the production Documents log: HRESULT {status:#x}')
    try:
        return Path(ctypes.wstring_at(result.value)) / 'My Games' / 'Skyrim VR' / 'SKSE' / 'OCUnleashedSKSE.log'
    finally:
        ole.CoTaskMemFree(result)


def run_smoke_child(fixture):
    os.chdir(fixture)
    config = fixture / 'apps-config.json'
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
    kernel.GetModuleHandleW.restype = ctypes.c_void_p
    assert not kernel.GetModuleHandleW('vrclient_x64.dll'), 'Unexpected preloaded SteamVR client'
    dll = ctypes.CDLL(str(fixture / 'openvr_api.dll'))
    factory = dll.VRClientCoreFactory
    factory.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)]
    factory.restype = ctypes.c_void_p
    def request(version):
        status = ctypes.c_int(-1)
        result = factory(version, ctypes.byref(status))
        assert result and status.value == 0
        return result

    callers = 32
    start = threading.Barrier(callers)
    def first_request(_):
        start.wait(timeout=10)
        return request(b'IVRClientCore_003')

    with ThreadPoolExecutor(max_workers=callers) as executor:
        initial = list(executor.map(first_request, range(callers)))
    original = initial[0]
    assert all(result == original for result in initial), 'Concurrent factory calls returned different OCU cores'
    config.write_text(json.dumps({'***GlobalConfig': {'default_runtime': 2}}))
    expected = {b'IVRClientCore_003': original, b'IVRClientCore_002': request(b'IVRClientCore_002')}
    versions = list(expected) * 256
    with ThreadPoolExecutor(max_workers=callers) as executor:
        results = list(executor.map(request, versions))
    assert all(result == expected[version] for version, result in zip(versions, results)), 'Late concurrent calls changed OCU core'
    assert not kernel.GetModuleHandleW('vrclient_x64.dll'), 'Factory loaded SteamVR in an OCU process'
    print('PASS: actual built OCU DLL served 32 concurrent first requests and 513 requests after an isolated preference change to SteamVR; both client core versions remained stable, no native SteamVR client loaded, no VR/OpenXR initialization performed.')


def run_preserving_log(dll_source, fixture):
    if game_is_running():
        raise RuntimeError('Close Skyrim/Fallout 4 VR before the smoke test; its live log must not be in use.')
    live_log = production_log_path()
    assert not fixture.exists(), 'Use a fresh isolated directory'
    fixture.mkdir(parents=True)
    shutil.copy2(dll_source, fixture / 'openvr_api.dll')
    (fixture / 'opencomposite.ini').write_text('enableAudioSwitch=false\n')
    (fixture / 'apps-config.json').write_text(json.dumps({'***GlobalConfig': {'default_runtime': 1}}))
    backup = fixture / 'OCUnleashedSKSE.pre-smoke.log'
    original_present = live_log.exists()
    if original_present:
        shutil.copy2(live_log, backup)
    if game_is_running():
        raise RuntimeError(f'A game started before the test. No DLL loaded; backup retained: {backup}')
    try:
        # Child exit (including failure/timeout) closes the DLL logger before
        # the finally block reads or restores the user's log.
        result = subprocess.run([sys.executable, str(Path(__file__).resolve()), '--smoke-child', str(fixture)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
        (fixture / 'smoke-output.txt').write_text(result.stdout, encoding='utf-8')
        print(result.stdout, end='')
        result.check_returncode()
    finally:
        if game_is_running():
            raise RuntimeError(f'Log restoration refused because a game is running. Original backup retained: {backup}')
        if live_log.exists():
            shutil.copy2(live_log, fixture / 'OCUnleashedSKSE.smoke.log')
        if original_present:
            restore_path = None
            try:
                # Atomic replacement avoids partially restoring the old log.
                with tempfile.NamedTemporaryFile(dir=live_log.parent, prefix='.ocu-smoke-restore-', delete=False) as temporary:
                    restore_path = Path(temporary.name)
                shutil.copy2(backup, restore_path)
                if game_is_running():
                    raise RuntimeError(f'A game started before restoration. Original backup retained: {backup}')
                os.replace(restore_path, live_log)
            finally:
                if restore_path is not None:
                    restore_path.unlink(missing_ok=True)
        elif live_log.exists():
            if game_is_running():
                raise RuntimeError('A game started before cleanup; leaving its live log alone.')
            live_log.unlink()
        print(f'Original OCU log restored ({"present" if original_present else "absent"}); smoke log retained in {fixture}.')


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--smoke-child':
        run_smoke_child(Path(sys.argv[2]).resolve())
    else:
        dll_source, fixture = (Path(value).resolve() for value in sys.argv[1:3])
        run_preserving_log(dll_source, fixture)
