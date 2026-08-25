#!/usr/bin/env python3
"""Quick sanity check for a downloaded vectors.json.

Run after you download the GitHub Actions artifact:
    python inspect_vectors.py vectors.json

It just confirms the file looks well-formed (uniform values in [0,1), right
counts, seeds present) before we use it to build the pure-Python RNG port.
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path


def main() -> None:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "vectors.json")
    data = json.loads(path.read_text(encoding="utf-8"))

    vectors = data.get("vectors", [])
    n = data.get("n_draws")
    print(f"generator : {data.get('generator')}")
    print(f"n_draws   : {n}")
    print(f"seeds     : {len(vectors)}")
    print()

    problems = 0
    for v in vectors:
        seed = v["seed"]
        bits = v["uniform_bits"]
        vals = v["uniform_values"]

        # Recompute the float from its 32-bit bit pattern; must match & be in [0,1).
        for b, val in zip(bits, vals):
            f = struct.unpack("<f", struct.pack("<I", int(b)))[0]
            if not (0.0 <= f < 1.0):
                problems += 1
                print(f"  seed {seed}: uniform out of range: {f}")
                break
            if abs(f - val) > 1e-6:
                problems += 1
                print(f"  seed {seed}: bit/value mismatch: {f} vs {val}")
                break

        first = struct.unpack("<f", struct.pack("<I", int(bits[0])))[0]
        print(f"  seed {seed:>20}  first uniform={first:.9f}  sub_seeds={v['get_random_seeds']}")

    print()
    print("OK — vectors look well-formed." if problems == 0
          else f"{problems} problem(s) found.")


if __name__ == "__main__":
    main()
