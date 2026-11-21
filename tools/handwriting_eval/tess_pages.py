# Tesseract baseline on the same GNHK pages (TESSERACT=<path> if not on PATH).
import os, sys, subprocess, tempfile, time
import numpy as np, cv2, pyarrow.parquet as pq
from collections import Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))
def lev(a, b):
    if len(a) < len(b): a, b = b, a
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1): cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]
t = pq.read_table(os.path.join(HERE, "gnhk", "test.parquet")).slice(0, int(os.environ.get("NP","20"))).to_pylist()
ce=cn=we=wn=tp=gn=hn=0; tt=0
for r in t:
    with tempfile.NamedTemporaryFile(suffix=".jpg", delete=False) as f: f.write(r["image"]["bytes"]); p=f.name
    t0=time.perf_counter()
    h=subprocess.run([os.environ.get("TESSERACT", "tesseract"), p, "stdout", "-l", "eng"], capture_output=True).stdout.decode("utf-8","replace")
    tt+=time.perf_counter()-t0; os.remove(p)
    g=" ".join(r["text"].split()); h=" ".join(h.split())
    ce+=lev(g,h); cn+=len(g); we+=lev(g.split(),h.split()); wn+=len(g.split())
    G=Counter(w.lower().strip(".,;:!?") for w in g.split()); H=Counter(w.lower().strip(".,;:!?") for w in h.split())
    tp+=sum((G&H).values()); gn+=sum(G.values()); hn+=sum(H.values())
print("tesseract pages %d "%len(t)+"CER %.3f WER %.3f recall %.3f precision %.3f %.2f s/page"%(ce/cn,we/wn,tp/gn,tp/max(1,hn),tt/len(t)))
