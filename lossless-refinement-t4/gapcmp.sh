#!/bin/bash
# Compare the decode of a stream with lost frames left out against the decode of the complete stream.
# Usage: gapcmp.sh <out name> <h264|hevc> [ffmpeg decoder flags...]
set -u
cd "$(dirname "$0")"
FFMPEG=${FFMPEG:-/root/dl/ffmpeg/ffmpeg-master-latest-linux64-gpl/bin/ffmpeg}
name=$1 codec=$2
shift 2
"$FFMPEG" -v error "$@" -f "$codec" -i "out/$name.full.bit" -fps_mode passthrough -f framemd5 - 2>/dev/null | grep -v '^#' | awk -F, '{ print $NF }' | tr -d ' ' > "out/$name.full.md5"
"$FFMPEG" -v error "$@" -f "$codec" -i "out/$name.bit" -fps_mode passthrough -f framemd5 - 2>/dev/null | grep -v '^#' | awk -F, '{ print $NF }' | tr -d ' ' > "out/$name.gap.md5"
python3 - "out/$name" <<'PY'
import sys
base = sys.argv[1]
full = open(base + '.full.md5').read().split()
gap = open(base + '.gap.md5').read().split()
written = [int(l.split()[2]) for l in open(base + '.frames') if l.startswith('F ')]
print(f'  complete stream: {len(full)} frames decoded, gapped stream: {len(gap)} of {len(written)} written frames decoded')
bad = [k for k, f in enumerate(written) if k < len(gap) and f < len(full) and gap[k] != full[f]]
if bad:
    print(f'  {len(bad)} decoded frames differ from the complete stream, first at written frame {bad[0]} (encoder frame {written[bad[0]]})')
else:
    print('  every decoded frame matches the complete stream')
PY
