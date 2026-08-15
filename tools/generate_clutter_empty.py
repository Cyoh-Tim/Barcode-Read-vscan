#!/usr/bin/env python3
"""
generate_clutter_empty.py — **코드가 하나도 없는 잡동사니 장면**을 만든다.

`generate_texture.py`는 벨트 리브/나무결 같은 **자연 무늬**를 낸다. 이
파일은 다른 쪽이다 — 물류 현장의 **인공 구조물**이다:

  label        물류 라벨(테두리 + 표 줄 + 활자)
  warehouse    박스 모서리 사각형 + 테이프 조각
  falsepattern **가짜 파인더(체커보드) + 1D 미끼 막대열**

`falsepattern`이 가장 위험하다. 8x8 체커보드는 QR/DataMatrix 파인더와
같은 국소 구조를 갖고, 막대열은 1D 스캔 신호와 같은 모양이다. 즉 이
프레임들은 "유령이 태어나기 가장 쉬운 장면"이다.

여기 코드는 하나도 없다. **여기서 나오는 것은 전부 유령**이고 기대 코드
수는 0이다(§3.18: 미검출 < 오디코딩).

배경 그림은 generate_corpus.background()를 그대로 쓴다 — 난수 코퍼스와
같은 장면이어야 게이트가 지키는 것과 코퍼스가 재는 것이 어긋나지 않는다.

    python3 tools/generate_clutter_empty.py <출력디렉터리>
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate_corpus as g  # noqa: E402

W, H = 2048, 1536
STYLES = ("label", "warehouse", "falsepattern")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    n = 0
    for style in STYLES:
        for seed in (1, 2, 3):
            rng = np.random.default_rng([0xC10DE, seed, hash(style) % 9973])
            img = g.background(rng, W, H, style, [])
            arr = np.array(img.convert("L"))
            path = os.path.join(outdir, f"clutter_{style}_{seed}_0.pgm")
            with open(path, "wb") as f:
                f.write(f"P5\n{W} {H}\n255\n".encode())
                f.write(arr.tobytes())
            n += 1
    print(f"{n}장 ({', '.join(STYLES)} x 3) — 기대 코드 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
