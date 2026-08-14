#!/usr/bin/env python3
"""generate_gradient.py — 강한 조명 기울기 + 저대비 QR.

## 왜 있나

§3.65에서 1단계 이진화 기본을 GlobalHistogram으로 바꿨다. 세 코퍼스에서
잃는 축이 없었지만, **임계가 하나**라는 원리적 약점은 남아 있다 — 프레임
좌우 밝기가 크게 다르면 한 임계로 양쪽을 다 자를 수 없다.

이 생성기는 그 약점을 **일부러** 만든다. 두 가지 용도가 있다:

1. `field_diagnose`의 "옛 이진화가 더 낫다" 권고가 실제로 걸리는지
   확인하는 **양성 대조**(§3.59의 규율: 규칙을 넣었으면 걸리는지 볼 것).
2. 기본값을 다시 만질 때 이 축을 같이 재기.

실측(2단계, QR 마스크, reps 3): 기본 14코드 vs `accurate_locate=1` 15코드.
차이가 작다 — 기울기만으로는 잘 안 무너지고 **저대비와 겹쳐야** 벌어진다.
그래서 이 생성기는 잉크 반사율을 0.45까지 올려(대비를 낮춰) 둘을 겹친다.

    python3 tools/generate_gradient.py <출력디렉터리>
"""
import os, sys
import numpy as np, qrcode
from PIL import Image, ImageFilter
W,H=1280,960
out=sys.argv[1]; os.makedirs(out,exist_ok=True)
def qr(p,m):
    q=qrcode.QRCode(border=2,box_size=1,error_correction=qrcode.constants.ERROR_CORRECT_M)
    q.add_data(p); q.make(fit=True)
    i=q.make_image(fill_color="black",back_color="white").convert("L")
    return i.resize((i.width*m,i.width*m),Image.NEAREST)
rows=[]
for gi,grad in enumerate([0.55,0.75,0.88,0.94]):
    refl=np.full((H,W),0.82,dtype=np.float32)
    vis=[]
    for k in range(3):
        p="GRAD-%d-%d"%(gi,k); img=qr(p,5); a=np.array(img,np.float32)/255.
        ah,aw=a.shape; x=60+k*(aw+90); y=380
        if x+aw<=W and y+ah<=H:
            refl[y:y+ah,x:x+aw]=0.45+(0.82-0.45)*a; vis.append(p)
    yy,xx=np.mgrid[0:H,0:W]
    # 왼쪽에서 오른쪽으로 밝기가 grad 배까지 떨어진다 = 한 임계로 못 자른다
    lit=1.0-grad*(xx/W)
    sig=255.*refl*lit
    sig=np.array(Image.fromarray(np.clip(sig,0,255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.6)),np.float32)
    arr=np.clip(sig,0,255).astype(np.uint8)
    n="grad%02d.pgm"%int(grad*100)
    with open(os.path.join(out,n),"wb") as f:
        f.write(b"P5\n%d %d\n255\n"%(W,H)); f.write(arr.tobytes())
    rows.append((n,vis)); print("  %s (코드 %d, 좌우 밝기비 1:%.2f)"%(n,len(vis),1-grad))
with open(os.path.join(out,"labels.tsv"),"w") as f:
    for n,v in rows:
        f.write("%s\t%d\t%s\t%s\t%s\t%s\n"%(n,len(v),"QR","|".join(v),"|".join(["QR"]*len(v)),"gradient"))
