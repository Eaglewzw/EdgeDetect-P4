#!/usr/bin/env python3
"""Repack esp-ppq 1.3.x .espdl into the multi-model container that
ESP-DL 3.3.x FbsLoader expects.

esp-ppq 1.3 writes a 16-byte "single model" wrapper:

    [ EDL2 | encrypt(0/1) | data_len | reserved ][ flatbuffer raw data ]

ESP-DL 3.3 FbsLoader interprets the wrapper as a "multi model" index:

    header[1] = model_count        ← reads encrypt byte (always 0)
    header[2+3*i]   = offset_i     ← offset of sub-model i
    header[2+3*i+1] = name_offset  ← offset of model name string
    header[2+3*i+2] = name_length

When model_count=0, get_model_offset_by_index(index=0) returns ESP_FAIL,
leaving offset=0. create_fbs_model then reads the wrapper header as a
sub-model descriptor, gets a wrong size, and eventually crashes reading
16 bytes past EOF.

Fix
---
1. Strip the 16-byte ppq wrapper.
2. Prepend a 16-byte FbsModel sub-header that create_fbs_model expects:
      [magic=0][encrypt=0][size=N][reserved=0]
3. Wrap everything in an EDL2 multi-model container with model_num=1,
   one index entry pointing at `name="main"`, model at the correct offset.

Output: a file that passes FbsLoader's model_num check, offset lookup,
and sub-header parsing cleanly.

Usage
-----
    conda activate esp-ppq
    python python/repack_for_espdl.py python/car_mcu.espdl

Writes python/car_mcu_packed.espdl (ready for ESP-DL 3.3).
"""

import struct
import sys
from pathlib import Path

MODEL_NAME = b"main"
# ESP-DL 3.3: EDLx = single model (offset always 0), PDLx = packed multi-model (uses index).
# We need PDL2 so FbsLoader actually reads the multi-model index.
WRAPPER_MAGIC = b"PDL2"


def repack(src_path: Path) -> Path:
    raw = src_path.read_bytes()
    assert raw[:4] == b"EDL2", f"not a ppq espdl file: {src_path}"
    # Skip the 16-byte ppq wrapper; the rest is flatbuffer model data.
    fbs_data = raw[16:]

    # --- sub-model header (16 bytes, as create_fbs_model expects) ---
    sub_header = struct.pack("<IIII", 0, 0, len(fbs_data), 0)

    # --- multi-model index ---
    # FbsLoader layout:
    #   header[0]     = "EDL2" magic
    #   header[1]     = model_count (N)
    #   header[2..2+3*N-1] = index entries [offset, name_offset, name_len]
    #   Then name strings, then sub-model header + data
    #
    # Offsets relative to the new file start:
    #   [0-3]   "EDL2" magic
    #   [4-7]   model_count (4 B)
    #   [8-19]  index entry (3×4 = 12 B)
    #   [20-23] model name (4 B)
    #   [24-39] sub-model header (16 B)
    #   [40-]   model data
    NAME_LEN     = len(MODEL_NAME)
    HDR_SIZE     = 4 + 4 + 12              # magic + model_count + index(1)
    NAME_OFFSET  = HDR_SIZE
    MODEL_OFFSET = NAME_OFFSET + NAME_LEN  # model starts right after the name
    MODEL_COUNT  = 1

    wrapper = bytearray(WRAPPER_MAGIC)
    wrapper += struct.pack("<I", MODEL_COUNT)   # header[1] = model_num = 1
    # Index for model 0: header[2]=offset, header[3]=name_off, header[4]=name_len
    wrapper += struct.pack("<III", MODEL_OFFSET, NAME_OFFSET, NAME_LEN)

    dst_path = src_path.with_stem(src_path.stem + "_packed")
    data = wrapper + MODEL_NAME + sub_header + fbs_data
    dst_path.write_bytes(data)

    print(f"[repack] {src_path.name}  ({len(raw)} B)")
    print(f"          → {dst_path.name}  ({len(data)} B)")
    print(f"          wrapper: model_num={MODEL_COUNT}  "
          f"model_offset={MODEL_OFFSET}  name=\"{MODEL_NAME.decode()}\"  "
          f"sub_size={len(fbs_data)}")
    return dst_path


if __name__ == "__main__":
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("python/car_mcu.espdl")
    repack(src.resolve())
