#!/usr/bin/env python3
"""
generate_empty.py — **코드가 하나도 없는** 프레임을 만든다.

## 왜 필요한가

컨베이어에서는 물건과 물건 사이가 있다. 그 프레임에는 코드가 없고, 그때
드는 값이 곧 **낭비**다. 그런데 이 저장소의 코퍼스는 전부 "코드가 있는"
프레임이라 그 값을 잰 적이 없었다.

재보니 갈렸다(2단계, reps 3):

| 프레임 | 블록범위 상위1% | 빠른불판독 시간 |
|---|---|---|
| 균일 / 어두움 / 그라디언트 | 12 | **0.0ms** |
| 컨베이어 벨트결 | 26 | 6.4 |
| **노이즈 큰 빈 프레임** | **84** | **25.7** |

빈 프레임 건너뛰기(`[[vscan-lite-blank-frame-skip]]`)가 깨끗한 프레임은
공짜로 만든다. 그런데 **노이즈가 심하면 노이즈 자체가 블록 명암을 만들어**
"구조 있음"으로 통과한다 — §3.61에서 "가망 없는 프레임"을 못 가려낸 것과
정확히 같은 물리다.

즉 **노이즈가 큰 빈 프레임은 실패 프레임과 같은 값이 든다.** 싸게 가릴
방법이 없다(§3.72). 줄이는 길은 둘뿐이다: 센서 노이즈를 줄이거나,
실패 경로 자체를 싸게 만들거나(`fast_no_read` + `symbology_mask` +
`NO_INVERT`).

## 쓰는 법

    python3 tools/generate_empty.py <출력디렉터리>
"""
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

W, H = 1280, 960


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/empty"
    os.makedirs(out, exist_ok=True)
    rng = np.random.default_rng(3)
    cases = [
        ("uniform",   lambda: np.full((H, W), 120.0)),
        ("gradient",  lambda: 60 + 120 * np.mgrid[0:H, 0:W][1] / W),
        ("noisy",     lambda: 120 + rng.normal(0, 25, (H, W))),
        ("belt",      lambda: 110 + 18 * np.sin(np.mgrid[0:H, 0:W][0] / 7.0)),
        ("dark",      lambda: np.full((H, W), 28.0)),
    ]
    rows = []
    for i, (nm, f) in enumerate(cases):
        a = np.clip(f() + rng.normal(0, 3, (H, W)), 0, 255).astype(np.uint8)
        a = np.array(Image.fromarray(a).filter(ImageFilter.GaussianBlur(0.6)))
        fn = "empty_%s.pgm" % nm
        with open(os.path.join(out, fn), "wb") as fp:
            fp.write(b"P5\n%d %d\n255\n" % (W, H))
            fp.write(a.tobytes())
        rows.append(fn)
        print("  %s" % fn)
    # 기대 코드 수 0 — 여기서 뭔가 나오면 그건 전부 유령이다.
    with open(os.path.join(out, "labels.tsv"), "w") as fp:
        for fn in rows:
            fp.write("%s\t0\t-\t\t\tempty\n" % fn)


if __name__ == "__main__":
    main()
