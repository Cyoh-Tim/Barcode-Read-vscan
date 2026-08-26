# 같은 내용 코드를 격자로 촘촘히 붙인 프레임 — dedup이 별개 코드를 먹는지 본다.
import sys, os
sys.path.insert(0, 'tools')
import numpy as np
from PIL import Image
import generate_corpus as g

os.makedirs('/tmp/dense', exist_ok=True)
KIND = sys.argv[1] if len(sys.argv) > 1 else 'PDF417'
COLS = int(sys.argv[2]) if len(sys.argv) > 2 else 4
ROWS = int(sys.argv[3]) if len(sys.argv) > 3 else 4
MOD  = float(sys.argv[4]) if len(sys.argv) > 4 else 3.0
TEXT = {'PDF417': 'VSCAN-DENSE-01', 'QR': 'VSCAN-DENSE-01',
        'DATAMATRIX': 'VSCAN-DENSE-01', 'CODE128': 'VSCANDENSE01',
        'EAN13': '312245984327'}[KIND]

grid, symname, text, is2d = g._grid_for(KIND, TEXT)
if is2d:
    lab = np.array(g._grid_to_image(grid, MOD, quiet=4).convert('L'), dtype=np.uint8)
else:
    lab = np.array(g._grid_to_image(grid, MOD, quiet=10, rows=40, quiet_y=2)
                   .convert('L'), dtype=np.uint8)
lh, lw = lab.shape
print(f'{symname} 라벨 {lw}x{lh} 정답 "{text}"')

for gap in (8, 24, 60):
    W = COLS * lw + (COLS + 1) * gap
    H = ROWS * lh + (ROWS + 1) * gap
    canvas = np.full((H, W), 235, dtype=np.uint8)
    for r in range(ROWS):
        for c in range(COLS):
            y = gap + r * (lh + gap); x = gap + c * (lw + gap)
            canvas[y:y+lh, x:x+lw] = lab
    p = f'/tmp/dense/{KIND}_{COLS}x{ROWS}_gap{gap:03d}.pgm'
    Image.fromarray(canvas).save(p)
    print(f'  {os.path.basename(p)}  {W}x{H}  기대 {COLS*ROWS}개')
