<!-- 이 문서의 1~6장은 tools/fulltest_report.py가 fulltest-out/에서 생성한다.
     7장(분석)과 8장(다음 개선 대상)은 사람이 읽고 쓴 것이다. 다시 만들려면:
       tools/fulltest_100k.sh && python3 tools/fulltest_report.py fulltest-out -->

# vscan-lite 풀테스트 보고서

프레임 67,680장 / 코드 122,092개 / 심볼로지 15종 / 격자 12종

**텍스트 일치 56.9%** (69,527/122,092) · **오디코딩 91건**

시간(x86 실측): 평균 63.1ms · p95 318.4ms
  → 보드(i.MX8MP) 환산 x8: 평균 505ms · p95 2547ms

> 격자에는 물리적으로 불가능한 모서리를 **일부러** 넣는다. 검출률의 절대값이 아니라 심볼로지·축 사이의 **차이**를 볼 것.

## 1. 심볼로지 x 격자 (텍스트 일치 %)

| 심볼로지 | contrast_bright | module_angle | noise_blur | persp_curve | glare_shadow | dirty_damaged | zone_print | H | tinymod_contrast | tinymod_noise | mod_density | res_module | 전체 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| CODABAR | 96.2 | 100.0 | 100.0 | 44.6 | 100.0 | 100.0 | 99.0 | - | 99.1 | 100.0 | - | - | **92.2** |
| CODE128 | 95.3 | 100.0 | 56.1 | 52.6 | 95.8 | 74.1 | 75.0 | - | 80.2 | 65.8 | 59.6 | 95.2 | **73.5** |
| CODE39 | 95.0 | 100.0 | 56.1 | 44.9 | 96.6 | 90.5 | 100.0 | - | 95.4 | 73.3 | - | - | **84.5** |
| CODE93 | 98.1 | 100.0 | 100.0 | 47.4 | 100.0 | 71.4 | 100.0 | - | 99.7 | 100.0 | - | - | **88.4** |
| DATABAR | 96.2 | 100.0 | 100.0 | 30.5 | 100.0 | 80.3 | 100.0 | - | 99.4 | 100.0 | - | - | **87.4** |
| DATABAREXP | 95.0 | 100.0 | 100.0 | 28.3 | 100.0 | 95.2 | 80.0 | - | 99.7 | 100.0 | - | - | **88.6** |
| DATAMATRIX | 88.8 | 91.8 | 36.2 | 100.0 | 100.0 | 16.1 | 100.0 | - | 40.2 | 17.1 | 74.6 | 81.0 | **69.4** |
| EAN13 | 95.6 | 99.8 | 56.6 | 83.1 | 100.0 | 77.6 | 76.0 | - | 75.9 | 70.6 | 74.3 | 95.2 | **81.5** |
| EAN8 | 92.5 | 99.8 | 62.4 | 95.0 | 100.0 | 70.1 | 97.0 | - | 70.3 | 71.7 | - | - | **85.3** |
| ITF | 97.5 | 100.0 | 100.0 | 65.1 | 100.0 | 94.6 | 95.0 | - | 96.9 | 100.0 | - | - | **93.8** |
| PDF417 | 91.9 | 60.8 | 28.1 | 29.1 | 100.0 | 66.4 | 92.0 | - | 60.4 | 22.5 | 52.6 | 88.1 | **59.5** |
| QR | 83.8 | 96.0 | 40.7 | 87.5 | 100.0 | 14.7 | 100.0 | - | 44.3 | 28.9 | 92.0 | 85.7 | **75.0** |
| UPCA | 95.6 | 99.8 | 58.4 | 84.2 | 100.0 | 72.3 | 87.0 | - | 81.4 | 71.7 | - | - | **85.2** |
| UPCE | 98.1 | 100.0 | 100.0 | 76.5 | 100.0 | 85.7 | 100.0 | - | 91.0 | 100.0 | - | - | **93.4** |
| random | - | - | - | - | - | - | - | 42.3 | - | - | - | - | **42.3** |

## 2. 축별 절단점 — 값이 커질수록 어디서 끊기는가

각 축의 값별 텍스트 일치율이다. **50%를 지나는 값**이 그 축의 벽이다.


### 축 `angle` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| angle | 일치% | 프레임 |
|---|---|---|
| 0 | 100.0 | 350 |
| 10 | 96.6 | 350 |
| 20 | 96.3 | 350 |
| 30 | 96.0 | 350 |
| 40 | 96.0 | 350 |
| 50 | 95.7 | 350 |
| 60 | 95.1 | 350 |
| 70 | 95.4 | 350 |
| 80 | 95.7 | 350 |
| 90 | 98.6 | 350 |

### 축 `blur` — 50% 아래로 내려가는 첫 값: **2.75**

| blur | 일치% | 프레임 |
|---|---|---|
| 0 | 96.7 | 182 |
| 0.5 | 97.3 | 182 |
| 1 | 97.3 | 182 |
| 1.5 | 89.6 | 182 |
| 2 | 80.2 | 182 |
| 2.5 | 50.0 | 182 |
| 3 | 42.9 | 182 |
| 3.5 | 42.9 | 182 |
| 4 | 42.9 | 182 |

### 축 `bright` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| bright | 일치% | 프레임 |
|---|---|---|
| 0.3 | 80.4 | 280 |
| 0.5 | 88.2 | 280 |
| 0.7 | 91.8 | 280 |
| 0.9 | 94.6 | 280 |
| 1.1 | 95.0 | 280 |
| 1.3 | 99.3 | 280 |
| 1.5 | 100.0 | 280 |
| 1.7 | 99.6 | 280 |

### 축 `contrast` — 50% 아래로 내려가는 첫 값: **0.05**

| contrast | 일치% | 프레임 |
|---|---|---|
| 0.05 | 49.6 | 224 |
| 0.15 | 72.3 | 462 |
| 0.25 | 81.6 | 462 |
| 0.35 | 87.7 | 462 |
| 0.45 | 91.8 | 462 |
| 0.55 | 93.5 | 462 |
| 0.65 | 94.2 | 462 |
| 0.75 | 93.9 | 462 |
| 0.85 | 94.6 | 462 |
| 0.95 | 94.2 | 462 |

### 축 `count` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| count | 일치% | 프레임 |
|---|---|---|
| 1 | 96.0 | 50 |
| 3 | 86.0 | 50 |
| 5 | 78.4 | 50 |
| 7 | 68.9 | 50 |
| 9 | 68.0 | 50 |
| 11 | 66.9 | 50 |
| 13 | 70.6 | 50 |
| 15 | 70.0 | 50 |

### 축 `curve` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| curve | 일치% | 프레임 |
|---|---|---|
| 0 | 64.7 | 266 |
| 0.1 | 59.4 | 266 |
| 0.2 | 60.2 | 266 |
| 0.3 | 60.2 | 266 |
| 0.4 | 60.2 | 266 |
| 0.5 | 62.0 | 266 |
| 0.6 | 62.4 | 266 |
| 0.7 | 62.8 | 266 |
| 0.8 | 64.3 | 266 |
| 0.9 | 65.4 | 266 |

### 축 `damaged` — 50% 아래로 내려가는 첫 값: **0.75**

| damaged | 일치% | 프레임 |
|---|---|---|
| 0 | 91.2 | 294 |
| 0.1 | 92.2 | 294 |
| 0.2 | 91.2 | 294 |
| 0.3 | 91.2 | 294 |
| 0.4 | 80.3 | 294 |
| 0.5 | 84.7 | 294 |
| 0.6 | 78.2 | 294 |
| 0.7 | 69.4 | 294 |
| 0.8 | 49.0 | 294 |
| 0.9 | 41.5 | 294 |
| 1 | 18.0 | 294 |

### 축 `dirty` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| dirty | 일치% | 프레임 |
|---|---|---|
| 0 | 78.6 | 294 |
| 0.1 | 78.2 | 294 |
| 0.2 | 77.2 | 294 |
| 0.3 | 75.2 | 294 |
| 0.4 | 73.5 | 294 |
| 0.5 | 71.1 | 294 |
| 0.6 | 70.1 | 294 |
| 0.7 | 69.0 | 294 |
| 0.8 | 67.3 | 294 |
| 0.9 | 67.0 | 294 |
| 1 | 64.6 | 294 |

### 축 `dpm` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| dpm | 일치% | 프레임 |
|---|---|---|
| 0 | 100.0 | 700 |
| 1 | 85.9 | 700 |

### 축 `glare` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| glare | 일치% | 프레임 |
|---|---|---|
| 0 | 99.6 | 238 |
| 0.1 | 99.2 | 238 |
| 0.2 | 99.2 | 238 |
| 0.3 | 99.6 | 238 |
| 0.4 | 100.0 | 238 |
| 0.5 | 99.6 | 238 |
| 0.6 | 99.6 | 238 |
| 0.7 | 99.2 | 238 |
| 0.8 | 99.6 | 238 |
| 0.9 | 99.2 | 238 |
| 1 | 99.2 | 238 |

### 축 `invert` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| invert | 일치% | 프레임 |
|---|---|---|
| 0 | 99.1 | 700 |
| 1 | 86.7 | 700 |

### 축 `module` — 50% 아래로 내려가는 첫 값: **1.125**

| module | 일치% | 프레임 |
|---|---|---|
| 1 | 58.6 | 440 |
| 1.5 | 59.3 | 520 |
| 2 | 76.7 | 786 |
| 2.5 | 82.5 | 786 |
| 3 | 86.1 | 786 |
| 4 | 79.5 | 366 |
| 5 | 80.0 | 366 |
| 6 | 80.3 | 366 |
| 7 | 100.0 | 266 |
| 8 | 100.0 | 266 |

### 축 `noise` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| noise | 일치% | 프레임 |
|---|---|---|
| 0 | 83.8 | 476 |
| 5 | 75.2 | 238 |
| 10 | 74.8 | 238 |
| 15 | 73.1 | 238 |
| 20 | 69.7 | 476 |
| 25 | 69.7 | 238 |
| 30 | 69.7 | 238 |
| 35 | 68.5 | 238 |
| 40 | 66.8 | 476 |
| 50 | 68.5 | 238 |
| 60 | 69.7 | 238 |

### 축 `persp` — 50% 아래로 내려가는 첫 값: **0.6**

| persp | 일치% | 프레임 |
|---|---|---|
| 0 | 96.6 | 266 |
| 0.1 | 96.6 | 266 |
| 0.2 | 91.0 | 266 |
| 0.3 | 88.7 | 266 |
| 0.4 | 73.3 | 266 |
| 0.5 | 51.1 | 266 |
| 0.6 | 45.5 | 266 |
| 0.7 | 42.5 | 266 |
| 0.8 | 17.3 | 266 |
| 0.9 | 9.4 | 266 |

### 축 `printdefect` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| printdefect | 일치% | 프레임 |
|---|---|---|
| 0 | 93.2 | 280 |
| 0.25 | 92.9 | 280 |
| 0.5 | 92.5 | 280 |
| 0.75 | 93.6 | 280 |
| 1 | 92.5 | 280 |

### 축 `quietzone` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| quietzone | 일치% | 프레임 |
|---|---|---|
| 0 | 93.6 | 280 |
| 0.25 | 92.9 | 280 |
| 0.5 | 92.9 | 280 |
| 0.75 | 93.9 | 280 |
| 1 | 91.4 | 280 |

### 축 `shadow` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| shadow | 일치% | 프레임 |
|---|---|---|
| 0.2 | 92.2 | 294 |
| 0.3 | 100.0 | 294 |
| 0.4 | 100.0 | 294 |
| 0.5 | 100.0 | 294 |
| 0.6 | 100.0 | 294 |
| 0.7 | 100.0 | 294 |
| 0.8 | 100.0 | 294 |
| 0.9 | 100.0 | 294 |
| 1 | 100.0 | 294 |

## 3. 실제 모듈 px별 — 크기 교란을 걷어낸 비교

§3.105: 요청 모듈과 실제 모듈이 다를 수 있어서 태그의 `modpx`를 쓴다.

| 실제 모듈 | 일치% | 프레임 | 오디코딩 |
|---|---|---|---|
| 0~2px | 52.6 | 4983 | 0 |
| 2~3px | 81.1 | 5387 | 0 |
| 3~4px | 77.6 | 2311 | 1 |
| 4~6px | 84.7 | 23518 | 1 |
| 6~8px | 98.7 | 3593 | 0 |
| 8~12px | 98.3 | 418 | 0 |

### 3.1 심볼로지 x 실제 모듈 — 같은 크기에서 비교해야 심볼로지 비교다

| 심볼로지 | 1.5~2.5px | 2.5~3.5px | 3.5~4.5px | 4.5~6.5px | 6.5~9px |
|---|---|---|---|---|---|
| CODABAR | 88.4 | 86.6 | 95.6 | 100.0 | 100.0 |
| CODE128 | 78.4 | 73.5 | 75.5 | 73.7 | 100.0 |
| CODE39 | 83.0 | 86.6 | 85.9 | 100.0 | 100.0 |
| CODE93 | 88.7 | 76.3 | 91.5 | 100.0 | - |
| DATABAR | 83.0 | 76.3 | 93.2 | 95.8 | 93.7 |
| DATABAREXP | 79.1 | 77.8 | 94.7 | 94.7 | 98.7 |
| DATAMATRIX | 19.1 | 68.3 | 69.7 | 100.0 | 100.0 |
| EAN13 | 92.9 | 100.0 | 81.4 | 69.0 | 100.0 |
| EAN8 | 78.8 | 99.6 | 84.4 | 100.0 | 100.0 |
| ITF | 100.0 | 100.0 | 97.6 | 100.0 | 100.0 |
| PDF417 | 50.2 | 53.7 | 65.9 | 64.8 | 96.8 |
| QR | 50.7 | 90.9 | 69.0 | 94.5 | 100.0 |
| UPCA | 89.9 | 100.0 | 84.5 | 100.0 | 100.0 |
| UPCE | 98.1 | 96.2 | 95.5 | 100.0 | 100.0 |
| random | - | - | - | - | - |

## 4. 열화 겹수별 — 남은 격차는 축이 아니라 겹수다 (§3.100)

| 겹수 | 일치% | 프레임 | 평균 ms(x86) | 평균 ms(보드) |
|---|---|---|---|---|
| 0 | 95.6 | 7630 | 22.2 | 178 |
| 1 | 76.9 | 10743 | 49.7 | 397 |
| 2 | 79.2 | 24352 | 80.7 | 646 |
| 3 | 72.8 | 5248 | 57.3 | 458 |
| 4 | 58.4 | 5550 | 80.9 | 648 |
| 5 | 47.7 | 4656 | 89.7 | 718 |
| 6 | 38.4 | 3508 | 67.8 | 542 |
| 7 | 32.6 | 2468 | 46.4 | 371 |
| 8 | 28.8 | 1707 | 28.7 | 229 |
| 9 | 25.2 | 1038 | 18.2 | 146 |
| 10 | 23.5 | 520 | 16.6 | 133 |
| 11 | 21.1 | 207 | 16.5 | 132 |
| 12 | 19.6 | 49 | 11.9 | 95 |
| 13 | 13.2 | 4 | 2.0 | 16 |

## 5. 오디코딩 전수 — 미검출보다 나쁘다, 그래서 한 건씩 적는다

**88건** (프레임 67,680장 중 0.130%)

| 심볼로지 | 격자 | 프레임 | 건수 | 태그 |
|---|---|---|---|---|
| EAN13 | C_noise_blur | sw_blur-2_noise-45_1 | 1 | blur=2;noise=45;sym-EAN_13;modpx-4.00;img-borderline |
| random | H | c000443_1 | 1 | mixed;clutter-label;mod-3px;sym-ITF;rot-free;quietzone;curved;overexpo |
| random | H | c000518_4 | 1 | mixed;clutter-label;mod-3px;sym-ITF;mod-5px+;sym-QR;inverted;sym-EAN13 |
| random | H | c000652_1 | 1 | mixed;mod-5px+;sym-ITF;inverted;printdefect;perspective;shadow;underex |
| random | H | c001571_1 | 1 | mixed;mod-3px;sym-ITF;perspective-strong;quietzone;glare;n1;img-ok |
| random | H | c001696_1 | 1 | mixed;mod-3px;sym-ITF;lowcontrast;rot-small;glare;n1;img-ok |
| random | H | c001977_1 | 1 | mixed;clutter-label;mod-3px;sym-ITF;damaged;rot-small;curved;noise-str |
| random | H | c002951_3 | 1 | mixed;clutter-warehouse;mod-3px;sym-CODE128;lowcontrast;rot-small;mod- |
| random | H | c002962_1 | 1 | mixed;mod-3px;sym-ITF;rot-small;dirty;glare;noise;n1;img-ok |
| random | H | c004158_1 | 1 | mixed;clutter-falsepattern;mod-3px;sym-ITF;inverted;perspective;quietz |
| random | H | c004169_1 | 1 | mixed;clutter-falsepattern;mod-5px+;sym-QR;dpm;overexposed;noise-stron |
| random | H | c004283_1 | 1 | mixed;mod-3px;sym-ITF;lowcontrast;inverted;rot-free;shadow;n1;img-ok |
| random | H | c004729_1 | 1 | mixed;clutter-label;mod-3px;sym-ITF;damaged;rot-ortho;quietzone;curved |
| random | H | c005112_1 | 1 | mixed;mod-3px;sym-EAN13;perspective;curved;glare;sym-EAN_13;n1;img-ok |
| random | H | c005298_1 | 1 | mixed;clutter-falsepattern;mod-3px;sym-EAN13;inverted;dirty;shadow;und |
| random | H | c005437_1 | 1 | mixed;clutter-falsepattern;mod-5px+;sym-ITF;shadow;n1;img-ok |
| random | H | c005665_5 | 1 | mixed;clutter-falsepattern;mod-3px;sym-ITF;lowcontrast;printdefect;sym |
| random | H | c006830_2 | 1 | mixed;mod-3px;sym-ITF;rot-ortho;printdefect;dirty;shadow;n2;img-ok |
| random | H | c007962_2 | 1 | mixed;clutter-warehouse;mod-3px;sym-ITF;lowcontrast;rot-free;dirty;cur |
| random | H | c000020_2 | 1 | mixed;mod-3px;sym-ITF;lowcontrast;rot-free;shadow;n2;img-ok |
| random | H | c000525_2 | 1 | mixed;clutter-label;mod-5px+;sym-CODE128;rot-free;perspective;mod-3px; |
| random | H | c001894_9 | 1 | mixed;clutter-falsepattern;mod-3px;sym-ITF;rot-small;sym-CODE128;rot-f |
| random | H | c003697_1 | 1 | mixed;clutter-falsepattern;mod-5px+;sym-QR;printdefect;curved;sym-QR_C |
| random | H | c004272_1 | 1 | mixed;mod-3px;sym-ITF;rot-free;perspective;glare;shadow;n1;img-ok |
| random | H | c006253_3 | 1 | mixed;clutter-label;mod-3px;sym-ITF;printdefect;damaged;rot-small;mod- |
| random | H | c007424_7 | 1 | mixed;mod-5px+;sym-QR;lowcontrast;mod-3px;sym-EAN13;inverted;rot-small |
| random | H | c009071_6 | 1 | mixed;mod-5px+;sym-QR;dpm;mod-3px;sym-CODE128;lowcontrast;rot-small;sy |
| random | H | c000532_1 | 1 | mixed;clutter-falsepattern;mod-3px;sym-CODE39;lowcontrast;dirty;undere |
| random | H | c000805_1 | 2 | mixed;mod-3px;sym-ITF;lowcontrast;rot-small;overexposed;n1;img-ok |
| random | H | c000831_1 | 1 | mixed;clutter-label;mod-3px;sym-ITF;printdefect;rot-small;shadow;overe |
| random | H | c001404_6 | 1 | mixed;mod-3px;sym-ITF;mod-5px+;sym-EAN13;printdefect;damaged;sym-CODE1 |
| random | H | c001450_1 | 1 | mixed;mod-3px;sym-ITF;lowcontrast;rot-free;glare;n1;img-ok |
| random | H | c001468_3 | 1 | mixed;clutter-falsepattern;mod-5px+;sym-CODE128;inverted;damaged;sym-Q |
| random | H | c001921_2 | 1 | mixed;mod-3px;sym-ITF;sym-CODE128;rot-small;curved;shadow;underexposed |
| random | H | c002411_2 | 1 | mixed;clutter-warehouse;mod-3px;sym-ITF;inverted;rot-ortho;lowcontrast |
| random | H | c003685_2 | 1 | mixed;mod-3px;sym-CODE39;lowcontrast;rot-free;sym-ITF;curved;sym-CODE_ |
| random | H | c004172_2 | 1 | mixed;mod-3px;sym-ITF;sym-CODE128;lowcontrast;quietzone;dirty;glare;sy |
| random | H | c004179_1 | 1 | mixed;clutter-falsepattern;mod-3px;sym-CODE128;shadow;sym-CODE_128;n1; |
| random | H | c005484_2 | 1 | mixed;clutter-falsepattern;mod-3px;sym-CODE128;sym-CODE39;curved;under |
| random | H | c005558_3 | 1 | mixed;clutter-label;mod-5px+;sym-EAN13;sym-ITF;rot-ortho;rot-small;per |
| random | H | c005718_1 | 1 | mixed;mod-3px;sym-ITF;rot-ortho;shadow;n1;img-ok |
| random | H | c006697_1 | 1 | mixed;mod-3px;sym-ITF;rot-small;glare;shadow;n1;img-ok |
| random | H | c006950_3 | 1 | mixed;clutter-warehouse;mod-3px;sym-QR;sym-ITF;rot-free;damaged;rot-sm |
| random | H | c007294_1 | 1 | mixed;clutter-falsepattern;mod-5px+;sym-QR;dirty;underexposed;sym-QR_C |
| random | H | c007390_2 | 1 | mixed;clutter-falsepattern;mod-3px;sym-EAN13;rot-small;perspective;mod |
| random | H | c007469_1 | 1 | mixed;clutter-falsepattern;mod-5px+;sym-ITF;damaged;rot-small;quietzon |
| random | H | c007760_8 | 1 | mixed;mod-5px+;sym-QR;inverted;rot-ortho;mod-3px;sym-ITF;perspective;r |
| random | H | c008032_1 | 1 | mixed;clutter-falsepattern;mod-5px+;sym-CODE128;perspective-strong;cur |
| random | H | c008994_1 | 1 | mixed;clutter-falsepattern;mod-3px;sym-QR;shadow;underexposed;sym-QR_C |
| random | H | c000481_1 | 1 | mixed;clutter-falsepattern;mod-3px;sym-ITF;rot-free;glare;overexposed; |
| random | H | c000520_1 | 1 | mixed;clutter-warehouse;mod-3px;sym-ITF;rot-small;shadow;underexposed; |
| random | H | c000700_1 | 1 | mixed;mod-3px;sym-EAN13;overexposed;sym-EAN_13;n1;img-ok |
| random | H | c001151_8 | 1 | mixed;clutter-falsepattern;mod-5px+;sym-QR;damaged;perspective-strong; |
| random | H | c001473_2 | 1 | mixed;mod-3px;sym-QR;inverted;rot-free;sym-ITF;lowcontrast;perspective |
| random | H | c001569_1 | 1 | mixed;clutter-falsepattern;mod-3px;sym-CODE128;printdefect;rot-small;q |
| random | H | c002434_1 | 1 | mixed;mod-3px;sym-ITF;lowcontrast;rot-small;shadow;overexposed;n1;img- |
| random | H | c002564_1 | 1 | mixed;mod-5px+;sym-QR;rot-ortho;curved;glare;sym-QR_CODE;n1;img-ok |
| random | H | c003611_1 | 1 | mixed;clutter-falsepattern;mod-5px+;sym-ITF;lowcontrast;perspective;un |
| random | H | c003705_1 | 1 | mixed;clutter-falsepattern;mod-3px;sym-ITF;lowcontrast;inverted;damage |
| random | H | c005326_2 | 1 | mixed;clutter-falsepattern;mod-5px+;sym-QR;dpm;damaged;perspective-str |

(위 60건만 표시 — 전체 88건)

## 6. 시간 — x86 실측과 보드 환산

보드(i.MX8MP)는 x86의 약 8배 느리다(§3.60). **두 값을 같이 적는다.**

| 심볼로지 | 평균(x86) | p95(x86) | 평균(보드) | p95(보드) |
|---|---|---|---|---|
| CODABAR | 67.6 | 478.0 | 541 | 3824 |
| CODE128 | 92.3 | 397.8 | 738 | 3182 |
| CODE39 | 83.7 | 518.5 | 670 | 4148 |
| CODE93 | 91.3 | 529.5 | 730 | 4236 |
| DATABAR | 80.0 | 412.3 | 640 | 3299 |
| DATABAREXP | 99.3 | 612.1 | 794 | 4897 |
| DATAMATRIX | 49.2 | 173.4 | 394 | 1387 |
| EAN13 | 48.2 | 220.6 | 386 | 1765 |
| EAN8 | 39.3 | 173.5 | 314 | 1388 |
| ITF | 38.3 | 215.3 | 306 | 1722 |
| PDF417 | 94.9 | 321.5 | 759 | 2572 |
| QR | 45.7 | 163.0 | 366 | 1304 |
| UPCA | 43.2 | 191.6 | 345 | 1532 |
| UPCE | 36.9 | 194.9 | 295 | 1559 |
| random | 60.3 | 325.1 | 482 | 2601 |

---

## 7. 분석 — 오디코딩 88건을 한 건씩 뜯어봤다

이 저장소의 우선순위는 **미검출 < 오디코딩**이다. 검출률 56.9%는 격자에
물리적으로 불가능한 모서리를 일부러 넣은 결과라 그 자체로는 나쁜 숫자가
아니지만, **오디코딩 88건은 그냥 나쁜 숫자다.** 그래서 88건을 전부
재생성해서 어떤 심볼로지가 무엇을 잘못 냈는지 직접 확인했다.

### 7.1 어디에 몰려 있나

| 격자 | 프레임 | 오디코딩 |
|---|---:|---:|
| H (난수 혼합) | 27,470 | **89** |
| C 노이즈x블러 / EAN13 | 221 | 1 |
| S1 작은모듈x대비 / DATABAREXP | 323 | 1 |
| 나머지 격자 전부 | 39,666 | **0** |

**격자는 깨끗하고 난수 혼합만 더럽다.** 둘의 차이는 두 가지다 —
난수는 (1) 한 프레임에 코드가 여럿이고 (2) 열화가 겹친다. 격자는
축 두 개만 곱한 코드 1개짜리다.

### 7.2 유령을 낸 심볼로지와 그 정체

86개 프레임을 재생성해 디코더 출력을 정답과 대조했다.

| 심볼로지 | 부분 스캔인가 | 진짜 코드도 나왔나 | 건수 |
|---|---|---|---:|
| ITF | 예 (정답의 부분 문자열) | **아니오** | **34** |
| EAN/UPC | 아니오 (진짜 오독) | - | **25** |
| ITF | 아니오 (진짜 오독) | - | **21** |
| Code128 | 예 | 아니오 | 3 |
| MicroQR | 아니오 | - | 2 |
| Code128 | 아니오 | - | 1 |
| ITF | 예 | 예 | **1** |

부분 스캔의 실제 모습:

```
c000443_1   ITF '681731'          <- 정답 '867935681731' 의 뒤 6자리
c001571_1   ITF '31803327'        <- 정답 '7631803327'   의 뒤 8자리
c001696_1   ITF '632847406755'    <- 정답 '66632847406755' 의 뒤 12자리
c001977_1   ITF '664416'          <- 정답 '855807664416' 의 뒤 6자리
```

전부 **정답의 꼬리**다. 바 열을 끝까지 못 훑고 중간부터 읽은 것이다.
ITF는 체크디짓이 규격상 선택이라 잘린 조각도 문법에 맞는 유효한 코드가
된다.

### 7.3 여기서 나온 가장 중요한 사실

**기존 dedup은 제 몫을 하고 있다.** 부분 스캔을 지우는 규칙들
(`textContains` + 기하 겹침, 밴드 규칙 네 가지)이 잡을 수 있는 경우는
**87건 중 1건뿐**이었다. 나머지는 전부 **진짜 코드가 아예 안 나온**
프레임이라, 대조할 상대가 없어서 dedup이 원리적으로 개입할 수 없다.

즉 "dedup 규칙을 더 넣자"는 방향은 값이 거의 없다. 남은 88건을 줄이려면
**단독으로 서 있는 결과 자체의 타당성**을 봐야 한다.

### 7.4 축은 어디서 끊기는가

| 축 | 50%를 지나는 값 | 읽는 법 |
|---|---|---|
| `module` | **1.125px** | 1px 아래는 물리적으로 표본이 없다. 벽이 여기다. |
| `blur` | **2.75** (시그마) | 모듈 4px에 시그마 2.75면 이미 코드가 아니다. |
| `persp` | **0.6** | 원근이 남은 축 중 가장 이르게 끊긴다. |
| `damaged` | **0.75** | 오류정정 용량 밖이라 정상. |
| `contrast` | **0.05** | 대비 0.05는 SNR 벽(§3.61). |
| angle / noise / glare / shadow / dirty / quietzone / printdefect / invert / dpm / curve / count | 없음 | **끝까지 버틴다.** |

훑은 축 17개 중 **11개가 끝까지 안 무너진다.** 남은 다섯 개도 물리
한계에 붙어 있다. 그래서 다음 개선은 축이 아니라 아래 두 곳이다.

### 7.5 겹수가 여전히 지배한다 (§3.100 재확인)

| 겹수 | 0 | 1 | 2 | 4 | 6 | 8 | 10 | 12 |
|---|---|---|---|---|---|---|---|---|
| 일치% | 95.6 | 76.9 | 79.2 | 58.4 | 38.4 | 28.8 | 23.5 | 19.6 |

단일 축은 다 버티는데 **겹치면 무너진다.** 그리고 오디코딩 88건도 전부
겹수가 높은 난수 코퍼스에 있다. 두 현상이 같은 자리를 가리킨다.

### 7.6 크기 교란을 걷어낸 심볼로지 비교

§3.105를 고친 덕에 이번에는 **같은 실제 모듈 px에서** 비교할 수 있다.
2.5~3.5px 구간(현장에서 가장 흔한 구간)만 뽑으면:

| 잘 되는 쪽 | | 안 되는 쪽 | |
|---|---:|---|---:|
| ITF | 100.0 | PDF417 | **53.7** |
| EAN13 / UPCA | 100.0 | DATAMATRIX | **68.3** |
| UPCE | 96.2 | CODE128 | **73.5** |
| CODE39 / CODABAR | 86.6 | CODE93 | 76.3 |

**PDF417이 이 구간에서 가장 약하다**(53.7%). 2D인데 QR(90.9%)의 절반을
조금 넘는다. 이건 크기 교란이 아니라 진짜 차이다.

---

## 8. 다음 개선 대상 — 이 보고서가 가리키는 곳

우선순위는 **오디코딩 > 검출**이다.

### [1] 단독으로 선 1D 결과의 **정지대 확인** (오디코딩 37건 겨냥)

부분 스캔 37건(ITF 34 + Code128 3)은 전부 "진짜 코드는 못 읽고 조각만
낸" 경우다. 런타임에 정답을 모르지만 **물리 신호는 있다** — 진짜 1D
코드는 양 끝에 정지대(quiet zone, 좁은 요소의 10배)가 있고, **부분
스캔은 잘린 자리 바로 바깥에 바가 계속 있다.**

그래서 이렇게 본다:

> 선형 코드 결과의 꼭짓점 양 끝 바깥을 코드 축 방향으로 들여다본다.
> 정지대가 아니라 바 같은 변조가 있으면 그것은 부분 스캔이다.

규격에 있는 성질이라 코퍼스에 맞춘 문턱이 아니다. 대가는 프레임 가장자리
코드와 잡동사니가 바짝 붙은 코드다 — 넣기 전에 실측한다.

### [2] 낮은 SNR에서 1D의 **다중 읽기 합의** (오디코딩 49건 겨냥)

부분 스캔이 아닌 49건(EAN/UPC 25, ITF 21, …)은 진짜 오독이다. EAN13은
체크디짓이 필수인데도 통과했다 — 체크디짓 하나는 무작위 오류의 1/10만
막는다. 예: 정답 `4907304703100` -> `4907304713109`.

산업용 리더기가 쓰는 방법은 **여러 스캔 라인/패스가 같은 값을 낼 때만
받는 것**이다. 이 파이프라인은 이미 패스를 여러 개 돌리므로, 낮은 품질
구간에서만 합의를 요구하면 지연 없이 얹을 수 있다.

### [3] PDF417의 작은 모듈 (검출 겨냥)

같은 2.5~3.5px에서 PDF417 53.7% / QR 90.9%. PDF417은 행 높이가 낮아
행 동기화가 먼저 깨진다는 가설이 있으나 **아직 안 쟀다.**

### [4] 원근 0.6 (검출 겨냥)

끊기는 다섯 축 중 물리 한계가 아닌 유일한 축이다. §3.29에서 한 번
건드렸고 그때 리프트 1.50을 얻었다. 지금 격자에서 `persp` 0.6이 벽이다.
