"""Combine the PlatformIO outputs into one Android-friendly binary at offset 0x0.

Run after: pio run -e esp32c3
Requires:  python -m pip install 'esptool==4.11.0'
The factory binary deliberately stops before the log filesystem partition,
so reflashing it at 0x0 does not wipe logged data by itself. CAUTION: changing
partition layouts or clicking full-chip erase may still lose data.
"""
from pathlib import Path
import hashlib
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / '.pio' / 'build' / 'esp32c3'
OUT = ROOT / 'dist'
OUT.mkdir(exist_ok=True)
required = [BUILD / x for x in ('bootloader.bin', 'partitions.bin', 'firmware.bin')]
for file in required:
    if not file.is_file():
        sys.exit(f'Missing {file}; run: pio run -e esp32c3')
merged = OUT / 'temp-logger-esp32c3-factory-0x0.bin'
# esptool 4.x command; C3's bootloader must be at 0x0, not 0x1000.
cmd = [sys.executable, '-m', 'esptool', '--chip', 'esp32c3', 'merge_bin',
       '-o', str(merged), '--flash_mode', 'dio', '--flash_freq', '40m',
       '--flash_size', '4MB',
       '0x0', str(required[0]),
       '0x8000', str(required[1]),
       '0x10000', str(required[2])]
print(' '.join(cmd))
subprocess.run(cmd, check=True)
for source, target in zip(required, ('bootloader.bin', 'partitions.bin', 'firmware.bin')):
    (OUT / target).write_bytes(source.read_bytes())
with (OUT / 'SHA256SUMS.txt').open('w') as output:
    for f in sorted(OUT.glob('*.bin')):
        digest = hashlib.sha256(f.read_bytes()).hexdigest()
        output.write(f'{digest}  {f.name}\n')
print(f'Factory image created: {merged} ({merged.stat().st_size:,} bytes)')
