<!-- 사이클 1. tools/fulltest_100k.sh && python3 tools/fulltest_report.py fulltest-out -->

# vscan-lite 풀테스트 보고서

프레임 87,826장 / 코드 142,238개 / 심볼로지 15종 / 격자 12종

**텍스트 일치 61.6%** (87,548/142,238) · **오디코딩 91건**

시간(x86 실측): 평균 136.3ms · p95 725.2ms
  → 보드(i.MX8MP) 환산 x8: 평균 1090ms · p95 5801ms

> 격자에는 물리적으로 불가능한 모서리를 **일부러** 넣는다. 검출률의 절대값이 아니라 심볼로지·축 사이의 **차이**를 볼 것.

## 1. 심볼로지 x 격자 (텍스트 일치 %)

| 심볼로지 | contrast_bright | module_angle | noise_blur | persp_curve | glare_shadow | dirty_damaged | zone_print | H | tinymod_contrast | tinymod_noise | mod_density | res_module | 전체 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| CODABAR | 96.2 | 100.0 | 100.0 | 44.6 | 88.5 | 100.0 | 99.5 | - | 99.1 | 100.0 | - | - | **92.7** |
| CODE128 | 96.6 | 100.0 | 78.1 | 52.6 | 97.9 | 83.3 | 85.5 | - | 80.2 | 65.8 | 59.6 | 95.2 | **79.5** |
| CODE39 | 96.1 | 100.0 | 78.1 | 44.9 | 98.3 | 87.1 | 100.0 | - | 95.4 | 73.3 | - | - | **87.9** |
| CODE93 | 97.2 | 100.0 | 100.0 | 47.4 | 99.2 | 67.1 | 75.0 | - | 99.7 | 100.0 | - | - | **86.9** |
| DATABAR | 96.6 | 100.0 | 100.0 | 30.5 | 79.4 | 61.6 | 92.0 | - | 99.4 | 100.0 | - | - | **81.6** |
| DATABAREXP | 94.8 | 100.0 | 100.0 | 28.3 | 100.0 | 95.2 | 67.5 | - | 99.7 | 100.0 | - | - | **90.5** |
| DATAMATRIX | 92.3 | 91.8 | 59.7 | 100.0 | 99.9 | 14.9 | 100.0 | - | 40.2 | 17.1 | 74.6 | 81.0 | **69.5** |
| EAN13 | 96.1 | 99.8 | 78.3 | 83.1 | 100.0 | 79.0 | 76.0 | - | 75.9 | 70.6 | 74.3 | 95.2 | **84.1** |
| EAN8 | 95.2 | 99.8 | 81.2 | 95.0 | 100.0 | 70.7 | 87.5 | - | 70.3 | 71.7 | - | - | **86.6** |
| ITF | 97.5 | 100.0 | 100.0 | 65.1 | 98.3 | 94.7 | 97.5 | - | 96.9 | 100.0 | - | - | **94.9** |
| PDF417 | 94.7 | 60.8 | 63.1 | 29.1 | 100.0 | 60.4 | 84.5 | - | 60.4 | 22.5 | 52.6 | 88.1 | **65.6** |
| QR | 90.8 | 96.0 | 67.9 | 87.5 | 100.0 | 14.6 | 100.0 | - | 44.3 | 28.9 | 92.0 | 85.7 | **74.4** |
| UPCA | 96.2 | 99.8 | 79.2 | 84.2 | 100.0 | 78.2 | 83.5 | - | 81.4 | 71.7 | - | - | **87.9** |
| UPCE | 96.1 | 100.0 | 100.0 | 76.5 | 100.0 | 88.2 | 100.0 | - | 91.0 | 100.0 | - | - | **94.2** |
| random | - | - | - | - | - | - | - | 42.8 | - | - | - | - | **42.8** |

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

### 축 `blur` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| blur | 일치% | 프레임 |
|---|---|---|
| 0 | 98.4 | 364 |
| 0.5 | 98.6 | 364 |
| 1 | 98.6 | 364 |
| 1.5 | 94.8 | 364 |
| 2 | 90.1 | 364 |
| 2.5 | 75.0 | 364 |
| 3 | 70.6 | 364 |
| 3.5 | 69.2 | 364 |
| 4 | 65.1 | 364 |

### 축 `bright` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| bright | 일치% | 프레임 |
|---|---|---|
| 0.3 | 83.0 | 560 |
| 0.5 | 90.5 | 560 |
| 0.7 | 93.0 | 560 |
| 0.9 | 96.1 | 560 |
| 1.1 | 97.1 | 560 |
| 1.3 | 99.6 | 560 |
| 1.5 | 100.0 | 560 |
| 1.7 | 99.8 | 560 |

### 축 `contrast` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| contrast | 일치% | 프레임 |
|---|---|---|
| 0.05 | 52.7 | 448 |
| 0.15 | 79.7 | 686 |
| 0.25 | 87.5 | 686 |
| 0.35 | 91.7 | 686 |
| 0.45 | 94.5 | 686 |
| 0.55 | 95.6 | 686 |
| 0.65 | 96.1 | 686 |
| 0.75 | 95.9 | 686 |
| 0.85 | 96.4 | 686 |
| 0.95 | 96.1 | 686 |

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

### 축 `damaged` — 50% 아래로 내려가는 첫 값: **0.85**

| damaged | 일치% | 프레임 |
|---|---|---|
| 0 | 92.5 | 588 |
| 0.1 | 92.7 | 588 |
| 0.2 | 91.3 | 588 |
| 0.3 | 81.5 | 588 |
| 0.4 | 76.0 | 588 |
| 0.5 | 84.5 | 588 |
| 0.6 | 70.9 | 588 |
| 0.7 | 66.5 | 588 |
| 0.8 | 54.8 | 588 |
| 0.9 | 37.1 | 588 |
| 1 | 19.4 | 588 |

### 축 `dirty` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| dirty | 일치% | 프레임 |
|---|---|---|
| 0 | 76.7 | 588 |
| 0.1 | 76.4 | 588 |
| 0.2 | 75.2 | 588 |
| 0.3 | 74.5 | 588 |
| 0.4 | 73.1 | 588 |
| 0.5 | 71.4 | 588 |
| 0.6 | 69.0 | 588 |
| 0.7 | 68.0 | 588 |
| 0.8 | 66.7 | 588 |
| 0.9 | 65.6 | 588 |
| 1 | 63.8 | 588 |

### 축 `dpm` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| dpm | 일치% | 프레임 |
|---|---|---|
| 0 | 100.0 | 1400 |
| 1 | 78.4 | 1400 |

### 축 `glare` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| glare | 일치% | 프레임 |
|---|---|---|
| 0 | 97.5 | 476 |
| 0.1 | 96.8 | 476 |
| 0.2 | 97.3 | 476 |
| 0.3 | 97.7 | 476 |
| 0.4 | 97.5 | 476 |
| 0.5 | 97.5 | 476 |
| 0.6 | 97.5 | 476 |
| 0.7 | 97.1 | 476 |
| 0.8 | 97.3 | 476 |
| 0.9 | 97.1 | 476 |
| 1 | 97.3 | 476 |

### 축 `invert` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| invert | 일치% | 프레임 |
|---|---|---|
| 0 | 96.4 | 1400 |
| 1 | 82.0 | 1400 |

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
| 0 | 89.1 | 714 |
| 5 | 87.2 | 476 |
| 10 | 86.8 | 476 |
| 15 | 86.3 | 476 |
| 20 | 79.4 | 714 |
| 25 | 84.5 | 476 |
| 30 | 83.8 | 476 |
| 35 | 83.0 | 476 |
| 40 | 76.8 | 714 |
| 50 | 83.2 | 476 |
| 60 | 84.0 | 476 |

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
| 0 | 89.3 | 560 |
| 0.25 | 88.9 | 560 |
| 0.5 | 89.1 | 560 |
| 0.75 | 89.6 | 560 |
| 1 | 88.9 | 560 |

### 축 `quietzone` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| quietzone | 일치% | 프레임 |
|---|---|---|
| 0 | 88.9 | 560 |
| 0.25 | 89.8 | 560 |
| 0.5 | 88.8 | 560 |
| 0.75 | 89.8 | 560 |
| 1 | 88.6 | 560 |

### 축 `shadow` — 50% 아래로 내려가는 첫 값: **없음(끝까지 버팀)**

| shadow | 일치% | 프레임 |
|---|---|---|
| 0.2 | 88.8 | 588 |
| 0.3 | 94.4 | 588 |
| 0.4 | 94.6 | 588 |
| 0.5 | 95.4 | 588 |
| 0.6 | 100.0 | 588 |
| 0.7 | 100.0 | 588 |
| 0.8 | 100.0 | 588 |
| 0.9 | 100.0 | 588 |
| 1 | 100.0 | 588 |

## 3. 실제 모듈 px별 — 크기 교란을 걷어낸 비교

§3.105: 요청 모듈과 실제 모듈이 다를 수 있어서 태그의 `modpx`를 쓴다.

| 실제 모듈 | 일치% | 프레임 | 오디코딩 |
|---|---|---|---|
| 0~2px | 52.6 | 4983 | 0 |
| 2~3px | 81.1 | 5387 | 0 |
| 3~4px | 77.6 | 2311 | 1 |
| 4~6px | 84.7 | 23518 | 1 |
| 6~8px | 94.4 | 7910 | 0 |
| 8~12px | 87.1 | 16247 | 0 |

### 3.1 심볼로지 x 실제 모듈 — 같은 크기에서 비교해야 심볼로지 비교다

| 심볼로지 | 1.5~2.5px | 2.5~3.5px | 3.5~4.5px | 4.5~6.5px | 6.5~9px |
|---|---|---|---|---|---|
| CODABAR | 88.4 | 86.6 | 95.6 | 100.0 | 94.2 |
| CODE128 | 78.4 | 73.5 | 75.5 | 73.7 | 97.3 |
| CODE39 | 83.0 | 86.6 | 85.9 | 100.0 | 95.0 |
| CODE93 | 88.7 | 76.3 | 91.5 | 87.0 | - |
| DATABAR | 83.0 | 76.3 | 93.2 | 95.8 | 73.2 |
| DATABAREXP | 79.1 | 77.8 | 94.7 | 94.7 | 94.7 |
| DATAMATRIX | 19.1 | 68.3 | 69.7 | 100.0 | 76.7 |
| EAN13 | 92.9 | 100.0 | 81.4 | 69.0 | 92.7 |
| EAN8 | 78.8 | 99.6 | 84.4 | 100.0 | 91.1 |
| ITF | 100.0 | 100.0 | 97.6 | 100.0 | 97.4 |
| PDF417 | 50.2 | 53.7 | 65.9 | 64.8 | 85.1 |
| QR | 50.7 | 90.9 | 69.0 | 94.5 | 78.5 |
| UPCA | 89.9 | 100.0 | 84.5 | 100.0 | 94.0 |
| UPCE | 98.1 | 96.2 | 95.5 | 100.0 | 96.4 |
| random | - | - | - | - | - |

## 4. 열화 겹수별 — 남은 격차는 축이 아니라 겹수다 (§3.100)

| 겹수 | 일치% | 프레임 | 평균 ms(x86) | 평균 ms(보드) |
|---|---|---|---|---|
| 0 | 95.6 | 7700 | 42.1 | 337 |
| 1 | 78.9 | 12815 | 102.4 | 819 |
| 2 | 82.5 | 41572 | 183.2 | 1466 |
| 3 | 74.2 | 5809 | 120.0 | 960 |
| 4 | 58.9 | 5773 | 148.0 | 1184 |
| 5 | 48.3 | 4656 | 124.5 | 996 |
| 6 | 39.0 | 3508 | 93.1 | 744 |
| 7 | 33.1 | 2468 | 61.4 | 492 |
| 8 | 29.1 | 1707 | 39.2 | 314 |
| 9 | 25.6 | 1038 | 24.6 | 197 |
| 10 | 23.6 | 520 | 22.3 | 178 |
| 11 | 21.1 | 207 | 18.7 | 149 |
| 12 | 19.8 | 49 | 17.7 | 142 |
| 13 | 13.2 | 4 | 2.4 | 19 |

## 5. 오디코딩 전수 — 미검출보다 나쁘다, 그래서 한 건씩 적는다

**87건** (프레임 87,826장 중 0.099%)

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
| random | H | c006830_2 | 1 | mixed;mod-3px;sym-ITF;rot-ortho;printdefect;dirty;shadow;n2;img-ok |
| random | H | c007962_2 | 1 | mixed;clutter-warehouse;mod-3px;sym-ITF;lowcontrast;rot-free;dirty;cur |
| random | H | c000020_2 | 1 | mixed;mod-3px;sym-ITF;lowcontrast;rot-free;shadow;n2;img-ok |
| random | H | c001894_9 | 1 | mixed;clutter-falsepattern;mod-3px;sym-ITF;rot-small;sym-CODE128;rot-f |
| random | H | c003697_1 | 1 | mixed;clutter-falsepattern;mod-5px+;sym-QR;printdefect;curved;sym-QR_C |
| random | H | c004272_1 | 1 | mixed;mod-3px;sym-ITF;rot-free;perspective;glare;shadow;n1;img-ok |
| random | H | c005630_1 | 1 | mixed;clutter-warehouse;mod-3px;sym-ITF;lowcontrast;noise-strong;n1;im |
| random | H | c007424_7 | 1 | mixed;mod-5px+;sym-QR;lowcontrast;mod-3px;sym-EAN13;inverted;rot-small |
| random | H | c009071_6 | 1 | mixed;mod-5px+;sym-QR;dpm;mod-3px;sym-CODE128;lowcontrast;rot-small;sy |
| random | H | c000532_1 | 1 | mixed;clutter-falsepattern;mod-3px;sym-CODE39;lowcontrast;dirty;undere |
| random | H | c000805_1 | 2 | mixed;mod-3px;sym-ITF;lowcontrast;rot-small;overexposed;n1;img-ok |
| random | H | c000831_1 | 1 | mixed;clutter-label;mod-3px;sym-ITF;printdefect;rot-small;shadow;overe |
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
| random | H | c005431_1 | 1 | mixed;mod-5px+;sym-CODE128;perspective-strong;quietzone;sym-CODE_128;n |
| random | H | c005838_2 | 1 | mixed;clutter-label;mod-5px+;sym-EAN13;inverted;rot-free;mod-3px;sym-Q |
| random | H | c006231_2 | 1 | mixed;clutter-falsepattern;mod-5px+;sym-ITF;rot-small;mod-3px;sym-QR;p |

(위 60건만 표시 — 전체 87건)

## 6. 시간 — x86 실측과 보드 환산

보드(i.MX8MP)는 x86의 약 8배 느리다(§3.60). **두 값을 같이 적는다.**

| 심볼로지 | 평균(x86) | p95(x86) | 평균(보드) | p95(보드) |
|---|---|---|---|---|
| CODABAR | 158.4 | 1160.2 | 1267 | 9282 |
| CODE128 | 164.9 | 885.6 | 1319 | 7085 |
| CODE39 | 204.0 | 1210.6 | 1632 | 9685 |
| CODE93 | 254.0 | 1469.1 | 2032 | 11753 |
| DATABAR | 247.5 | 1252.8 | 1980 | 10022 |
| DATABAREXP | 371.6 | 2612.7 | 2973 | 20901 |
| DATAMATRIX | 96.6 | 347.8 | 773 | 2783 |
| EAN13 | 99.0 | 502.9 | 792 | 4023 |
| EAN8 | 83.4 | 395.5 | 667 | 3164 |
| ITF | 113.4 | 649.7 | 907 | 5198 |
| PDF417 | 177.5 | 761.9 | 1420 | 6095 |
| QR | 85.4 | 313.2 | 683 | 2506 |
| UPCA | 91.5 | 431.7 | 732 | 3453 |
| UPCE | 109.2 | 671.0 | 873 | 5368 |
| random | 83.3 | 442.1 | 666 | 3537 |
