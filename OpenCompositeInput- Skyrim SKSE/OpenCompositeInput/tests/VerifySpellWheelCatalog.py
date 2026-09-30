"""Read-only cross-check against the author source and the actual installed ESP."""
import pathlib
import re
import struct
import sys

helper = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8-sig")
esp = pathlib.Path(sys.argv[2]).read_bytes()
header = (pathlib.Path(__file__).parent.parent / "src/DapaSpellWheelForms.h").read_text()
pattern = r"UInt32\s+((?:right|left)Projectile(?:SpellWheel|Text)\d+|VitalityBarsProjectile|NeedsBarsProjectile|EnchantmentBarProjectileLeft|EnchantmentBarProjectileRight|FrostfallBarsProjectile)FormId\s*=\s*(0x[0-9A-Fa-f]+)\s*;"
source = {int(value, 16): name for name, value in re.findall(pattern, helper)}
catalog = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", header.split("localIds{")[1].split("};")[0])]
assert len(catalog) == len(source) == 247 and set(catalog) == set(source)
records = {}

def walk(start, end):
    pos = start
    while pos < end:
        assert pos + 24 <= end
        tag, size = struct.unpack_from("<4sI", esp, pos)
        if tag == b"GRUP":
            assert size >= 24 and pos + size <= end
            walk(pos + 24, pos + size)
            pos += size
        else:
            _, form = struct.unpack_from("<II", esp, pos + 8)
            assert pos + 24 + size <= end
            records[form & 0xFFFFFF] = tag
            pos += 24 + size
    assert pos == end

walk(0, len(esp))
for id in catalog:
    assert records.get(id) == b"PROJ", (hex(id), source[id], records.get(id))
excluded = {int(value, 16): name for name, value in re.findall(
    r"UInt32\s+((?:distanceCheckProjectile|SparksProjectile1|GestureSymbolProjectile|MagicLineProjectile1|ConjureCircle\w*Projectile\w*))FormId\s*=\s*(0x[0-9A-Fa-f]+)\s*;", helper)}
assert excluded and not (excluded.keys() & source.keys())
print(f"PASS: all {len(catalog)} catalog identities match author source and installed PROJ records; {len(excluded)} effect/probe identities excluded")
