#include "vscan_internal/deskew1d.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace vscan {
namespace {

// 임의 배율 박스 다운샘플 (이 파일 전용 — preprocess.cpp의 downsampleBox는
// 2/3배율만 지원해서, 여기서는 4배율을 별도로 쓴다).
void downsampleN(const GrayView& src, int factor, std::vector<uint8_t>& dst, int& dw, int& dh) {
    dw = src.width / factor;
    dh = src.height / factor;
    dst.assign(static_cast<size_t>(dw) * dh, 0);
    const int stride = src.stride > 0 ? src.stride : src.width;
    for (int y = 0; y < dh; ++y) {
        uint8_t* __restrict out = dst.data() + static_cast<size_t>(y) * dw;
        for (int x = 0; x < dw; ++x) {
            int sum = 0;
            for (int j = 0; j < factor; ++j) {
                const uint8_t* __restrict row = src.pixels + static_cast<size_t>(y * factor + j) * stride + x * factor;
                for (int i = 0; i < factor; ++i) sum += row[i];
            }
            out[x] = static_cast<uint8_t>(sum / (factor * factor));
        }
    }
}

// Sobel 그래디언트 (3x3). 경계 1px는 0으로 둔다 — 타일 판정에 영향 없음.
void sobel(const std::vector<uint8_t>& img, int w, int h, std::vector<float>& gx, std::vector<float>& gy) {
    gx.assign(static_cast<size_t>(w) * h, 0.0f);
    gy.assign(static_cast<size_t>(w) * h, 0.0f);
    for (int y = 1; y < h - 1; ++y) {
        const uint8_t* __restrict r0 = img.data() + static_cast<size_t>(y - 1) * w;
        const uint8_t* __restrict r1 = img.data() + static_cast<size_t>(y) * w;
        const uint8_t* __restrict r2 = img.data() + static_cast<size_t>(y + 1) * w;
        float* __restrict gxRow = gx.data() + static_cast<size_t>(y) * w;
        float* __restrict gyRow = gy.data() + static_cast<size_t>(y) * w;
        for (int x = 1; x < w - 1; ++x) {
            float sx = (r0[x + 1] + 2 * r1[x + 1] + r2[x + 1]) - (r0[x - 1] + 2 * r1[x - 1] + r2[x - 1]);
            float sy = (r2[x - 1] + 2 * r2[x] + r2[x + 1]) - (r0[x - 1] + 2 * r0[x] + r0[x + 1]);
            gxRow[x] = sx;
            gyRow[x] = sy;
        }
    }
}

struct TileStat {
    float coherence = 0.0f;
    float energy = 0.0f;
    bool candidate = false;
};

} // namespace

bool findDeskewCandidate(const GrayView& image, DeskewCandidate& out, int tileSize, int downscale,
                          float minCoherence) {
    if (image.empty() || image.width < downscale * tileSize * 2 || image.height < downscale * tileSize * 2)
        return false;

    std::vector<uint8_t> small;
    int dw, dh;
    downsampleN(image, downscale, small, dw, dh);

    std::vector<float> gx, gy;
    sobel(small, dw, dh, gx, gy);

    const int tilesX = dw / tileSize;
    const int tilesY = dh / tileSize;
    if (tilesX < 2 || tilesY < 2) return false;

    // [타일별 블록 리덕션] 8x8 이진화 블록과 같은 패턴 — 원래는 여기서
    // 그래디언트 크기(sqrt(vx^2+vy^2))를 매 픽셀 누적해 "에너지"로
    // 썼는데, sqrt가 리덕션 루프 안에 섞이면 벡터화가 막힌다(A53 빌드
    // -fopt-info-vec-missed로 확인: "complicated access pattern").
    // 구조텐서 이론상 jxx+jyy가 이미 그래디언트 크기의 "제곱합"이므로,
    // 굳이 sqrt 안 걸린 별도 누적 없이 이걸 그대로 에너지 척도로
    // 재사용한다 — 크기 그 자체가 아니라 "그래디언트가 강한 정도"의
    // 상대적 랭킹만 필요하므로 제곱합으로도 같은 판정을 할 수 있다.
    // 결과: sqrt 완전 제거, 순수 곱셈-누적만 남아 자동 벡터화된다.
    // [[vscan-lite-deskew-neon]]
    std::vector<TileStat> tiles(static_cast<size_t>(tilesX) * tilesY);
    std::vector<float> energies;
    energies.reserve(tiles.size());
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            float jxx = 0, jyy = 0, jxy = 0;
            const int y0 = ty * tileSize, x0 = tx * tileSize;
            for (int y = 0; y < tileSize; ++y) {
                const float* __restrict gxRow = gx.data() + static_cast<size_t>(y0 + y) * dw + x0;
                const float* __restrict gyRow = gy.data() + static_cast<size_t>(y0 + y) * dw + x0;
                for (int x = 0; x < tileSize; ++x) {
                    float vx = gxRow[x], vy = gyRow[x];
                    jxx += vx * vx;
                    jyy += vy * vy;
                    jxy += vx * vy;
                }
            }
            float coherence = std::sqrt((jxx - jyy) * (jxx - jyy) + 4.0f * jxy * jxy) / (jxx + jyy + 1e-6f);
            float energy = (jxx + jyy) / (tileSize * tileSize);
            size_t idx = static_cast<size_t>(ty) * tilesX + tx;
            tiles[idx].coherence = coherence;
            tiles[idx].energy = energy;
            energies.push_back(energy);
        }
    }

    // 에너지 상위 10%만 후보 대상 (절대 임계값 대신 프레임마다 적응).
    std::vector<float> sorted = energies;
    size_t p90 = static_cast<size_t>(sorted.size() * 0.9);
    std::nth_element(sorted.begin(), sorted.begin() + p90, sorted.end());
    float energyThresh = sorted[p90];

    for (auto& t : tiles) t.candidate = (t.coherence > minCoherence) && (t.energy > energyThresh);

    // [연결된 후보 타일 중 최대 덩어리 찾기] 타일 그리드가 작아서
    // (보통 수백 개) 굳이 벡터화할 필요 없이 단순 BFS로 충분히 빠르다.
    std::vector<int> visited(tiles.size(), 0);
    int bestArea = 0;
    int bestX0 = 0, bestY0 = 0, bestX1 = 0, bestY1 = 0;
    std::vector<int> stack;
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            size_t start = static_cast<size_t>(ty) * tilesX + tx;
            if (!tiles[start].candidate || visited[start]) continue;
            stack.clear();
            stack.push_back(static_cast<int>(start));
            visited[start] = 1;
            int area = 0, minX = tx, maxX = tx, minY = ty, maxY = ty;
            while (!stack.empty()) {
                int cur = stack.back();
                stack.pop_back();
                int cx = cur % tilesX, cy = cur / tilesX;
                ++area;
                minX = std::min(minX, cx); maxX = std::max(maxX, cx);
                minY = std::min(minY, cy); maxY = std::max(maxY, cy);
                static const int dxs[4] = {1, -1, 0, 0};
                static const int dys[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; ++k) {
                    int nx = cx + dxs[k], ny = cy + dys[k];
                    if (nx < 0 || nx >= tilesX || ny < 0 || ny >= tilesY) continue;
                    size_t nidx = static_cast<size_t>(ny) * tilesX + nx;
                    if (tiles[nidx].candidate && !visited[nidx]) {
                        visited[nidx] = 1;
                        stack.push_back(static_cast<int>(nidx));
                    }
                }
            }
            if (area > bestArea) {
                bestArea = area;
                bestX0 = minX; bestY0 = minY; bestX1 = maxX + 1; bestY1 = maxY + 1;
            }
        }
    }
    if (bestArea == 0) return false;

    // 타일 그리드 -> 다운샘플 픽셀 -> 원본 픽셀 좌표로 환산.
    int scale = tileSize * downscale;
    out.bbox.x0 = std::max(0, bestX0 * scale);
    out.bbox.y0 = std::max(0, bestY0 * scale);
    out.bbox.x1 = std::min(image.width, bestX1 * scale);
    out.bbox.y1 = std::min(image.height, bestY1 * scale);
    out.coherence = minCoherence; // 대표값(단일 스칼라 요구되는 인터페이스라 최소치만 기록)
    return true;
}

} // namespace vscan
