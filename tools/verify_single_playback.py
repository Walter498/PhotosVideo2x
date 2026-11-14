#!/usr/bin/env python3
"""Prevent automatic repeat playback from returning to the shipped tweak."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[1]
blocked=re.compile(r'PV2Loop|LoopController|LoopClock|setLoopingEnabled|isLoopingEnabled|loopTimeRange|_copyProxyUnfoldedTimebase|currentUnfoldedTime|CMTimeFoldIntoRange|pv2_(?:refreshLoop|observeLoop|installLoop|clearLoop|syncNativeSlider)|_loop(?:Timing|Repair|Slider|Periodic|Observed)|AVPlayerLooper|AVPlayerItemDidPlayToEndTimeNotification')
errors=[]
for path in sorted(root.iterdir()):
    if path.suffix not in {'.h','.xm','.m','.mm','.cpp'}: continue
    for line,text in enumerate(path.read_text().splitlines(),1):
        if blocked.search(text):errors.append(f'{path.name}:{line}: {text.strip()}')
for name in ['LoopController.h','tests/LoopControllerTests.mm','tests/LoopClockTests.mm']:
    if (root/name).exists():errors.append(f'obsolete file: {name}')
if errors:raise SystemExit('\n'.join(errors))
print('PASS: no automatic-repeat hooks, loop state, cross-lap clocks or loop repair observers in production')
