# Handwriting evaluation (development only)

Reproduces the numbers in `docs/HANDWRITING_OCR.md`. Not part of the build or the packages.

Data (not stored in the repository): the GNHK test split (CC-BY-4.0) from the Hugging Face mirror
`radhakrishnadeshpande/gnhk-pagelevel`, revision `0189a76ad06a7df77f21b1fb6fb1fd12414b00f3`,
file `data/test-00000-of-00001.parquet`, saved as `gnhk/test.parquet` next to the scripts.

Models: the three files listed in `HandwritingOcr::modelFiles()`, saved as
`official/PP-OCRv6_small_det_onnx/inference.onnx`, `official/PP-OCRv6_small_rec_onnx/inference.{onnx,yml}`.

```
python -m venv venv
venv/bin/pip install onnxruntime==1.31.1 opencv-python-headless==4.10.0.84 "numpy<2.3" pyarrow pyyaml
LIMIT=1280 venv/bin/python eval_pages.py 172   # PP-OCRv6 small, whole pages
NP=172 venv/bin/python tess_pages.py           # Tesseract baseline (needs tesseract + eng)
venv/bin/python mkref.py                       # lossless pages + reference text for the C++ parity test
```

Then run the C++ tests with `FOLVIO_HANDWRITING_MODELS=<folder with det.onnx, rec.onnx, rec.yml>` and
`FOLVIO_HANDWRITING_SAMPLES=<pages folder written by mkref.py>`.
