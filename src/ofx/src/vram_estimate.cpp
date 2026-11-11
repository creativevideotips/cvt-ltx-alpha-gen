#include "vram_estimate.h"

#include <algorithm>

namespace ltxag {

namespace {
constexpr double kBaseFp8 = 19.5;
constexpr double kBf16Extra = 17.3;
constexpr double kPerMpxFrame = 0.0718;
constexpr double kPerMpxAboveHD = 0.70;
constexpr double kHDMpx = 1920.0 * 1088.0 / 1e6;
constexpr double kMargin = 1.0;
constexpr double kHeldFp8 = 21.7;   // reserved with the transformer kept (A/B run, HD, frames streamed)
constexpr double kHeldBf16 = 36.8;  // same, bf16 (Flame test 2)
constexpr double kEncodeBase = 1.56;
constexpr double kEncodePerMpxFrame = 0.1088;
constexpr double kContext = 0.7;  // CUDA context, outside torch's numbers
constexpr double kMpsBase = 44.5;  // M1 Ultra fit, see the header
constexpr double kMpsPerMpxFrame = 0.231;
// Apple Silicon at 16 bits per weight: M5 and newer in bf16, an M1-class Mac on the fp16 copy
// (CVT port, cvt1.28), see the header.
constexpr double kMps16Floor = 40.8;
constexpr double kMps16Base = 37.0;
constexpr double kMps16PerMpxFrame = 0.10;
constexpr double kStreamBase = 6.0;  // Windows port: weights streamed to the GPU, see the header
}  // namespace

VramNeed estimateVramNeed(bool fp8, int width, int height, int frames, bool residentTransformer, int mps,
                          bool streamed) {
  VramNeed n{};
  n.modelW = (std::max(width, 1) + 31) / 32 * 32;
  n.modelH = (std::max(height, 1) + 31) / 32 * 32;
  const int f = std::min(std::max(frames, 1), 145);
  n.modelFrames = f + (8 - (f - 1) % 8) % 8;
  const double mpx = double(n.modelW) * double(n.modelH) / 1e6;
  n.overMpsLimit = mps != 0 && mpx * n.modelFrames > kMpsMaxMpxFrames;
  if (mps >= 2) {  // cvt1.28: one fit for the M5 (bf16) and an M1 on the fp16 copy
    n.gb = std::max(kMps16Floor, kMps16Base + kMps16PerMpxFrame * mpx * n.modelFrames) + kMargin;
    return n;
  }
  if (mps) {
    n.gb = kMpsBase + kMpsPerMpxFrame * mpx * n.modelFrames + kMargin;
    return n;
  }
  if (streamed) {  // Windows port
    n.gb = std::max(kStreamBase + kPerMpxFrame * mpx * n.modelFrames,
                    kEncodeBase + kContext + kEncodePerMpxFrame * mpx * n.modelFrames) +
           kPerMpxAboveHD * std::max(0.0, mpx - kHDMpx) + kMargin;
    return n;
  }
  n.gb = kBaseFp8 + (fp8 ? 0.0 : kBf16Extra) + kPerMpxFrame * mpx * n.modelFrames +
         kPerMpxAboveHD * std::max(0.0, mpx - kHDMpx) + kMargin;
  if (residentTransformer) {
    const double withHeld = (fp8 ? kHeldFp8 : kHeldBf16) + kEncodeBase + kEncodePerMpxFrame * mpx * n.modelFrames +
                            kContext + kMargin;
    n.gb = std::max(n.gb, withHeld);
  }
  return n;
}

}  // namespace ltxag
