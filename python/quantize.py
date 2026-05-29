"""Quantize car_mcu.onnx to a .espdl file targeting ESP32-P4.

Run inside the `esp-ppq` conda env (esp-ppq 1.3.x with PyTorch installed).

    conda activate esp-ppq
    python python/quantize.py \
        --onnx   python/car_mcu.onnx \
        --output python/car_mcu.espdl \
        --calib-dir python/calib_images   # optional; random data if omitted

The script:
  - Loads the ONNX model (input: [1, 3, 320, 320] fp32).
  - Builds a small calibration DataLoader (real images if --calib-dir is
    given, otherwise random tensors as a sanity-check fallback).
  - Calls esp_ppq.api.espdl_quantize_onnx with target=esp32p4, 8-bit
    per-channel quantization (the default for ESP-DL >= 3.3).
  - Writes <output>.espdl, plus diagnostic .json/.info files alongside it.

Calibration tips
----------------
For real deployments, point --calib-dir at ~100 images that *resemble what
the camera sees in production* (same lighting, same framing, same classes).
Quantization accuracy degrades quickly without representative data.

Images can be JPEG/PNG of any aspect ratio: this script center-crops the
short side to a square and resizes to 320x320, then applies the same
normalization the model was trained with (here: scale to [0, 1] — adjust
NORMALIZE_MEAN/STD if your training pipeline used ImageNet normalization).
"""

import argparse
import os
from pathlib import Path
from typing import Iterable, List

import numpy as np
import onnx
import torch
from onnx import shape_inference
from PIL import Image
from torch.utils.data import DataLoader, Dataset

from esp_ppq.api import espdl_quantize_onnx


# ---------- ONNX pre-processing ----------
# esp-ppq's espdl exporter cannot handle negative `axis` attributes when it
# rewrites tensor layouts (NCHW <-> NHWC). The original car_mcu.onnx uses
# axis=-1 on its tail Concat/Softmax ops, which trips:
#     ValueError: -1 is not in list
# Walk the graph, infer each tensor's rank, and rewrite every negative
# axis/axes attribute as the equivalent non-negative index. Saved to a new
# file so the original ONNX stays untouched.

# Ops with a single scalar `axis` attribute.
_AXIS_OPS = {
    "Concat", "Softmax", "LogSoftmax", "ArgMax", "ArgMin",
    "Gather", "GatherElements", "Flatten",
}
# Ops with a list `axes` attribute baked into the node (not an input).
_AXES_OPS = {
    "ReduceSum", "ReduceMean", "ReduceMax", "ReduceMin", "ReduceProd",
    "ReduceL1", "ReduceL2", "ReduceLogSum", "ReduceLogSumExp",
    "ReduceSumSquare", "Squeeze", "Unsqueeze",
}


def _rank_of(value_info_map: dict, name: str) -> int | None:
    """Return tensor rank from inferred shapes, or None if unknown."""
    vi = value_info_map.get(name)
    if vi is None or not vi.type.tensor_type.HasField("shape"):
        return None
    return len(vi.type.tensor_type.shape.dim)


def fix_negative_axes(onnx_in: Path, onnx_out: Path) -> None:
    model = onnx.load(str(onnx_in))
    model = shape_inference.infer_shapes(model)
    g = model.graph

    value_info_map = {}
    for vi in list(g.input) + list(g.output) + list(g.value_info):
        value_info_map[vi.name] = vi

    n_axis_fixes = 0
    n_axes_fixes = 0
    for node in g.node:
        if not node.input:
            continue
        rank = _rank_of(value_info_map, node.input[0])

        if node.op_type in _AXIS_OPS:
            for attr in node.attribute:
                if attr.name == "axis" and attr.i < 0:
                    if rank is None:
                        print(f"[fix-axes] WARNING: rank unknown for {node.name}, "
                              f"cannot rewrite axis={attr.i}")
                        continue
                    new_axis = attr.i + rank
                    print(f"[fix-axes] {node.op_type:8s} {node.name or '<unnamed>':30s}: "
                          f"axis {attr.i} -> {new_axis}")
                    attr.i = new_axis
                    n_axis_fixes += 1

        elif node.op_type in _AXES_OPS:
            for attr in node.attribute:
                if attr.name == "axes":
                    new_ints = []
                    changed = False
                    for a in attr.ints:
                        if a < 0:
                            if rank is None:
                                new_ints.append(a)
                                continue
                            changed = True
                            new_ints.append(a + rank)
                        else:
                            new_ints.append(a)
                    if changed:
                        print(f"[fix-axes] {node.op_type:8s} {node.name or '<unnamed>':30s}: "
                              f"axes {list(attr.ints)} -> {new_ints}")
                        del attr.ints[:]
                        attr.ints.extend(new_ints)
                        n_axes_fixes += 1

    print(f"[fix-axes] rewrote {n_axis_fixes} axis attrs, {n_axes_fixes} axes attrs")
    onnx.save(model, str(onnx_out))


# ---------- Preprocessing ----------
INPUT_SIZE = 320            # model expects 3 x 320 x 320
BATCH_SIZE = 1              # ESP-PPQ requires batch=1 for export
DEVICE = "cuda" if torch.cuda.is_available() else "cpu"

# Normalisation used at training time. car_mcu.onnx appears to use plain
# [0, 1] scaling (Clip-based activations, no preprocessing baked in). If
# your training script applied ImageNet mean/std, change these to:
#     NORMALIZE_MEAN = [0.485, 0.456, 0.406]
#     NORMALIZE_STD  = [0.229, 0.224, 0.225]
NORMALIZE_MEAN: List[float] = [0.0, 0.0, 0.0]
NORMALIZE_STD: List[float] = [1.0, 1.0, 1.0]


def load_and_preprocess(path: Path) -> torch.Tensor:
    img = Image.open(path).convert("RGB")
    w, h = img.size
    side = min(w, h)
    img = img.crop(((w - side) // 2, (h - side) // 2,
                    (w - side) // 2 + side, (h - side) // 2 + side))
    img = img.resize((INPUT_SIZE, INPUT_SIZE), Image.BILINEAR)
    arr = np.asarray(img, dtype=np.float32) / 255.0          # HWC, [0,1]
    arr = (arr - np.array(NORMALIZE_MEAN, dtype=np.float32)) / \
          np.array(NORMALIZE_STD,  dtype=np.float32)
    arr = arr.transpose(2, 0, 1)                              # CHW
    return torch.from_numpy(arr)


class ImageFolderCalibSet(Dataset):
    """Loads JPEG/PNG files from a directory for calibration."""

    EXTS = {".jpg", ".jpeg", ".png", ".bmp"}

    def __init__(self, root: Path, max_samples: int = 256):
        self.paths = sorted(p for p in root.iterdir()
                            if p.suffix.lower() in self.EXTS)[:max_samples]
        if not self.paths:
            raise RuntimeError(f"No calibration images found under {root}")

    def __len__(self) -> int:
        return len(self.paths)

    def __getitem__(self, i: int) -> torch.Tensor:
        return load_and_preprocess(self.paths[i])


class RandomCalibSet(Dataset):
    """Fallback: random tensors. Quick sanity check only — use real images
    for any model you intend to deploy."""

    def __init__(self, n: int = 32):
        self.n = n

    def __len__(self) -> int:
        return self.n

    def __getitem__(self, i: int) -> torch.Tensor:
        torch.manual_seed(i)
        return torch.randn(3, INPUT_SIZE, INPUT_SIZE)


def build_dataloader(calib_dir: Path | None) -> DataLoader:
    if calib_dir and calib_dir.exists():
        ds = ImageFolderCalibSet(calib_dir)
        print(f"[calib] using {len(ds)} images from {calib_dir}")
    else:
        ds = RandomCalibSet(n=32)
        print("[calib] WARNING: no --calib-dir given, using random tensors. "
              "Accuracy of the quantized model will be poor. Re-run with "
              "real representative images for production use.")
    return DataLoader(ds, batch_size=BATCH_SIZE, shuffle=False)


# ---------- Main ----------
def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--onnx",       default="./car_mcu.onnx",
                    help="input ONNX model path")
    ap.add_argument("--output",     default="./car_mcu.espdl",
                    help="output .espdl path")
    ap.add_argument("--calib-dir",  default="/media/verser/robot/Dataset/standford_car/JPEGImages",
                    help="folder with calibration images (jpg/png)")
    ap.add_argument("--calib-steps", type=int, default=32,
                    help="number of calibration batches to run")
    ap.add_argument("--target",     default="esp32p4",
                    choices=["esp32p4", "esp32s3", "esp32s31", "c"])
    ap.add_argument("--bits",       type=int, default=8, choices=[8, 16])
    args = ap.parse_args()

    onnx_path = Path(args.onnx).resolve()
    out_path  = Path(args.output).resolve()
    calib_dir = Path(args.calib_dir).resolve() if args.calib_dir else None

    if not onnx_path.is_file():
        raise SystemExit(f"ONNX file not found: {onnx_path}")
    out_path.parent.mkdir(parents=True, exist_ok=True)

    # Patch negative axis/axes attributes in place so the espdl exporter is
    # happy. The fixed model is written to a sibling file so the original
    # ONNX is preserved for debugging.
    fixed_onnx_path = onnx_path.with_name(onnx_path.stem + ".axesfixed.onnx")
    print(f"[fix-axes] {onnx_path.name} -> {fixed_onnx_path.name}")
    fix_negative_axes(onnx_path, fixed_onnx_path)

    dataloader = build_dataloader(calib_dir)

    print(f"[quant] {fixed_onnx_path.name}  ->  {out_path.name}")
    print(f"[quant] target={args.target}  bits={args.bits}  device={DEVICE}")

    espdl_quantize_onnx(
        onnx_import_file=str(fixed_onnx_path),
        espdl_export_file=str(out_path),
        calib_dataloader=dataloader,
        calib_steps=args.calib_steps,
        input_shape=[BATCH_SIZE, 3, INPUT_SIZE, INPUT_SIZE],
        target=args.target,
        num_of_bits=args.bits,
        device=DEVICE,
        error_report=True,
        verbose=1,
    )

    print(f"[done] wrote {out_path}")
    print("Sidecar files (same folder):")
    for f in sorted(out_path.parent.iterdir()):
        if f.stem == out_path.stem and f != out_path:
            print(f"  {f.name}")


if __name__ == "__main__":
    main()
