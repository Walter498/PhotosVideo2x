from pathlib import Path
root=Path(__file__).resolve().parent.parent
source=(root/'TimelineController.h').read_text()
names=['invalidateSeeks','requestSeekSeconds:', 'submitSeekTarget:', 'issueNativeSeekTo:', 'nativeSeekCompletedForEpoch:', 'readyForSeeking', 'currentSeconds', 'playbackDuration']
methods=[]
for name in names:
    matches=[]
    pos=0
    while True:
        start=source.find('\n- (',pos)
        if start<0:break
        brace=source.find('{',start)
        line=source[start:source.find('\n',start+1)]
        pos=start+4
        if name not in line or ';' in line:continue
        depth=1;end=brace+1
        while depth:
            if source[end]=='{':depth+=1
            elif source[end]=='}':depth-=1
            end+=1
        matches.append(source[start+1:end])
    assert len(matches)==1,(name,len(matches))
    methods.append(matches[0])
template=(root/'tests/TimelineRuntime.template.mm').read_text()
(root/'tests/GeneratedTimelineRuntimeTests.mm').write_text(template.replace('/* PRODUCTION_METHODS */','\n'.join(methods)))
print('extracted production timeline seek methods')
