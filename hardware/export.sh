#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ByteShift Labs LLC
# Refill the pours, run ERC/DRC and re-export the JLCPCB gerber/drill package from hardware/kicad/.
# Needs KiCad 8 (kicad-cli + the pcbnew python module, Ubuntu: ppa:kicad/kicad-8.0-releases).
# Usage: hardware/export.sh   (from anywhere). Fails on any ERC/DRC violation or unconnected item.
set -euo pipefail
cd "$(dirname "$0")/kicad"
PY=${KICAD_PYTHON:-/usr/bin/python3}

echo "== refill zones"
"$PY" - <<'EOF'
import pcbnew
b = pcbnew.LoadBoard('fuel-center.kicad_pcb')
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save('fuel-center.kicad_pcb')
EOF

echo "== ERC"
kicad-cli sch erc --severity-error --exit-code-violations -o ${TMPDIR:-/tmp}/fuel-center-erc.rpt fuel-center.kicad_sch
echo "== DRC (all violations + unconnected + schematic parity)"
kicad-cli pcb drc --schematic-parity --exit-code-violations -o ${TMPDIR:-/tmp}/fuel-center-drc.rpt fuel-center.kicad_pcb
grep -E "Found [0-9]+ (violations|unconnected|schematic)" ${TMPDIR:-/tmp}/fuel-center-drc.rpt || true

echo "== gerbers + drill"
rm -rf gerbers && mkdir gerbers
kicad-cli pcb export gerbers --layers F.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts \
    --subtract-soldermask -o gerbers/ fuel-center.kicad_pcb
kicad-cli pcb export drill --format excellon --excellon-units mm --generate-map --map-format gerberx2 -o gerbers/ fuel-center.kicad_pcb
rm -f fuel-center-gerbers.zip && "$PY" -c "import zipfile,glob,os; z=zipfile.ZipFile('fuel-center-gerbers.zip','w',zipfile.ZIP_DEFLATED); [z.write(f, os.path.basename(f)) for f in sorted(glob.glob('gerbers/*'))]"

echo "== previews"
mkdir -p preview
"$PY" ../render_preview.py
kicad-cli sch export pdf -o preview/fuel-center-schematic.pdf fuel-center.kicad_sch || echo "schematic pdf skipped"
echo "done: $(ls gerbers | wc -l) gerber files, $(du -h fuel-center-gerbers.zip | cut -f1) zip"
