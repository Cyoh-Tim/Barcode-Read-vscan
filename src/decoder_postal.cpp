#include "vscan_internal/decoder_postal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <cstdio>

namespace vscan {
namespace {

/* ---------------------------------------------------------------------------
 * 일본우편 고객 바코드 표
 * BWIPP의 japanpost 인코더를 ghostscript로 덤프해서 얻었다.
 * 각 심볼은 막대 3개, 각 막대는 0=tracker 1=descender 2=ascender 3=full.
 * ------------------------------------------------------------------------- */
const char* kJpEnc[19] = {
    "300", "330", "312", "132", "321", "303", "123", "231", "213", "033",  // 0~9
    "030",                                                                  // 10 = '-'
    "120", "102", "210",                                                    // 11~13 = 제어(A~J/K~T/U~Z)
    "012", "201", "021", "003", "333",                                      // 14~18 (14는 채움)
};
const char* kJpStart = "31";
const char* kJpStop  = "13";
constexpr int kJpSymbols = 21;                 // 데이터 20 + 검사 1
constexpr int kJpBars = 2 + kJpSymbols * 3 + 2; // 67

// USPS Intelligent Mail(IMB) 표 — BWIPP의 onecode 인코더를 ghostscript로
// 덤프해서 얻었다(§3.50/§3.51과 같은 방법). 손으로 고치지 말 것.
// tab513: 코드워드 0~1286 -> 13비트 문자(1이 5개)
// tab213: 코드워드 1287~1364 -> 13비트 문자(1이 2개)
// barmap: 막대 65개 x {디센더 문자, 비트, 어센더 문자, 비트}
const uint16_t kImbTab513[1287] = {
    31, 7936, 47, 7808, 55, 7552, 59, 7040, 61, 6016, 62, 3968,
    79, 7744, 87, 7488, 91, 6976, 93, 5952, 94, 3904, 103, 7360,
    107, 6848, 109, 5824, 110, 3776, 115, 6592, 117, 5568, 118, 3520,
    121, 5056, 122, 3008, 124, 1984, 143, 7712, 151, 7456, 155, 6944,
    157, 5920, 158, 3872, 167, 7328, 171, 6816, 173, 5792, 174, 3744,
    179, 6560, 181, 5536, 182, 3488, 185, 5024, 186, 2976, 188, 1952,
    199, 7264, 203, 6752, 205, 5728, 206, 3680, 211, 6496, 213, 5472,
    214, 3424, 217, 4960, 218, 2912, 220, 1888, 227, 6368, 229, 5344,
    230, 3296, 233, 4832, 234, 2784, 236, 1760, 241, 4576, 242, 2528,
    244, 1504, 248, 992, 271, 7696, 279, 7440, 283, 6928, 285, 5904,
    286, 3856, 295, 7312, 299, 6800, 301, 5776, 302, 3728, 307, 6544,
    309, 5520, 310, 3472, 313, 5008, 314, 2960, 316, 1936, 327, 7248,
    331, 6736, 333, 5712, 334, 3664, 339, 6480, 341, 5456, 342, 3408,
    345, 4944, 346, 2896, 348, 1872, 355, 6352, 357, 5328, 358, 3280,
    361, 4816, 362, 2768, 364, 1744, 369, 4560, 370, 2512, 372, 1488,
    376, 976, 391, 7216, 395, 6704, 397, 5680, 398, 3632, 403, 6448,
    405, 5424, 406, 3376, 409, 4912, 410, 2864, 412, 1840, 419, 6320,
    421, 5296, 422, 3248, 425, 4784, 426, 2736, 428, 1712, 433, 4528,
    434, 2480, 436, 1456, 440, 944, 451, 6256, 453, 5232, 454, 3184,
    457, 4720, 458, 2672, 460, 1648, 465, 4464, 466, 2416, 468, 1392,
    472, 880, 481, 4336, 482, 2288, 484, 1264, 488, 752, 527, 7688,
    535, 7432, 539, 6920, 541, 5896, 542, 3848, 551, 7304, 555, 6792,
    557, 5768, 558, 3720, 563, 6536, 565, 5512, 566, 3464, 569, 5000,
    570, 2952, 572, 1928, 583, 7240, 587, 6728, 589, 5704, 590, 3656,
    595, 6472, 597, 5448, 598, 3400, 601, 4936, 602, 2888, 604, 1864,
    611, 6344, 613, 5320, 614, 3272, 617, 4808, 618, 2760, 620, 1736,
    625, 4552, 626, 2504, 628, 1480, 632, 968, 647, 7208, 651, 6696,
    653, 5672, 654, 3624, 659, 6440, 661, 5416, 662, 3368, 665, 4904,
    666, 2856, 668, 1832, 675, 6312, 677, 5288, 678, 3240, 681, 4776,
    682, 2728, 684, 1704, 689, 4520, 690, 2472, 692, 1448, 696, 936,
    707, 6248, 709, 5224, 710, 3176, 713, 4712, 714, 2664, 716, 1640,
    721, 4456, 722, 2408, 724, 1384, 728, 872, 737, 4328, 738, 2280,
    740, 1256, 775, 7192, 779, 6680, 781, 5656, 782, 3608, 787, 6424,
    789, 5400, 790, 3352, 793, 4888, 794, 2840, 796, 1816, 803, 6296,
    805, 5272, 806, 3224, 809, 4760, 810, 2712, 812, 1688, 817, 4504,
    818, 2456, 820, 1432, 824, 920, 835, 6232, 837, 5208, 838, 3160,
    841, 4696, 842, 2648, 844, 1624, 849, 4440, 850, 2392, 852, 1368,
    865, 4312, 866, 2264, 868, 1240, 899, 6200, 901, 5176, 902, 3128,
    905, 4664, 906, 2616, 908, 1592, 913, 4408, 914, 2360, 916, 1336,
    929, 4280, 930, 2232, 932, 1208, 961, 4216, 962, 2168, 964, 1144,
    1039, 7684, 1047, 7428, 1051, 6916, 1053, 5892, 1054, 3844, 1063, 7300,
    1067, 6788, 1069, 5764, 1070, 3716, 1075, 6532, 1077, 5508, 1078, 3460,
    1081, 4996, 1082, 2948, 1084, 1924, 1095, 7236, 1099, 6724, 1101, 5700,
    1102, 3652, 1107, 6468, 1109, 5444, 1110, 3396, 1113, 4932, 1114, 2884,
    1116, 1860, 1123, 6340, 1125, 5316, 1126, 3268, 1129, 4804, 1130, 2756,
    1132, 1732, 1137, 4548, 1138, 2500, 1140, 1476, 1159, 7204, 1163, 6692,
    1165, 5668, 1166, 3620, 1171, 6436, 1173, 5412, 1174, 3364, 1177, 4900,
    1178, 2852, 1180, 1828, 1187, 6308, 1189, 5284, 1190, 3236, 1193, 4772,
    1194, 2724, 1196, 1700, 1201, 4516, 1202, 2468, 1204, 1444, 1219, 6244,
    1221, 5220, 1222, 3172, 1225, 4708, 1226, 2660, 1228, 1636, 1233, 4452,
    1234, 2404, 1236, 1380, 1249, 4324, 1250, 2276, 1287, 7188, 1291, 6676,
    1293, 5652, 1294, 3604, 1299, 6420, 1301, 5396, 1302, 3348, 1305, 4884,
    1306, 2836, 1308, 1812, 1315, 6292, 1317, 5268, 1318, 3220, 1321, 4756,
    1322, 2708, 1324, 1684, 1329, 4500, 1330, 2452, 1332, 1428, 1347, 6228,
    1349, 5204, 1350, 3156, 1353, 4692, 1354, 2644, 1356, 1620, 1361, 4436,
    1362, 2388, 1377, 4308, 1378, 2260, 1411, 6196, 1413, 5172, 1414, 3124,
    1417, 4660, 1418, 2612, 1420, 1588, 1425, 4404, 1426, 2356, 1441, 4276,
    1442, 2228, 1473, 4212, 1474, 2164, 1543, 7180, 1547, 6668, 1549, 5644,
    1550, 3596, 1555, 6412, 1557, 5388, 1558, 3340, 1561, 4876, 1562, 2828,
    1564, 1804, 1571, 6284, 1573, 5260, 1574, 3212, 1577, 4748, 1578, 2700,
    1580, 1676, 1585, 4492, 1586, 2444, 1603, 6220, 1605, 5196, 1606, 3148,
    1609, 4684, 1610, 2636, 1617, 4428, 1618, 2380, 1633, 4300, 1634, 2252,
    1667, 6188, 1669, 5164, 1670, 3116, 1673, 4652, 1674, 2604, 1681, 4396,
    1682, 2348, 1697, 4268, 1698, 2220, 1729, 4204, 1730, 2156, 1795, 6172,
    1797, 5148, 1798, 3100, 1801, 4636, 1802, 2588, 1809, 4380, 1810, 2332,
    1825, 4252, 1826, 2204, 1857, 4188, 1858, 2140, 1921, 4156, 1922, 2108,
    2063, 7682, 2071, 7426, 2075, 6914, 2077, 5890, 2078, 3842, 2087, 7298,
    2091, 6786, 2093, 5762, 2094, 3714, 2099, 6530, 2101, 5506, 2102, 3458,
    2105, 4994, 2106, 2946, 2119, 7234, 2123, 6722, 2125, 5698, 2126, 3650,
    2131, 6466, 2133, 5442, 2134, 3394, 2137, 4930, 2138, 2882, 2147, 6338,
    2149, 5314, 2150, 3266, 2153, 4802, 2154, 2754, 2161, 4546, 2162, 2498,
    2183, 7202, 2187, 6690, 2189, 5666, 2190, 3618, 2195, 6434, 2197, 5410,
    2198, 3362, 2201, 4898, 2202, 2850, 2211, 6306, 2213, 5282, 2214, 3234,
    2217, 4770, 2218, 2722, 2225, 4514, 2226, 2466, 2243, 6242, 2245, 5218,
    2246, 3170, 2249, 4706, 2250, 2658, 2257, 4450, 2258, 2402, 2273, 4322,
    2311, 7186, 2315, 6674, 2317, 5650, 2318, 3602, 2323, 6418, 2325, 5394,
    2326, 3346, 2329, 4882, 2330, 2834, 2339, 6290, 2341, 5266, 2342, 3218,
    2345, 4754, 2346, 2706, 2353, 4498, 2354, 2450, 2371, 6226, 2373, 5202,
    2374, 3154, 2377, 4690, 2378, 2642, 2385, 4434, 2401, 4306, 2435, 6194,
    2437, 5170, 2438, 3122, 2441, 4658, 2442, 2610, 2449, 4402, 2465, 4274,
    2497, 4210, 2567, 7178, 2571, 6666, 2573, 5642, 2574, 3594, 2579, 6410,
    2581, 5386, 2582, 3338, 2585, 4874, 2586, 2826, 2595, 6282, 2597, 5258,
    2598, 3210, 2601, 4746, 2602, 2698, 2609, 4490, 2627, 6218, 2629, 5194,
    2630, 3146, 2633, 4682, 2641, 4426, 2657, 4298, 2691, 6186, 2693, 5162,
    2694, 3114, 2697, 4650, 2705, 4394, 2721, 4266, 2753, 4202, 2819, 6170,
    2821, 5146, 2822, 3098, 2825, 4634, 2833, 4378, 2849, 4250, 2881, 4186,
    2945, 4154, 3079, 7174, 3083, 6662, 3085, 5638, 3086, 3590, 3091, 6406,
    3093, 5382, 3094, 3334, 3097, 4870, 3107, 6278, 3109, 5254, 3110, 3206,
    3113, 4742, 3121, 4486, 3139, 6214, 3141, 5190, 3145, 4678, 3153, 4422,
    3169, 4294, 3203, 6182, 3205, 5158, 3209, 4646, 3217, 4390, 3233, 4262,
    3265, 4198, 3331, 6166, 3333, 5142, 3337, 4630, 3345, 4374, 3361, 4246,
    3393, 4182, 3457, 4150, 3587, 6158, 3589, 5134, 3593, 4622, 3601, 4366,
    3617, 4238, 3649, 4174, 3713, 4142, 3841, 4126, 4111, 7681, 4119, 7425,
    4123, 6913, 4125, 5889, 4135, 7297, 4139, 6785, 4141, 5761, 4147, 6529,
    4149, 5505, 4153, 4993, 4167, 7233, 4171, 6721, 4173, 5697, 4179, 6465,
    4181, 5441, 4185, 4929, 4195, 6337, 4197, 5313, 4201, 4801, 4209, 4545,
    4231, 7201, 4235, 6689, 4237, 5665, 4243, 6433, 4245, 5409, 4249, 4897,
    4259, 6305, 4261, 5281, 4265, 4769, 4273, 4513, 4291, 6241, 4293, 5217,
    4297, 4705, 4305, 4449, 4359, 7185, 4363, 6673, 4365, 5649, 4371, 6417,
    4373, 5393, 4377, 4881, 4387, 6289, 4389, 5265, 4393, 4753, 4401, 4497,
    4419, 6225, 4421, 5201, 4425, 4689, 4483, 6193, 4485, 5169, 4489, 4657,
    4615, 7177, 4619, 6665, 4621, 5641, 4627, 6409, 4629, 5385, 4633, 4873,
    4643, 6281, 4645, 5257, 4649, 4745, 4675, 6217, 4677, 5193, 4739, 6185,
    4741, 5161, 4867, 6169, 4869, 5145, 5127, 7173, 5131, 6661, 5133, 5637,
    5139, 6405, 5141, 5381, 5155, 6277, 5157, 5253, 5187, 6213, 5251, 6181,
    5379, 6165, 5635, 6157, 6151, 7171, 6155, 6659, 6163, 6403, 6179, 6275,
    6211, 5189, 4681, 4433, 4321, 3142, 2634, 2386, 2274, 1612, 1364, 1252,
    856, 744, 496,
};

const uint16_t kImbTab213[78] = {
    3, 6144, 5, 5120, 6, 3072, 9, 4608, 10, 2560, 12, 1536,
    17, 4352, 18, 2304, 20, 1280, 24, 768, 33, 4224, 34, 2176,
    36, 1152, 40, 640, 48, 384, 65, 4160, 66, 2112, 68, 1088,
    72, 576, 80, 320, 96, 192, 129, 4128, 130, 2080, 132, 1056,
    136, 544, 144, 288, 257, 4112, 258, 2064, 260, 1040, 264, 528,
    513, 4104, 514, 2056, 516, 1032, 1025, 4100, 1026, 2052, 2049, 4098,
    4097, 2050, 1028, 520, 272, 160,
};

const uint8_t kImbBarmap[260] = {
    7, 2, 4, 3, 1, 10, 0, 0, 9, 12, 2, 8, 5, 5, 6, 11, 8, 9, 3, 1,
    0, 1, 5, 12, 2, 5, 1, 8, 4, 4, 9, 11, 6, 3, 8, 10, 3, 9, 7, 6,
    5, 11, 1, 4, 8, 5, 2, 12, 9, 10, 0, 2, 7, 1, 6, 7, 3, 6, 4, 9,
    0, 3, 8, 6, 6, 4, 2, 7, 1, 1, 9, 9, 7, 10, 5, 2, 4, 0, 3, 8,
    6, 2, 0, 4, 8, 11, 1, 0, 9, 8, 3, 12, 2, 6, 7, 7, 5, 1, 4, 10,
    1, 12, 6, 9, 7, 3, 8, 0, 5, 8, 9, 7, 4, 6, 2, 10, 3, 4, 0, 5,
    8, 4, 5, 7, 7, 11, 1, 9, 6, 0, 9, 6, 0, 6, 4, 8, 2, 1, 3, 2,
    5, 9, 8, 12, 4, 11, 6, 1, 9, 5, 7, 4, 3, 3, 1, 2, 0, 7, 2, 0,
    1, 3, 4, 1, 6, 10, 3, 5, 8, 7, 9, 4, 2, 11, 5, 6, 0, 8, 7, 12,
    4, 2, 8, 1, 5, 10, 3, 0, 9, 3, 0, 9, 6, 5, 2, 4, 7, 8, 1, 7,
    5, 0, 4, 5, 2, 3, 0, 10, 6, 12, 9, 2, 3, 11, 1, 6, 8, 8, 7, 9,
    5, 4, 0, 11, 1, 5, 2, 2, 9, 1, 4, 12, 8, 3, 6, 6, 7, 0, 3, 7,
    4, 7, 7, 5, 0, 12, 1, 11, 2, 9, 9, 0, 6, 8, 5, 3, 3, 10, 8, 2,
};

constexpr int kImbBars = 65;

struct Bar {
    int x0, x1;     // 가로 범위
    int top, bot;   // 세로 범위 (이미지 좌표, 아래로 증가)
};

/*
 * ROI에서 막대를 찾는다. 열 단위로 어두운 화소가 있는지 보고, 이어진 열을
 * 한 막대로 묶는다. 4-state는 막대 폭이 전부 같아서 폭으로는 정보가 없다 —
 * 필요한 것은 각 막대의 위/아래 끝이다.
 */
bool findBars(const GrayView& img, bool rotated, int minBars, std::vector<Bar>& out) {
    out.clear();
    int lo = 255, hi = 0;
    for (int y = 0; y < img.height; ++y) {
        const uint8_t* row = img.pixels + static_cast<size_t>(y) * img.stride;
        for (int x = 0; x < img.width; ++x) {
            lo = std::min(lo, static_cast<int>(row[x]));
            hi = std::max(hi, static_cast<int>(row[x]));
        }
    }
    if (hi - lo < 40) return false;
    const int thr = (lo + hi) / 2;

    /*
     * [90도로 세운 것도 읽는다] rotated면 x와 y의 역할을 바꾼다 — 막대가
     * 가로로 눕고 위아래로 쌓인 모양이다. 4-state는 정보가 높이에 있으므로
     * 축을 바꾸면 "높이"가 "가로 폭"이 된다.
     */
    const int nAlong = rotated ? img.height : img.width;   // 막대가 늘어선 방향
    const int nCross = rotated ? img.width : img.height;   // 막대의 길이 방향
    auto dark = [&](int a, int c) {
        const int x = rotated ? c : a, y = rotated ? a : c;
        return img.pixels[static_cast<size_t>(y) * img.stride + x] < thr;
    };

    std::vector<int> aTop(nAlong, -1), aBot(nAlong, -1);
    for (int a = 0; a < nAlong; ++a) {
        for (int c = 0; c < nCross; ++c) {
            if (!dark(a, c)) continue;
            if (aTop[a] < 0) aTop[a] = c;
            aBot[a] = c;
        }
    }

    int a = 0;
    while (a < nAlong) {
        if (aTop[a] < 0) { ++a; continue; }
        const int start = a;
        int t = aTop[a], b = aBot[a];
        while (a < nAlong && aTop[a] >= 0) {
            t = std::min(t, aTop[a]);
            b = std::max(b, aBot[a]);
            ++a;
        }
        out.push_back({start, a - 1, t, b});
        if (out.size() > 4096) return false;
    }
    return out.size() >= static_cast<size_t>(minBars);
}

/*
 * 막대들을 4-state로 분류한다.
 *
 * 전체 심볼의 위/아래 끝은 full 막대가 정한다. 그런데 **full 막대가 하나도
 * 없을 수는 없다** — 시작 패턴이 "31"이라 첫 막대가 full이다. 그래서
 * 관측된 최상단/최하단을 그대로 기준선으로 쓴다.
 *
 * 판정은 "위 끝에 닿는가 / 아래 끝에 닿는가"다. 경계는 전체 높이의 1/4로
 * 둔다 — tracker는 3/8~5/8 구간이라 여유가 크다.
 */
bool classify(const std::vector<Bar>& bars, size_t at, int count, std::string& out) {
    int top = 1 << 30, bot = -1;
    for (int i = 0; i < count; ++i) {
        top = std::min(top, bars[at + i].top);
        bot = std::max(bot, bars[at + i].bot);
    }
    const double h = bot - top;
    if (h < 4) return false;

    out.clear();
    for (int i = 0; i < count; ++i) {
        const bool hitTop = (bars[at + i].top - top) < h * 0.25;
        const bool hitBot = (bot - bars[at + i].bot) < h * 0.25;
        // 막대 높이도 검증한다 — tracker는 전체의 1/4, 나머지는 5/8 또는 1
        const double frac = (bars[at + i].bot - bars[at + i].top) / h;
        if (hitTop && hitBot) {
            if (frac < 0.8) return false;
            out.push_back('3');
        } else if (hitTop) {
            if (frac < 0.4 || frac > 0.85) return false;
            out.push_back('2');
        } else if (hitBot) {
            if (frac < 0.4 || frac > 0.85) return false;
            out.push_back('1');
        } else {
            if (frac > 0.55) return false;
            out.push_back('0');
        }
    }
    return true;
}

int jpSymbol(const std::string& three) {
    for (int i = 0; i < 19; ++i)
        if (three == kJpEnc[i]) return i;
    return -1;
}

// 막대 간격이 고르게 이어지는지 본다. 우편 바코드는 등간격이다.
bool evenlySpaced(const std::vector<Bar>& bars, size_t at, int count) {
    if (count < 3) return false;
    std::vector<int> gaps;
    for (int i = 0; i + 1 < count; ++i) gaps.push_back(bars[at + i + 1].x0 - bars[at + i].x0);
    std::sort(gaps.begin(), gaps.end());
    const int med = gaps[gaps.size() / 2];
    if (med < 2) return false;
    for (int g : gaps)
        if (std::abs(g - med) > std::max(2, med / 3)) return false;
    return true;
}

bool decodeJapanPattern(const std::string& pat, std::string& out);

/*
 * [평면 대칭 넷을 다 본다]
 * 심볼이 180도 돌면 막대 순서가 뒤집히고 ascender/descender도 서로 바뀐다.
 * 90도로 세운 것을 축을 바꿔 읽을 때는 어느 쪽으로 돌았느냐에 따라
 * **순서는 맞는데 극성만 뒤집히는** 경우가 생긴다 — 실측으로 90도에서
 * 시작/정지("31"..."13")는 맞는데 심볼이 하나도 표에 없었다.
 * 그래서 항등 / 극성만 / 순서만 / 둘 다, 넷을 다 시도한다.
 * 잘못된 변환은 시작·정지 패턴과 검사 심볼(mod 19)에서 걸린다.
 */
std::string swapAD(const std::string& p) {
    std::string r = p;
    for (char& c : r) { if (c == '1') c = '2'; else if (c == '2') c = '1'; }
    return r;
}
std::string reversed(const std::string& p) { return std::string(p.rbegin(), p.rend()); }

bool decodeJapanPost(const std::vector<Bar>& bars, size_t at, std::string& out) {
    if (at + kJpBars > bars.size()) return false;
    if (!evenlySpaced(bars, at, kJpBars)) return false;

    std::string raw;
    if (!classify(bars, at, kJpBars, raw)) return false;
    if (decodeJapanPattern(raw, out)) return true;
    if (decodeJapanPattern(swapAD(raw), out)) return true;
    if (decodeJapanPattern(reversed(raw), out)) return true;
    return decodeJapanPattern(reversed(swapAD(raw)), out);
}

bool decodeJapanPattern(const std::string& pat, std::string& out) {
    if (pat.compare(0, 2, kJpStart) != 0) return false;
    if (pat.compare(kJpBars - 2, 2, kJpStop) != 0) return false;

    std::vector<int> sym;
    for (int i = 0; i < kJpSymbols; ++i) {
        const int v = jpSymbol(pat.substr(2 + i * 3, 3));
        if (v < 0) return false;
        sym.push_back(v);
    }

    // 검사 심볼: (데이터 합 + 검사) mod 19 == 0
    int sum = 0;
    for (int i = 0; i < kJpSymbols - 1; ++i) sum += sym[i];
    if ((sum + sym[kJpSymbols - 1]) % 19 != 0) return false;

    std::string text;
    for (int i = 0; i < kJpSymbols - 1; ++i) {
        const int v = sym[i];
        if (v <= 9) { text.push_back(static_cast<char>('0' + v)); continue; }
        if (v == 10) { text.push_back('-'); continue; }
        if (v == 14) break;                       // 채움 — 여기서 데이터 끝
        if (v >= 11 && v <= 13) {                 // 제어 + 숫자 = 영문자
            if (i + 1 >= kJpSymbols - 1) return false;
            const int d = sym[++i];
            if (d > 9) return false;
            const char base = (v == 11) ? 'A' : (v == 12) ? 'K' : 'U';
            const char ch = static_cast<char>(base + d);
            if (ch > 'Z') return false;
            text.push_back(ch);
            continue;
        }
        return false;                             // 15~18은 예약
    }
    if (text.empty()) return false;
    out = text;
    return true;
}


/* ---------------------------------------------------------------------------
 * USPS Intelligent Mail (IMB)
 *
 * 같은 4-state지만 일본우편과 구조가 완전히 다르다. 시작/정지 패턴이 없고,
 * 65개 막대의 어센더/디센더 비트가 **13비트 문자 10개에 흩뿌려져** 있다
 * (barmap). 문자는 1이 5개(tab513) 또는 2개(tab213)인 값이고, CRC-11(FCS)의
 * 각 비트가 해당 문자를 8191과 XOR해서 표시한다.
 *
 * 그래서 디코딩이 이렇게 된다:
 *   막대 -> 문자 10개 -> (1의 개수로 원본/반전 판정) 코드워드 + FCS 비트
 *   -> 혼합 진법(1365^9 x 636) 역산 -> 102비트 값 -> CRC 검증 -> 자릿수 복원
 *
 * **1의 개수가 곧 판정이다**: 5=tab513 원본, 8=tab513 반전(FCS 비트 1),
 * 2=tab213 원본, 11=tab213 반전. 다른 값이면 그 문자는 손상이다.
 *
 * 시작/정지가 없어 방향을 못 정하므로 평면 대칭 넷을 다 시도한다 —
 * 틀린 방향은 **CRC-11이 거른다**(1/2048 확률로만 통과).
 * ------------------------------------------------------------------------- */

struct ImbRev {
    std::array<std::pair<uint16_t, uint16_t>, 1287> t513;
    std::array<std::pair<uint16_t, uint16_t>, 78> t213;
    ImbRev() {
        for (int i = 0; i < 1287; ++i) t513[i] = {kImbTab513[i], static_cast<uint16_t>(i)};
        for (int i = 0; i < 78; ++i) t213[i] = {kImbTab213[i], static_cast<uint16_t>(i)};
        auto by = [](const std::pair<uint16_t, uint16_t>& a, const std::pair<uint16_t, uint16_t>& b) {
            return a.first < b.first;
        };
        std::sort(t513.begin(), t513.end(), by);
        std::sort(t213.begin(), t213.end(), by);
    }
    template <size_t N>
    static int find(const std::array<std::pair<uint16_t, uint16_t>, N>& t, uint16_t k) {
        auto it = std::lower_bound(t.begin(), t.end(), k,
                                   [](const std::pair<uint16_t, uint16_t>& a, uint16_t v) { return a.first < v; });
        return (it != t.end() && it->first == k) ? it->second : -1;
    }
};
const ImbRev& imbRev() { static const ImbRev r; return r; }

// 13바이트(상위부터)에 대한 CRC-11. 첫 바이트는 6비트만 쓴다(102비트 값).
int imbCrc11(unsigned __int128 v) {
    uint8_t b[13];
    for (int i = 0; i < 13; ++i) b[i] = static_cast<uint8_t>((v >> (8 * (12 - i))) & 0xFF);
    int fcs = 2047;
    int dat = b[0] << 5;
    for (int k = 0; k < 6; ++k) {
        fcs = ((fcs ^ dat) & 1024) ? (((fcs << 1) ^ 3893) & 2047) : ((fcs << 1) & 2047);
        dat = (dat << 1) & 0xFFFF;
    }
    for (int i = 1; i < 13; ++i) {
        dat = b[i] << 3;
        for (int k = 0; k < 8; ++k) {
            fcs = ((fcs ^ dat) & 1024) ? (((fcs << 1) ^ 3893) & 2047) : ((fcs << 1) & 2047);
            dat = (dat << 1) & 0xFFFF;
        }
    }
    return fcs;
}

bool decodeImbPattern(const std::string& pat, std::string& out) {
    if (static_cast<int>(pat.size()) != kImbBars) return false;

    uint16_t chars[10] = {0};
    for (int i = 0; i < kImbBars; ++i) {
        const char s = pat[i];
        const bool desc = (s == '1' || s == '3');
        const bool asc = (s == '2' || s == '3');
        if (desc) chars[kImbBarmap[i * 4]] |= static_cast<uint16_t>(1u << kImbBarmap[i * 4 + 1]);
        if (asc) chars[kImbBarmap[i * 4 + 2]] |= static_cast<uint16_t>(1u << kImbBarmap[i * 4 + 3]);
    }

    int cw[10];
    int fcs = 0;
    for (int i = 0; i < 10; ++i) {
        const uint16_t c = chars[i];
        const int pc = __builtin_popcount(c);
        int v;
        if (pc == 5 && (v = ImbRev::find(imbRev().t513, c)) >= 0) cw[i] = v;
        else if (pc == 2 && (v = ImbRev::find(imbRev().t213, c)) >= 0) cw[i] = v + 1287;
        else if (pc == 8 && (v = ImbRev::find(imbRev().t513, static_cast<uint16_t>(c ^ 8191))) >= 0) {
            cw[i] = v; fcs |= 1 << i;
        } else if (pc == 11 && (v = ImbRev::find(imbRev().t213, static_cast<uint16_t>(c ^ 8191))) >= 0) {
            cw[i] = v + 1287; fcs |= 1 << i;
        } else return false;
    }

    if (cw[0] >= 659) { cw[0] -= 659; fcs |= 1024; }
    if (cw[9] % 2) return false;
    cw[9] /= 2;
    if (cw[9] >= 636) return false;
    for (int i = 0; i < 9; ++i) if (cw[i] >= 1365) return false;

    unsigned __int128 binval = 0;
    for (int i = 0; i < 9; ++i) binval = binval * 1365 + static_cast<unsigned>(cw[i]);
    binval = binval * 636 + static_cast<unsigned>(cw[9]);

    if (imbCrc11(binval) != fcs) return false;

    unsigned __int128 p18 = 1;
    for (int i = 0; i < 18; ++i) p18 *= 10;
    const unsigned long long track = static_cast<unsigned long long>(binval % p18);
    unsigned __int128 v = binval / p18;
    const int d1 = static_cast<int>(v % 5); v /= 5;
    const int d0 = static_cast<int>(v % 10); v /= 10;

    char buf[64];
    std::string routing;
    if (v == 0) {
        // 라우팅 없음
    } else if (v <= 100000ULL) {
        std::snprintf(buf, sizeof(buf), "%05llu", static_cast<unsigned long long>(v - 1));
        routing = buf;
    } else if (v <= 1000100000ULL) {
        std::snprintf(buf, sizeof(buf), "%09llu", static_cast<unsigned long long>(v - 100001ULL));
        routing = buf;
    } else if (v <= 1000100000ULL + 100000000000ULL) {
        std::snprintf(buf, sizeof(buf), "%011llu", static_cast<unsigned long long>(v - 1000100001ULL));
        routing = buf;
    } else {
        return false;
    }
    std::snprintf(buf, sizeof(buf), "%d%d%018llu", d0, d1, track);
    out = std::string(buf) + routing;
    return true;
}

bool decodeImb(const std::vector<Bar>& bars, size_t at, std::string& out) {
    if (at + kImbBars > bars.size()) return false;
    if (!evenlySpaced(bars, at, kImbBars)) return false;
    std::string raw;
    if (!classify(bars, at, kImbBars, raw)) return false;
    if (decodeImbPattern(raw, out)) return true;
    if (decodeImbPattern(swapAD(raw), out)) return true;
    if (decodeImbPattern(reversed(raw), out)) return true;
    return decodeImbPattern(reversed(swapAD(raw)), out);
}

} // namespace

std::vector<DecodedSymbol> PostalDecoder::decode(const GrayView& image) {
    std::vector<DecodedSymbol> results;
    if (!opt_.japanPost && !opt_.imb) return results;
    // 켜진 것 중 더 적은 막대 수를 기준으로 찾는다 — 67(일본우편)로 재면
    // 65막대인 IMB가 통째로 걸러진다(실측으로 IMB가 0/20이었다).
    const int minBars = std::min(opt_.japanPost ? kJpBars : 1 << 20,
                                 opt_.imb ? kImbBars : 1 << 20);
    if (image.empty() || std::max(image.width, image.height) < minBars * 2) return results;

    for (int axis = 0; axis < 2; ++axis) {
        std::vector<Bar> bars;
        if (!findBars(image, axis == 1, minBars, bars)) continue;
        // IMB(65막대)를 먼저 본다 — CRC-11이 강해서 오탐이 사실상 없다.
        if (opt_.imb) {
            for (size_t i = 0; i + kImbBars <= bars.size(); ++i) {
                std::string text;
                if (!decodeImb(bars, i, text)) continue;
                DecodedSymbol s;
                s.symbology = Symbology::POSTAL_IMB;
                s.text = text;
                s.rawBytes.assign(text.begin(), text.end());
                int top = 1 << 30, bot = -1;
                for (int k = 0; k < kImbBars; ++k) {
                    top = std::min(top, bars[i + k].top);
                    bot = std::max(bot, bars[i + k].bot);
                }
                const int a0 = bars[i].x0, a1 = bars[i + kImbBars - 1].x1;
                const int x0 = axis ? top : a0, x1 = axis ? bot : a1;
                const int y0 = axis ? a0 : top, y1 = axis ? a1 : bot;
                s.position = {{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}};
                results.push_back(std::move(s));
                i += kImbBars - 1;
            }
        }
        if (!opt_.japanPost) { if (!results.empty()) break; else continue; }
        for (size_t i = 0; i + kJpBars <= bars.size(); ++i) {
            std::string text;
            if (!decodeJapanPost(bars, i, text)) continue;
            DecodedSymbol s;
            s.symbology = Symbology::POSTAL_JAPAN;
            s.text = text;
            s.rawBytes.assign(text.begin(), text.end());
            int top = 1 << 30, bot = -1;
            for (int k = 0; k < kJpBars; ++k) {
                top = std::min(top, bars[i + k].top);
                bot = std::max(bot, bars[i + k].bot);
            }
            const int a0 = bars[i].x0, a1 = bars[i + kJpBars - 1].x1;
            const int x0 = axis ? top : a0, x1 = axis ? bot : a1;
            const int y0 = axis ? a0 : top, y1 = axis ? a1 : bot;
            s.position = {{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}};
            results.push_back(std::move(s));
            i += kJpBars - 1;
        }
        if (!results.empty()) break;
    }
    return results;
}

} // namespace vscan
