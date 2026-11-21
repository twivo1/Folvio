"""Prepare the release-only handwriting evaluation in a disposable runner directory."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
from urllib.request import urlretrieve

root = Path(__file__).resolve().parents[2]
work = Path(os.environ["RUNNER_TEMP"]) / "folvio-ocr"
models = work / "models"
pages = work / "pages"
models.mkdir(parents=True, exist_ok=True)
pages.mkdir(exist_ok=True)

for name, model, revision, filename, digest in [
    ("det.onnx", "det", "28fe5895c24fd108c19eb3e8479f4ab385fbfc62", "inference.onnx",
     "d73e0058b7a8086bbd57f3d10b8bcd4ff95363f67e06e2762b5e814fe9c9410e"),
    ("rec.onnx", "rec", "b8f84f0b80c529de40b4fbb3544b84fa7233a513", "inference.onnx",
     "5435fd747c9e0efe15a96d0b378d5bd157e9492ed8fd80edf08f30d02fa24634"),
    ("rec.yml", "rec", "b8f84f0b80c529de40b4fbb3544b84fa7233a513", "inference.yml",
     "ab078671bb49f06228eadccd34f1bb501e157f7a047095ffb943ba81512c77d1"),
]:
    repository = f"PP-OCRv6_small_{model}_onnx"
    path = models / name
    urlretrieve(f"https://huggingface.co/PaddlePaddle/{repository}/resolve/{revision}/{filename}", path)
    if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise ValueError(f"Unexpected checksum for {name}")
    reference = work / "official" / repository
    reference.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(path, reference / filename)

dataset = work / "gnhk"
dataset.mkdir(exist_ok=True)
urlretrieve("https://huggingface.co/datasets/radhakrishnadeshpande/gnhk-pagelevel/resolve/"
            "0189a76ad06a7df77f21b1fb6fb1fd12414b00f3/data/test-00000-of-00001.parquet",
            dataset / "test.parquet")
for script in ("mkref.py", "eval_pages.py"):
    shutil.copyfile(root / "tools" / "handwriting_eval" / script, work / script)
subprocess.run([sys.executable, str(work / "mkref.py")], check=True)
with open(os.environ["GITHUB_ENV"], "a", encoding="utf-8") as env:
    print(f"FOLVIO_HANDWRITING_MODELS={models}", file=env)
    print(f"FOLVIO_HANDWRITING_SAMPLES={pages}", file=env)
    print("FOLVIO_NETWORK_TESTS=1", file=env)
