"""Check the complete user package independently of its packaging allowlist.

Run on the assembled folder and final ZIP before deployment. This script stays
in development tooling; it is not part of the installed mod.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct
import zipfile


REQUIRED = {
    "root/openvr_api.dll", "SKSE/Plugins/OpenCompositeInput.dll",
    "root/opencomposite.ini", "root/menu_quad_settings.ini",
    "root/nvngx_dlss.dll", "root/amd_fidelityfx_loader_dx12.dll",
    "root/amd_fidelityfx_upscaler_dx12.dll", "ControllerDotLayouts.json",
    "OC Unleashed Configurator for Skyrim VR.exe",
    "OCU Keyboard Studio/OCU Keyboard Studio.exe",
    "OCU Keyboard Studio/Assets/en_gb.kb",
    "Interface/controls/pc/controlmapvr.txt", "Interface/Translate_ENGLISH.txt",
    "Interface/controls/pc/oculuscontroller.txt",
    "Docs/OCU-Setup-Readme.html", "LICENSE.txt",
    "Docs/Licenses/THIRD-PARTY-NOTICES.txt",
    "Docs/Licenses/OpenComposite-OCU-GPLv3.txt",
}


def exports(data):
    """Read actual PE exports without loading or executing the DLL."""
    assert data[:2] == b"MZ", "Missing PE DOS header"
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0", "Missing PE signature"
    machine, sections = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    assert machine == 0x8664 and struct.unpack_from("<H", data, optional)[0] == 0x20B, "Expected x64 PE"
    table = optional + optional_size

    def offset(rva):
        for i in range(sections):
            virtual_size, virtual_address, raw_size, raw = struct.unpack_from("<IIII", data, table + i * 40 + 8)
            if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
                return raw + rva - virtual_address
        raise AssertionError(f"Unmapped PE RVA {rva:#x}")

    export_rva = struct.unpack_from("<I", data, optional + 112)[0]
    assert export_rva, "Missing PE export table"
    directory = offset(export_rva)
    count = struct.unpack_from("<I", data, directory + 24)[0]
    names = offset(struct.unpack_from("<I", data, directory + 32)[0])
    result = set()
    for i in range(count):
        start = offset(struct.unpack_from("<I", data, names + i * 4)[0])
        result.add(data[start:data.index(b"\0", start)].decode("ascii"))
    return result


def verify(package):
    archive = None
    try:
        if package.is_dir():
            names = [p.relative_to(package).as_posix() for p in package.rglob("*") if p.is_file()]
            read = lambda name: (package / name).read_bytes()
        else:
            archive = zipfile.ZipFile(package)
            assert archive.testzip() is None, "ZIP integrity failure"
            names = [n for n in archive.namelist() if not n.endswith("/")]
            read = archive.read
        assert len(names) == len({n.casefold() for n in names}), "Duplicate package paths"
        for name in names:
            p = PurePosixPath(name)
            assert not p.is_absolute() and ".." not in p.parts and "\\" not in name, f"Invalid path: {name}"
        missing = sorted(REQUIRED - set(names))
        assert not missing, "Missing required user files: " + ", ".join(missing)
        definition = {}
        for line in read("Interface/controls/pc/oculuscontroller.txt").decode("utf-8-sig").splitlines():
            if not line.strip() or line.startswith("//"):
                continue
            # Match Skyrim's native strtok_s delimiter set: TAB/CR, not spaces.
            fields = [field for field in line.split("\t") if field]
            if fields:
                assert len(fields) in (2, 3), "Controller definitions require TAB-separated fields"
                key = int(fields[1], 16)
                assert key not in definition, f"Duplicate controller definition ID: {key}"
                definition[key] = fields[0]
        for key, name in {2: "grab", 7: "OCC_A", 1: "OCC_B", 32: "thumbstick", 33: "trigger"}.items():
            assert definition.get(key) == name, f"Stock Touch identity changed: {name}"
        assert {3, 5, 6, 35} <= definition.keys(), "Missing Frame hold-time definitions"
        forbidden = [n for n in names if PurePosixPath(n).suffix.lower() in
                     {".cpp", ".h", ".inl", ".pdb", ".py", ".ps1", ".vcxproj"} or
                     n.startswith(("Docs/RDM-Source/", "tests/", "build/")) or
                     PurePosixPath(n).name in {"OCU-Build.json", "SHA256SUMS.txt"}]
        assert not forbidden, "Developer files in user package: " + ", ".join(forbidden)
        runtime = read("root/openvr_api.dll")
        plugin = read("SKSE/Plugins/OpenCompositeInput.dll")
        assert {"VR_InitInternal", "VR_GetGenericInterface"} <= exports(runtime), "Invalid OpenVR runtime exports"
        assert {"SKSEPlugin_Load", "SKSEPlugin_Query"} <= exports(plugin), "Invalid SKSE plugin exports"
        assert b"OCU runtime build:" in runtime, "Missing OCU runtime identity"
        assert b"OCU SKSE package:" in plugin, "Missing OCU SKSE identity"
        return {"package": str(package), "files": len(names), "required_components": "verified",
                "runtime_sha256": hashlib.sha256(runtime).hexdigest(),
                "skse_sha256": hashlib.sha256(plugin).hexdigest()}
    finally:
        if archive:
            archive.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(verify(args.package), indent=2))
    except (AssertionError, OSError, ValueError, struct.error, zipfile.BadZipFile) as error:
        parser.exit(1, f"PACKAGE REJECTED: {error}\n")
