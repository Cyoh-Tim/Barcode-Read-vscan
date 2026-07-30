# vscan-lite

산업용 바코드 리더기 수준의 성능을 목표로 만든 임베디드
1D/2D 코드 리딩 라이브러리. V4L2로 캡처된 프레임을 입력받아, 한 프레임 내 다중 코드를 동시에
검출/디코딩하고 GS1 AI를 파싱해 구조화된 결과를 낸다.

> **📋 [PROJECT_NOTES.md](PROJECT_NOTES.md) 먼저(또는 같이) 볼 것**
> 왜 이렇게 설계됐는지, 실측 벤치마크 데이터, 이미 틀린 것으로 판명된 방향,
> 열려있는 이슈가 전부 정리돼 있다. **이 문서(README)는 "무엇을 어떻게
> 빌드/실행하는가"**, PROJECT_NOTES는 "왜 그렇게 됐는가"를 다룬다. 코드를
> 고치기 전에 PROJECT_NOTES의 §7("하지 말 것")만이라도 먼저 훑을 것 —
> 이미 시도해보고 틀린 것으로 판명된 방향이 정리돼 있다.
> Claude / 제작자의 시도 모두 정리되어있다.

## 개발 환경에 대해

**타겟 보드(i.MX8M Plus / Nvidia Jetson Orin Nano) 전제로 이 가이드를
짰다.** 개발은 대부분 x86 PC(Windows/WSL2 포함)에서 하고, aarch64
크로스컴파일과 실기 배포는 보드가 있을 때만 한다 — 그래서 "지금 뭘 할 수
있고 뭘 할 수 없는지"를 단계별로 명확히 구분해뒀다.
기본적으로는 i.MX8M Plus에 맞춰 작업하지만, NPU/GPU를 안 쓴다는 가정의 라이브러리이다. (GPU는 효율이 안 나올 확률이 높으며 NPU는 시간을 많이 쏟아야함.)
Nvidia Jetson Orin Nano 보드를 사용할 땐 컴파일 시 -mcpu=cortex-a78 옵션으로 변경해야한다.   
2026-07-21 기준, Windows WSL2 환경에서 코드 수정 환경을 제작함에 따라 앞으로 작업 효율성을 따져 Nvidia 보드는 거의 사용되지 않는다.

| 하고 싶은 것 | 필요한 것 | 어디서 |
|---|---|---|
| 코드 수정, 정확도/속도 회귀 검증 | x86 리눅스(또는 WSL2) | **아무 PC에서나** |
| aarch64 바이너리가 도는지(정확성)만 확인 | Docker Desktop | **Windows에서도 가능** — §5 |
| 실제 A53 성능(ms, fps) 확인 | 진짜 i.MX8M Plus 보드 | 보드 있을 때만 — §6 |

---

## 1. 파이프라인

```
V4L2 mmap buffer (ISP가 이미 그레이스케일)
   -> GrayView (복사 없음, 포인터만)
   -> 코어 수만큼 워커로 분배 (프레임 단위 병렬, §3.2.7)
   -> 워커별: 빠른 locate -> 실패 시 단계적 폴백 -> 그래도 실패 시
      DPM/1D회전 구제(opt-in) -> GS1 AI 파싱
```

핵심 설계 결정(속도에 크게 기여한 것부터):
- **프레임 단위 병렬** — 4코어 중 1은 애플리케이션, 3은 각각 디코딩 용 -> Nvidia Jetson Orin Nano는 코어 6개로 디코딩 용으로 5개를 사용
- **coarse locate** — 축소본(1/3)에서 먼저 찾고, 못 찾으면 원본 해상도로
- **4단 폴백 체인** — 빠른locate → harder-only → harder+invert → 완전풀옵션
- **연속 프레임 추적(tracked)** — 컨베이어처럼 위치가 이어지는 경우 ROI만 재탐색
- **A53 NEON 자동 벡터화** — 이진화/다운샘플 핫루프를 벡터화 친화적으로 재작성

자세한 실측 수치와 각 결정의 근거는 PROJECT_NOTES.md §3을 볼 것.

---

## 2. 저장소 구조

```
include/vscan.h          공개 C API (여기 하나만 있으면 라이브러리 사용 가능)
include/vscan_internal/*.hpp     내부 C++ 구현 헤더
src/                     구현 (pipeline.cpp가 핵심 오케스트레이션)
third_party/             zxing-cpp, zbar 벤더링 (오프라인 빌드용, 네트워크 불필요)
examples/                decode_image, decode_rois, decode_workers 사용 예제
tools/
  generate_stress_images.py   악조건 40종 테스트 이미지 생성 — 회귀 게이트 (§4)
  generate_corpus.py          대량 코퍼스/파라미터 스윕 생성 (수만~수십만 장, §4.2)
  verify_accuracy.cpp         정확도 회귀 테스트 하네스 (40종 + 대량 코퍼스 겸용)
  bench_device.cpp            실기 캘리브레이션 벤치 (§6)
docker/                  Windows에서 aarch64 정확성 검증 (§5)
build-aarch64.sh          Yocto SDK 기반 크로스 빌드 스크립트
PROJECT_NOTES.md          설계 결정/실측 데이터/이력 (매우 김, 필요한 절만 찾아 읽을 것)
```

---

## 3. x86에서 빌드 (개발/테스트용 — 가장 먼저 할 것)

### 3.1 요구사항

- CMake ≥ 3.16, Ninja(권장) 또는 Make
- g++ (C++17)
- **Windows라면 WSL2(Ubuntu) 안에서** 진행할 것. 네이티브 Windows 빌드는
  지원하지 않는다(라이브러리 자체가 리눅스 임베디드 타겟이라 POSIX 가정이
  코드 곳곳에 있음 — V4L2 캡처 부분은 애초에 리눅스 전용).

```powershell
# Windows: WSL2 + Ubuntu 준비가 안 돼 있다면
wsl --install -d Ubuntu
```

### 3.2 빌드

```bash
# (WSL2/리눅스 안에서)
git clone <repo-url> vscan-lite
cd vscan-lite

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DVSCAN_USE_ZBAR=ON -DVSCAN_BUILD_EXAMPLE=ON
cmake --build build -j$(nproc)
```

빌드 옵션:

| 옵션 | 기본 | 설명 |
|---|---|---|
| `VSCAN_USE_ZBAR` | ON | ZBar 번들 빌드(런타임 사용은 `enable_zbar_fastpath`로 별도 제어, 기본 꺼짐) |
| `VSCAN_BUILD_EXAMPLE` | ON | 예제/CLI 빌드 |
| `VSCAN_USE_OPENCV` | OFF | **자리표시자 — 실제 구현 없음**(의도적으로 OpenCV 안 씀, PROJECT_NOTES §7) |

LTO는 Release 빌드에서 툴체인이 지원하면 자동 켜짐(실패해도 빌드는 계속 진행).

### 3.3 바로 확인

```bash
export LD_LIBRARY_PATH=build:build/third_party/zxing-cpp/core:build/zbar_install/lib
./build/decode_image examples/sample_images/industrial_sample.pgm
```

`[QR] ...`, `[Code128] ...` 같은 출력이 나오면 정상이다.

---

## 4. 정확도/속도 회귀 검증

**코드를 고쳤으면 반드시 이걸 돌릴 것.** PROJECT_NOTES §3.4에서 확인했듯,
이미지 한두 장으로만 판단하면 작은 코드/저대비 같은 조건에서 검출을
잃는 걸 놓친다.

```bash
# 1) 악조건 테스트 이미지 40종 생성 (pip install qrcode python-barcode pillow numpy 필요)
python3 tools/generate_stress_images.py --outdir ./stress

# 2) 정확도 검증 하네스 빌드/실행
g++ -O3 -std=c++17 -Iinclude tools/verify_accuracy.cpp \
    -Lbuild -lvscan -Wl,-rpath,'$ORIGIN' -o verify_accuracy
LD_LIBRARY_PATH=build:build/third_party/zxing-cpp/core:build/zbar_install/lib \
    ./verify_accuracy ./stress
```

**기준선: 세 경로(full/two_stage/two_stage-fast) 모두 37/40.** 이보다
떨어지면 회귀다. (`enable_dpm_rescue=1`을 명시하면 38/40 — DPM 구제는 opt-in,
PROJECT_NOTES §3.2.18 참고)

출력 예:
```
image                              exp | full               | 2stage             | 2stage-fast
...
검출 성공 / 전체                 | 37/40      1155.0ms | 37/40       638.2ms | 37/40       593.8ms
```

### 4.2 대량 코퍼스 / 파라미터 스윕 (튜닝용)

40종은 **회귀 게이트**다(통과/실패). 성능이나 검출률을 실제로 개선할 때는
표본이 40장뿐이라 부족하다 — 조건당 이미지 1장이라 개선인지 우연인지
구분이 안 되고, 조건 조합이 없고, 40장에 과적합된다.
`tools/generate_corpus.py`가 이걸 채운다.

**중요 — 용량**: 2048×1536 PGM 한 장이 3.1MB다. 1도 간격 회전 360장 ×
대비 50단계면 18,000장 = **56GB**. 축을 더 걸면 수십만 장 = 수백 GB로
디스크가 그냥 찬다. 그래서 기본 사용법은 파일 저장이 아니라 **스트리밍**이다
— 프레임을 만들어 파이프로 바로 디코더에 먹이고 버린다(**디스크 0**).

```bash
# (a) 파라미터 스윕: 1도 간격 360장을 디스크에 한 장도 안 남기고 측정
python3 tools/generate_corpus.py --sweep angle:0:359:1 --stream \
  | ./verify_accuracy --stdin --paths 2stage

# (b) 축 2개 곱하기 (대비 x 모듈 크기 = 20 x 27 = 540장)
python3 tools/generate_corpus.py --stream \
    --sweep contrast:0.05:1.0:0.05 --sweep module:1.5:8:0.25 --base sym=CODE128 \
  | ./verify_accuracy --stdin

# (c) 난수 코퍼스 A/B — 변경 전/후에 같은 --seed 로 두 번 돌려 CSV 비교
python3 tools/generate_corpus.py -n 5000 --difficulty mixed --stream \
  | ./verify_accuracy --stdin --csv before.csv

# (d) 용량/시간 미리보기 (아무것도 안 만든다)
python3 tools/generate_corpus.py --sweep angle:0:359:1 --sweep noise:0:50:2 --est

# (e) 정말 파일로 남겨야 할 때만 (용량 상한이 걸려 있다 — 넘으면 시작조차 안 함)
python3 tools/generate_corpus.py -o ./corpus -n 2000 --max-disk-gb 10
```

스윕 축: `angle module contrast bright blur motion noise persp curve glare
shadow count sym ec` (`--sweep AXIS:START:STOP:STEP` 또는 `AXIS:v1,v2,v3`,
나머지 축은 `--base k=v,k=v`로 고정). 스윕은 **결정적**이다 — 지정한 축
외에는 난수 열화가 전혀 안 들어가서, 차이가 그 축 때문이라고 말할 수 있다.

`verify_accuracy`는 정답(코드 개수/텍스트/조건 태그)을 프레임 헤더나
`labels.tsv`에서 읽어서 이렇게 보여준다:

```
path           img_pass         codes   text_ok  misdec    dup   mean_ms  p50_ms  p95_ms
2stage            78.3%    340/374        80.2%       0     40     44.36   24.07  108.07

조건 태그별 (코드 단위 검출률 % / 이미지당 평균 ms)
angle=19                    1 | 100.0%    53.74
angle=20                    1 |   0.0%   114.20      <- 여기서 끊긴다
```

- **코드 단위** 검출률(이미지 합격/불합격보다 해상도가 높다)
- 디코딩 **텍스트 정답 대조** — 개수만 세면 안 보이는 오디코딩(`misdec`)과
  같은 코드 중복 반환(`dup`)을 분리해서 센다
- **조건 태그별 집계** — 무엇이 느리고 어디서 끊기는지가 축 단위로 보인다

**판독 가능성 버킷 (난독 이미지 분리)** — 난수로 조건을 뽑으면 물리적으로
아무도 못 읽는 이미지(모듈 1px, 대비가 노이즈보다 작음, 블러가 모듈보다 큼)가
반드시 섞인다. 그걸 포함한 검출률은 "우리 성능 + 물리 한계"의 혼합이라
개선 판단에 못 쓴다. 그래서 생성기가 코드마다 판정해서 **폴더를 나눈다**:

```
corpus/ok/           읽을 수 있어야 정상   <- ★ 여기 성공률이 진짜 지표
corpus/borderline/   리더에 따라 갈리는 경계
corpus/mixed/        한 프레임에 섞임
corpus/impossible/   원래 안 되는 게 맞음(난독) — 실패해도 감점 아님
```

```bash
./verify_accuracy ./corpus/ok      # 읽혀야 정상인 것만 채점
./verify_accuracy ./corpus         # 전부 + 버킷별 요약 (한 단계 아래까지 훑는다)
python3 tools/generate_corpus.py -n 5000 --bucket ok --stream | ./verify_accuracy --stdin
```

분류 기준은 **물리(신호가 남아있는가)뿐이다.** "우리가 못 읽는 조건"은 절대
난독에 넣지 않는다 — 20~25도 회전한 1D는 지금 우리가 못 읽지만 상용 리더는
읽으므로 `ok`로 분류돼 실패로 집계된다. 그게 개선 대상이기 때문이다.

대량 코퍼스에는 물리적으로 못 읽는 이미지가 섞이므로 **절대 검출률의
목표치는 의미가 없다.** 항상 같은 `--seed`로 만든 같은 코퍼스에 대해
변경 전/후를 비교할 것. (배경은 PROJECT_NOTES §3.8, 1차 측정 결과와
개선 로드맵은 §3.9~3.10)

---

## 5. Windows에서 aarch64 정확성 검증 (보드 없이)

**Docker Desktop의 arm64 에뮬레이션(QEMU)으로 "aarch64 바이너리가 x86과
동일하게 판독하는가"를 검증할 수 있다.** ⚠️ 에뮬레이션이라 **여기서 나오는
시간(ms)은 전부 무의미** — 검출 개수만 본다. 실제 성능은 §6(실기)에서만
확인 가능.

```powershell
cd vscan-lite

# 최초 1회: arm64 에뮬레이션 되는지 확인 ("aarch64" 나와야 함)
docker run --rm --platform linux/arm64 ubuntu:24.04 uname -m

docker build --platform linux/arm64 -t vscan-arm64-test -f docker/Dockerfile.arm64-test .
docker run --rm --platform linux/arm64 `
  -v "$PWD:/work" vscan-arm64-test
```

빌드는 에뮬레이션이라 10~30분 걸릴 수 있다. 결과 로그는 `docker-out/`에
남는다. **기대값: 세 경로 모두 37/40**(x86과 완전히 동일해야 함 — 다르면
아키텍처별 버그가 있는 것).

---

## 6. 실기(i.MX8M Plus 기준) 빌드 및 배포

### 6.1 크로스 컴파일

Yocto SDK가 있는 환경(리눅스, WSL2도 가능)에서:

```bash
source /opt/fsl-imx-xwayland/6.x/environment-setup-aarch64-poky-linux  # 실제 경로는 SDK에 맞게
./build-aarch64.sh
```

산출물:
```
build-aarch64/libvscan.so
build-aarch64/third_party/zxing-cpp/core/libZXing.so*
build-aarch64/zbar_install/lib/libzbar.so*   (VSCAN_USE_ZBAR=ON일 때)
```

이 3종 `.so`를 전부 보드로 가져가야 한다(정적 링크 안 함). `ldd`로 NEEDED
확인 후 빠진 게 없는지 체크할 것.

> **Yocto SDK 함정**: `CC`/`CXX`를 `-DCMAKE_C_COMPILER=...`로 직접 넘기지
> 말 것. SDK의 CC/CXX는 "컴파일러 경로 + `--sysroot`/`-march` 플래그"가
> 한 문자열로 합쳐져 있어서, `-D`로 넘기면 CMake가 전체 문자열을 실행파일
> 경로로 오인해 실패한다. `build-aarch64.sh`가 이미 올바른 방식(환경변수로
> 전달)으로 처리해뒀다. <- 이거 때문에 헛수고로 이어질 확률이 매우 높다.

### 6.2 실기 성능 캘리브레이션 (필수 — 문서의 시간 수치는 전부 x86 기준)

**PROJECT_NOTES.md의 모든 ms/fps 수치는 x86 샌드박스 실측이다.** A53
인오더 코어의 상대 비용은 다르다(대략 7~10배 느림으로 추정 — 어디까지나
과거 실측 기반 추정치, 지금 빌드로 재확인 필요). 실기에서 꼭 돌려볼 것:

```bash
g++ -O3 -std=c++17 -Iinclude tools/bench_device.cpp \
    -Lbuild-aarch64 -lvscan -lpthread -o bench_device
# stress/ 디렉토리를 보드로 복사한 뒤
./bench_device ./stress
```

`[A]` 이진화 방식별 비용, `[B]` tracked 3단 비용, `[C]` packed vs
zero-copy, `[D]` API 경로별 평균, `[E]` 워커 1/2/3 실측 스케일링(이게
특히 중요 — x86은 vCPU 1개라 진짜 병렬 효율을 못 쟀다)까지 한 번에
나온다.

### 6.3 실전 어려운 프레임으로 검증

실기 카메라로 캡처한 실패 프레임이 있다면(raw 그레이스케일, `W*H` 바이트),
그대로 `verify_accuracy`나 아래 스니펫에 넣어서 어느 단계에서 걸리는지
확인할 수 있다:

```bash
# raw -> pgm 변환 (해상도에 맞게 W H 수정)
{ printf 'P5\n2048 1536\n255\n'; cat frame.raw; } > frame.pgm
```

**실기 숫자를 볼 때 x86과 직접 비교하지 말 것** — 반드시 §6.2의
`bench_device` 결과나 x86→A53 환산 비율(현재 추정 7~10배)을 곱한
예측 범위와 비교할 것. PROJECT_NOTES §6.5에 이 함정이 자세히 정리돼 있다.

---

## 7. 통합 시 참고 — 프로세스 경계로 쓸 때

vscan-lite를 별도 프로세스로 fork+exec 해서 쓰는 통합이라면, **매 프레임
새 프로세스를 띄우지 말 것** — 실측 프레임당 ~29ms의 고정 오버헤드가
붙는다(PROJECT_NOTES §6.4.5). `tools/vscan_decode_file.cpp`가 영구
워커 패턴(파이프로 프레임 반복 수신)의 참고 구현이다.

---

## 8. 주요 API (`include/vscan.h`)

| 함수 | 언제 |
|---|---|
| `vscan_process_gray()` | 기본. 위치를 전혀 모를 때, 정확도 최우선 |
| `vscan_process_gray_two_stage()` | **일반적으로 이걸 쓸 것.** full과 검출력 동일하면서 훨씬 빠름 |
| `vscan_process_gray_rois()` | 상위 검출기/사람이 위치를 이미 알 때 |
| `vscan_process_gray_tracked()` | 연속 프레임(컨베이어). 위치가 이어질 때 |

자주 쓰는 `vscan_config_t` 필드:

| 필드 | 기본값 | 언제 만질까 |
|---|---|---|
| `worker_mode` | 0 | **워커 여러 개로 병렬 처리한다면 반드시 1** (안 하면 최대 62% 손해) |
| `symbology_mask` | 0(전체) | 배치 환경의 심볼로지를 안다면 좁힐 것 — 2~2.5배 |
| `min_expected_codes` | 1 | 아이템당 코드 개수를 안다면 설정 — 다중코드 부분검출 방지 |
| `enable_dpm_rescue` | 0(꺼짐) | **그 라인이 DPM(레이저 점각인)을 쓴다고 설치 시 확인됐을 때만** opt-in |

각 필드의 상세 설명과 트레이드오프는 `vscan.h`의 주석에 실측치와 함께
적혀있다.

---

## 9. 알려진 한계

PROJECT_NOTES §4/§7에 자세히 있지만 요약하면 — 지금 구성으로 **못 읽는
것으로 확인된** 조건:
- DPM(레이저 도트 각인) — opt-in 구제로 해결됨(§3.2.18)
- 초저대비(대비 8% 수준) — 미해결. 디코드 레벨 처리로는 안 됨, 캡처
  단계(노출/게인) 문제일 가능성(§6.4.5b)
- 강한 원근 왜곡 — 미해결. 실물 비교 대상 리더기도 동일 조건에서 실패 확인(§6.4.6)

새로운 실패 케이스를 발견하면, 먼저 PROJECT_NOTES §7("하지 말 것")에
이미 시도하고 틀린 접근이 있는지 확인할 것.

## 10. 목표
2026년 8월 31일 내 최대한의 정확성과 최대 속도로 바코드를 스캔하는 것   
~100ms(10 fps)내에 할 수 있는 모든 바코드 리딩
