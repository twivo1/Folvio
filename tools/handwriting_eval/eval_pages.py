# Page-level prototype of the C++ pipeline: PP-OCRv6 small det (DB) + rec (CTC) on GNHK test pages.
# Metric: CER/WER on the page text with lines ordered top->bottom (as the app outputs them), plus
# bag-of-words recall/precision (order independent).
import json, os, sys, math, time
import numpy as np, cv2, onnxruntime as ort, pyarrow.parquet as pq, yaml
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
O = os.path.join(HERE, "official")
N = int(sys.argv[1]) if len(sys.argv) > 1 else 20
LIMIT = int(os.environ.get("LIMIT", "1280"))  # same as the app


def lev(a, b):
    if len(a) < len(b):
        a, b = b, a
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


so = ort.SessionOptions(); so.intra_op_num_threads = 4
det = ort.InferenceSession(os.path.join(O, "PP-OCRv6_small_det_onnx", "inference.onnx"), so)
rec = ort.InferenceSession(os.path.join(O, "PP-OCRv6_small_rec_onnx", "inference.onnx"), so)
chars = ["<blank>"] + [str(c) for c in yaml.safe_load(open(os.path.join(O, "PP-OCRv6_small_rec_onnx", "inference.yml"), encoding="utf-8"))["PostProcess"]["character_dict"]] + [" "]


def detect(bgr):
    h, w = bgr.shape[:2]
    s = min(1.0, LIMIT / max(h, w))
    nh, nw = max(32, int(round(h * s / 32)) * 32), max(32, int(round(w * s / 32)) * 32)
    img = cv2.resize(bgr, (nw, nh)).astype(np.float32) / 255.0
    img = (img - np.array([0.485, 0.456, 0.406], np.float32)) / np.array([0.229, 0.224, 0.225], np.float32)
    prob = det.run(None, {det.get_inputs()[0].name: img.transpose(2, 0, 1)[None]})[0][0, 0]
    mask = (prob > 0.3).astype(np.uint8)
    contours, _ = cv2.findContours(mask, cv2.RETR_LIST, cv2.CHAIN_APPROX_SIMPLE)
    boxes = []
    for c in contours[:1000]:
        rect = cv2.minAreaRect(c)
        (cx, cy), (rw, rh), ang = rect
        if min(rw, rh) < 3:
            continue
        m = np.zeros_like(mask); cv2.fillPoly(m, [c], 1)
        score = float(prob[m > 0].mean())
        if score < 0.6:
            continue
        area, peri = rw * rh, 2 * (rw + rh)
        d = area * 1.5 / peri
        rect = ((cx, cy), (rw + 2 * d, rh + 2 * d), ang)
        if min(rect[1]) < 5:
            continue
        pts = cv2.boxPoints(rect)
        pts[:, 0] *= w / nw; pts[:, 1] *= h / nh
        boxes.append(pts)
    return boxes


def order(pts):
    # tl, tr, br, bl
    s = pts.sum(1); d = np.diff(pts, axis=1).ravel()
    return np.array([pts[np.argmin(s)], pts[np.argmin(d)], pts[np.argmax(s)], pts[np.argmax(d)]], np.float32)


def recognize(bgr, pts):
    p = order(pts)
    w = int(max(np.linalg.norm(p[0] - p[1]), np.linalg.norm(p[2] - p[3])))
    h = int(max(np.linalg.norm(p[0] - p[3]), np.linalg.norm(p[1] - p[2])))
    M = cv2.getPerspectiveTransform(p, np.array([[0, 0], [w, 0], [w, h], [0, h]], np.float32))
    crop = cv2.warpPerspective(bgr, M, (w, h), borderMode=cv2.BORDER_REPLICATE)
    if h >= 1.5 * w:
        crop = np.rot90(crop)
        h, w = w, h
    W = min(3200, max(16, int(math.ceil(48 * w / max(1, h)))))
    x = (cv2.resize(crop, (W, 48)).astype(np.float32) / 255.0 - 0.5) / 0.5
    y = rec.run(None, {rec.get_inputs()[0].name: x.transpose(2, 0, 1)[None]})[0][0]
    idx, pr = y.argmax(1), y.max(1)
    out, conf, last = [], [], 0
    for t, k in enumerate(idx):
        if k != 0 and k != last:
            out.append(chars[k] if k < len(chars) else ""); conf.append(pr[t])
        last = k
    return "".join(out).strip(), float(np.mean(conf)) if conf else 0.0, p


def page_text(bgr):
    items = []
    for b in detect(bgr):
        t, c, p = recognize(bgr, b)
        if t:
            items.append((p[:, 1].mean(), p[:, 0].min(), p[:, 1].max() - p[:, 1].min(), t, c))
    items.sort()
    lines, cur, cy, ch = [], [], None, None
    for y, x, hh, t, c in items:
        if cur and abs(y - cy) > 0.5 * ch:
            lines.append(cur); cur = []
        if not cur:
            cy, ch = y, max(hh, 1)
        cur.append((x, t))
    if cur:
        lines.append(cur)
    return "\n".join(" ".join(t for _, t in sorted(l)) for l in lines), [c for *_, c in items]


t = pq.read_table(os.path.join(HERE, "gnhk", "test.parquet")).slice(0, N).to_pylist()
ce = cn = we = wn = 0
tp = gt_n = hyp_n = 0
tt = 0.0
for r in t:
    img = cv2.imdecode(np.frombuffer(r["image"]["bytes"], np.uint8), cv2.IMREAD_COLOR)
    t0 = time.perf_counter()
    hyp, confs = page_text(img)
    tt += time.perf_counter() - t0
    gt = r["text"]
    g, h = " ".join(gt.split()), " ".join(hyp.split())
    ce += lev(g, h); cn += len(g)
    we += lev(g.split(), h.split()); wn += len(g.split())
    G, H = Counter(w.lower().strip(".,;:!?") for w in g.split()), Counter(w.lower().strip(".,;:!?") for w in h.split())
    tp += sum((G & H).values()); gt_n += sum(G.values()); hyp_n += sum(H.values())
print("pages %d  LIMIT %d  CER %.3f  WER %.3f  word recall %.3f precision %.3f  %.2f s/page" % (len(t), LIMIT, ce / cn, we / wn, tp / gt_n, tp / max(1, hyp_n), tt / len(t)))
print("--- example (page 1) ---")
print(page_text(cv2.imdecode(np.frombuffer(t[0]["image"]["bytes"], np.uint8), cv2.IMREAD_COLOR))[0][:600])
print("--- gt ---")
print(t[0]["text"][:600])
