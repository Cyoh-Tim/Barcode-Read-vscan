# 서드파티 고지

이 저장소가 포함하거나 참고한 외부 저작물의 라이선스 고지.

## 소스를 통째로 포함하는 것 (third_party/)

| 항목 | 위치 | 라이선스 |
|---|---|---|
| zxing-cpp v2.2.1 | `third_party/zxing-cpp/` | Apache-2.0 (해당 디렉터리의 `LICENSE`) |
| ZBar | `third_party/zbar/` | LGPL-2.1 (해당 디렉터리의 `COPYING`) |

## 인코딩 표를 참고한 것

### BWIPP (Barcode Writer in Pure PostScript)

`src/decoder_linear.cpp`의 Industrial(Standard) 2of5 / COOP 2of5 /
Pharmacode 인코딩 표는 BWIPP의 `code2of5` / `pharmacode` 리소스에서
가져와, 같은 프로젝트로 렌더한 이미지의 런 길이와 교차 확인했다.

> MIT License
> Copyright (c) 2004-2024 Terry Burton
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

BWIPP 자체는 이 저장소에 포함되지 않는다 — 시험 이미지 생성기
(`tools/generate_extra_symbologies.py --bwipp`)가 개발 환경에 설치된
`treepoem`을 통해 선택적으로 호출할 뿐이고, 런타임/배포 산출물에는
들어가지 않는다.
