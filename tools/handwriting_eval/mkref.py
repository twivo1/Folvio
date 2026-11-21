import os, json, numpy as np, cv2, pyarrow.parquet as pq
HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)
os.environ['LIMIT'] = '1280'
ns = {'__file__': os.path.join(HERE, 'eval_pages.py')}
exec(open('eval_pages.py', encoding='utf-8').read().split('t = pq.read_table')[0], ns)
t = pq.read_table('gnhk/test.parquet').slice(0, 6).to_pylist()
ref = {}
for r in t:
    img = cv2.imdecode(np.frombuffer(r['image']['bytes'], np.uint8), cv2.IMREAD_COLOR)
    name = r['filename'].replace('.jpg', '.png')
    cv2.imwrite(os.path.join('pages', name), img)
    ref[name] = {'prototype': ns['page_text'](img)[0], 'gt': r['text']}
json.dump(ref, open('pages/ref.json', 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
print(list(ref))
