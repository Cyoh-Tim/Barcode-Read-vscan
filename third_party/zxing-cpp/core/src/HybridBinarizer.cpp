/*
* Copyright 2016 Nu-book Inc.
* Copyright 2016 ZXing authors
*/
// SPDX-License-Identifier: Apache-2.0

#include "HybridBinarizer.h"

#include "BitMatrix.h"
#include "Matrix.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace ZXing {

// This class uses 5x5 blocks to compute local luminance, where each block is 8x8 pixels.
// So this is the smallest dimension in each axis we can accept.
static constexpr int BLOCK_SIZE = 8;
static constexpr int MINIMUM_DIMENSION = BLOCK_SIZE * 5;
static constexpr int MIN_DYNAMIC_RANGE = 24;

HybridBinarizer::HybridBinarizer(const ImageView& iv) : GlobalHistogramBinarizer(iv) {}

HybridBinarizer::~HybridBinarizer() = default;

bool HybridBinarizer::getPatternRow(int row, int rotation, PatternRow& res) const
{
#if 1
	// This is the original "hybrid" behavior: use GlobalHistogram for the 1D case
	return GlobalHistogramBinarizer::getPatternRow(row, rotation, res);
#else
	// This is an alternative that can be faster in general and perform better in unevenly lit sitations like
	// https://github.com/zxing-cpp/zxing-cpp/blob/master/test/samples/ean13-2/21.png. That said, it fairs
	// worse in borderline low resolution situations. With the current black box sample set we'd loose 94
	// test cases while gaining 53 others.
	auto bits = getBitMatrix();
	if (bits)
		GetPatternRow(*bits, row, res, rotation % 180 != 0);
	return bits != nullptr;
#endif
}

/**
* Calculates a single black point for each block of pixels and saves it away.
* See the following thread for a discussion of this algorithm:
*  http://groups.google.com/group/zxing/browse_thread/thread/d06efa2c35a7ddc0
*/
static Matrix<int> CalculateBlackPoints(const uint8_t* __restrict luminances, int subWidth, int subHeight, int width, int height,
										int rowStride)
{
	Matrix<int>	blackPoints(subWidth, subHeight);

	for (int y = 0; y < subHeight; y++) {
		int yoffset = std::min(y * BLOCK_SIZE, height - BLOCK_SIZE);
		for (int x = 0; x < subWidth; x++) {
			int xoffset = std::min(x * BLOCK_SIZE, width - BLOCK_SIZE);
			// [vscan-lite patch v2] v1(완전 무분기)은 이진화만 순수 격리 실측
			// 시 깨끗한 이미지 -22~26%인데 고노이즈 이미지는 오히려 +27~50%
			// 느려졌다 — 원본의 조기종료가 "국소 분산이 크면(=노이즈) 나머지
			// 행을 합계만 계산"하는 실질적 스킵이었는데, v1이 이걸 완전히
			// 없애서 노이즈 이미지에서 매번 min/max 갱신 비용을 다 치렀기
			// 때문. 다양한 조명/노이즈 환경이 대상이라 이 역전이 걸린다.
			//
			// v2: 1행(8px)만 벡터화 친화적으로 먼저 보고 조기 판단한다.
			//   - 1행의 범위가 이미 임계값을 넘으면(노이즈처럼 국소 분산이
			//     큰 경우) -> 전체 블록도 반드시 그 이상이다(범위는 스캔할
			//     수록 넓어지기만 하므로 이 판단은 항상 안전하다). 이 분기
			//     에서는 min/max가 다운스트림에서 전혀 안 쓰이므로(아래
			//     "저분산" 분기 안에서만 min 사용) 나머지 7행은 합계만
			//     계산하면 된다 — 원본의 조기종료와 동일한 효과.
			//   - 1행이 임계값 안이면(일반적인 저분산~보통 이미지) -> 8행
			//     전체를 sum/min/max 계산한다(v1과 동일, 벡터화 잘 됨).
			// 블록당 분기 1회뿐이라(픽셀당 분기였던 원본과 다름) 벡터화가
			// 여전히 걸리면서, 노이즈 이미지의 조기종료 이득도 되살아난다.
			// [[vscan-lite-binarizer-neon-v2]]
			int sum = 0, mn, mx;
			{
				const uint8_t* __restrict row0 = luminances + yoffset * rowStride + xoffset;
				int r0min = 255, r0max = 0;
				for (int xx = 0; xx < BLOCK_SIZE; xx++) {
					int pixel = row0[xx];
					sum += pixel;
					r0min = std::min(r0min, pixel);
					r0max = std::max(r0max, pixel);
				}
				if (r0max - r0min > MIN_DYNAMIC_RANGE) {
					// 고분산 확정 - min/max는 이후 안 쓰이므로 판정만
					// 통과시키는 안전한 값으로 채우고 합계만 마저 더한다.
					for (int yy = 1; yy < BLOCK_SIZE; yy++) {
						const uint8_t* __restrict rowPx = luminances + (yoffset + yy) * rowStride + xoffset;
						for (int xx = 0; xx < BLOCK_SIZE; xx++) sum += rowPx[xx];
					}
					mn = 0;
					mx = MIN_DYNAMIC_RANGE + 1;
				} else {
					mn = r0min;
					mx = r0max;
					for (int yy = 1; yy < BLOCK_SIZE; yy++) {
						const uint8_t* __restrict rowPx = luminances + (yoffset + yy) * rowStride + xoffset;
						for (int xx = 0; xx < BLOCK_SIZE; xx++) {
							int pixel = rowPx[xx];
							sum += pixel;
							mn = std::min(mn, pixel);
							mx = std::max(mx, pixel);
						}
					}
				}
			}
			int min = mn, max = mx;

			// The default estimate is the average of the values in the block.
			int average = sum / (BLOCK_SIZE * BLOCK_SIZE);
			if (max - min <= MIN_DYNAMIC_RANGE) {
				// If variation within the block is low, assume this is a block with only light or only
				// dark pixels. In that case we do not want to use the average, as it would divide this
				// low contrast area into black and white pixels, essentially creating data out of noise.
				//
				// The default assumption is that the block is light/background. Since no estimate for
				// the level of dark pixels exists locally, use half the min for the block.
				average = min / 2;

				if (y > 0 && x > 0) {
					// Correct the "white background" assumption for blocks that have neighbors by comparing
					// the pixels in this block to the previously calculated black points. This is based on
					// the fact that dark barcode symbology is always surrounded by some amount of light
					// background for which reasonable black point estimates were made. The bp estimated at
					// the boundaries is used for the interior.

					// The (min < bp) is arbitrary but works better than other heuristics that were tried.
					int averageNeighborBlackPoint =
						(blackPoints(x, y - 1) + (2 * blackPoints(x - 1, y)) + blackPoints(x - 1, y - 1)) / 4;
					if (min < averageNeighborBlackPoint) {
						average = averageNeighborBlackPoint;
					}
				}
			}
			blackPoints(x, y) = average;
		}
	}
	return blackPoints;
}


/**
* Applies a single threshold to a block of pixels.
*/
static void ThresholdBlock(const uint8_t* __restrict luminances, int xoffset, int yoffset, int threshold, int rowStride,
						   BitMatrix& matrix)
{
	// [vscan-lite patch] 원본은 dst 포인터 형태 때문에 data ref 분석이
	// 실패해 벡터화가 막혔다. __restrict 인덱스 루프로 바꾸면 8바이트
	// 비교+마스크가 한 번에 벡터화된다(cmhs/and). 산출 값은 동일.
	for (int y = yoffset; y < yoffset + BLOCK_SIZE; ++y) {
		const uint8_t* __restrict src = luminances + y * rowStride + xoffset;
		auto* __restrict dst = matrix.row(y).begin() + xoffset;
		for (int i = 0; i < BLOCK_SIZE; ++i)
			dst[i] = (src[i] <= threshold) * BitMatrix::SET_V;
	}
}

/**
* For each block in the image, calculate the average black point using a 5x5 grid
* of the blocks around it. Also handles the corner cases (fractional blocks are computed based
* on the last pixels in the row/column which are also used in the previous block).
*/
static std::shared_ptr<BitMatrix> CalculateMatrix(const uint8_t* __restrict luminances, int subWidth, int subHeight, int width,
												  int height, int rowStride, const Matrix<int>& blackPoints)
{
	auto matrix = std::make_shared<BitMatrix>(width, height);

	for (int y = 0; y < subHeight; y++) {
		int yoffset = std::min(y * BLOCK_SIZE, height - BLOCK_SIZE);
		for (int x = 0; x < subWidth; x++) {
			int xoffset = std::min(x * BLOCK_SIZE, width - BLOCK_SIZE);
			int left = std::clamp(x, 2, subWidth - 3);
			int top = std::clamp(y, 2, subHeight - 3);
			int sum = 0;
			for (int dy = -2; dy <= 2; ++dy) {
				for (int dx = -2; dx <= 2; ++dx) {
					sum += blackPoints(left + dx, top + dy);
				}
			}
			int average = sum / 25;
			ThresholdBlock(luminances, xoffset, yoffset, average, rowStride, *matrix);
		}
	}

	return matrix;
}

std::shared_ptr<const BitMatrix> HybridBinarizer::getBlackMatrix() const
{
	if (width() >= MINIMUM_DIMENSION && height() >= MINIMUM_DIMENSION) {
		const uint8_t* luminances = _buffer.data(0, 0);
		int subWidth = (width() + BLOCK_SIZE - 1) / BLOCK_SIZE; // ceil(width/BS)
		int subHeight = (height() + BLOCK_SIZE - 1) / BLOCK_SIZE; // ceil(height/BS)
		auto blackPoints =
			CalculateBlackPoints(luminances, subWidth, subHeight, width(), height(), _buffer.rowStride());

		return CalculateMatrix(luminances, subWidth, subHeight, width(), height(), _buffer.rowStride(), blackPoints);
	} else {
		// If the image is too small, fall back to the global histogram approach.
		return GlobalHistogramBinarizer::getBlackMatrix();
	}
}

} // ZXing
