from pathlib import Path
import hashlib, statistics
w,h=1920,1080
for p in sorted(Path('diagnostics').glob('cap_seq_*.raw')):
    b=p.read_bytes()
    y=b[:w*h]
    uv=b[w*h:]
    zero=y.count(0)
    nonzero=len(y)-zero
    ymean=sum(y)/len(y)
    center=[]
    for yy in range(h//4, 3*h//4):
        start=yy*w+w//4
        center.extend(y[start:start+w//2])
    cmean=sum(center)/len(center)
    print(p.name, 'md5', hashlib.md5(b).hexdigest(), 'size', len(b), 'Ymean', round(ymean,2), 'Y0%', round(zero*100/len(y),2), 'centerY', round(cmean,2), 'Yminmax', min(y), max(y), 'Umean', round(sum(uv[0::2])/len(uv[0::2]),2), 'Vmean', round(sum(uv[1::2])/len(uv[1::2]),2))
# Difference ratio between consecutive Y planes
prev=None
for p in sorted(Path('diagnostics').glob('cap_seq_*.raw')):
    y=p.read_bytes()[:w*h]
    if prev is not None:
        diff=sum(1 for a,b in zip(prev,y) if a!=b)
        sad=sum(abs(a-b) for a,b in zip(prev,y))
        print('diff prev->', p.name, 'pixels%', round(diff*100/len(y),2), 'mean_abs', round(sad/len(y),3))
    prev=y
