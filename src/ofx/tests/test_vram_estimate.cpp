// The estimate must cover every measured peak (never under-estimate) without being wildly high.
#include <cmath>
#include <cstdio>

#include "vram_estimate.h"

using namespace ltxag;

static int failures = 0;

static void covers(const char* what, bool fp8, int w, int h, int f, bool resident, double measured,
                   double maxOver, int mps = 0) {
  const VramNeed n = estimateVramNeed(fp8, w, h, f, resident, mps);
  const bool ok = n.gb >= measured && n.gb - measured <= maxOver;
  std::printf("%s %-34s est %5.1f GB vs measured %5.1f GB (model %dx%dx%d)\n", ok ? "ok  " : "FAIL", what, n.gb,
              measured, n.modelW, n.modelH, n.modelFrames);
  if (!ok) ++failures;
}

int main() {
  // Official CLI measurements (HD sizes as the model sees them).
  covers("HD 97 fp8", true, 1920, 1088, 97, false, 34.1, 4.0);
  covers("HD 145 fp8", true, 1920, 1088, 145, false, 41.3, 4.0);
  covers("HD 97 bf16", false, 1920, 1088, 97, false, 51.7, 4.0);
  covers("HD 145 bf16", false, 1920, 1088, 145, false, 58.8, 4.0);
  covers("UHD 49 fp8", true, 3840, 2176, 49, false, 54.1, 4.0);
  covers("UHD 49 bf16", false, 3840, 2176, 49, false, 67.8, 4.0);
  // Our worker (peak allocated + ~1.5 GB CUDA context/reserve slack).
  covers("worker HD 60->65 fp8", true, 1920, 1080, 60, false, 28.3 + 1.5, 4.0);
  covers("worker HD 60->65 bf16", false, 1920, 1080, 60, false, 45.5 + 1.5, 4.0);
  covers("worker HD 60->65 fp8 resident", true, 1920, 1080, 60, true, 37.7 + 1.5, 4.0);
  // Transformer kept (resident), measured as reserved + 0.7 GB context = what free memory lost.
  covers("A/B HD 145 fp8 resident, chunk 2", true, 1920, 1080, 145, true, 56.2 + 0.7, 4.0);
  covers("test 2: 720p 57 bf16 resident reused", false, 1280, 720, 50, true, 44.2, 4.0);
  covers("test 2: 720p 57 fp8 resident fresh", true, 1280, 720, 50, true, 28.0, 4.0);
  // Apple Silicon, M1 Ultra, our worker (decision 65): lowest free working set.
  covers("mps 720p 49 bf16", false, 1280, 720, 49, false, 52.3, 4.0, true);
  covers("mps HD 33 bf16", false, 1920, 1080, 33, false, 60.4, 4.0, true);
  covers("mps HD 73 bf16", false, 1920, 1080, 73, false, 79.7, 4.0, true);
  // Apple Silicon, M5 Max (CVT port, cvt1.05): peak reserved MPS memory, bf16.
  // cvt1.28 refit, peak reserved: the cvt1.05 points (43.1, 82.2 GB) were lowest-free based and
  // included other apps; the job log's peak reserved is the model's own need.
  covers("mps M5 HD 9 bf16", false, 1920, 1080, 9, false, 40.6, 3.0, 2);
  covers("mps M5 HD 33 bf16", false, 1920, 1080, 33, false, 43.1, 3.0, 2);
  covers("mps M5 HD 49 bf16", false, 1920, 1080, 49, false, 45.6, 3.0, 2);
  covers("mps M5 HD 73 bf16", false, 1920, 1080, 73, false, 50.2, 3.5, 2);
  covers("mps M5 HD 81 bf16", false, 1920, 1080, 81, false, 51.4, 4.0, 2);
  covers("mps M5 UHD 17 bf16", false, 3840, 2160, 17, false, 52.1, 3.0, 2);
  covers("mps M5 1280x1024 81 bf16", false, 1280, 1024, 81, false, 45.6, 3.5, 2);
  // cvt1.28: the MPS tensor limit: HD 81 and UHD 17 ran, HD 145 failed (INT_MAX).
  if (estimateVramNeed(false, 1920, 1080, 81, false, 2).overMpsLimit ||
      estimateVramNeed(false, 3840, 2160, 17, false, 2).overMpsLimit ||
      !estimateVramNeed(false, 1920, 1080, 145, false, 2).overMpsLimit ||
      !estimateVramNeed(false, 1920, 1080, 145, false, 1).overMpsLimit ||
      estimateVramNeed(false, 1920, 1080, 145, false, 0).overMpsLimit) {
    std::printf("FAIL mps tensor limit\n");
    ++failures;
  }
  // M1 Max 64 GB on the fp16 copy (cvt1.28): max(used, peak reserved), Fusion Studio.
  covers("mps M1 Max 448x448 17 fp16", false, 420, 432, 12, false, 40.6, 3.0, 3);
  covers("mps M1 Max HD 17 fp16", false, 1920, 1080, 12, false, 40.8, 3.0, 3);
  covers("mps M1 Max HD 33 fp16", false, 1920, 1080, 33, false, 43.0, 3.0, 3);
  // Padding: 1080 -> 1088, 60 -> 65 frames, frames capped at the chunk length 145.
  const VramNeed p = estimateVramNeed(true, 1920, 1080, 300, false);
  if (p.modelH != 1088 || p.modelFrames != 145) {
    std::printf("FAIL padding %dx%d f=%d\n", p.modelW, p.modelH, p.modelFrames);
    ++failures;
  }
  // A 48 GB card next to Flame (~46.5 GB free, 5% reserve) must still take HD 145 fp8.
  if (estimateVramNeed(true, 1920, 1080, 145, false).gb > 46.5 - 2.3) {
    std::printf("FAIL HD 145 fp8 no longer fits a 48 GB card\n");
    ++failures;
  }
  std::printf(failures ? "%d failure(s)\n" : "test_vram_estimate: all checks passed\n", failures);
  return failures ? 1 : 0;
}
