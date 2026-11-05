// Content-addressed per-frame matte cache on disk (decision 36).
// Layout under the cache root (must match worker/ltxag_worker/cache.py):
//   <root>/<k[0:2]>/<key>.matte   one matte: FrameFileHeader "LTXM", n=1, then w*h fp16
//   <root>/pending/<key>          exists while a job is computing that key; holds the job pid
//   <root>/jobs/<job id>.json     final job status (done / failed / cancelled + message)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "user_config.h"

namespace ltxag {

struct CacheSettings {
  std::string root;
  int maxAgeDays = 30;
  double capGB = 50.0;
};

CacheSettings cacheSettings(const UserConfig& cfg);

// Everything that changes the matte besides the source pixels. The original (upstream) key:
// still read for mattes made before cvt1.18.
std::string matteKey(uint64_t sourceFrameHash, const std::string& paramSignature);

// CVT port (cvt1.18): mattes are found by node, frame and settings, and a small thumbnail of the
// source frame decides whether the picture is still the one that was processed. An exact pixel
// hash broke on long-GOP MP4s: Resolve decoded some frames slightly differently while Process
// read the clip than on later redraws, so those frames never found their mattes.
//   <root>/<k[0:2]>/<key>.thumb   "LTXT" u32 version u32 w u32 h u32 c, then w*h*c float32
// A frame has kThumbSlots keys (copies of a node share its id; a copy on another clip takes
// another slot). Process reuses the slot whose thumbnail matches, else a free one, else slot 0.
constexpr int kThumbW = 64, kThumbH = 36, kThumbC = 4, kThumbSlots = 4;
std::string frameKey(const std::string& nodeId, int frame, const std::string& paramSignature, int slot);
std::string thumbPath(const std::string& root, const std::string& key);
bool writeThumb(const std::string& root, const std::string& key, const std::vector<float>& thumb);
bool readThumb(const std::string& root, const std::string& key, std::vector<float>& thumb);
// The thumbnail of a canvas (w*h RGBA floats, rows top-down): the mean of each of the
// kThumbW x kThumbH cells, per channel, clamped to [0, 2] (NaN -> 0). Pure, for the tests.
void thumbnailOf(const float* rgba, int w, int h, std::vector<float>& out);
// Same picture? Mean absolute difference <= 0.02 and no cell channel off by more than 0.25:
// decoder noise passes, a grade, another clip or a moved crop does not. Pure.
bool thumbsMatch(const std::vector<float>& a, const std::vector<float>& b, double* meanDiff = nullptr,
                 double* maxDiff = nullptr);

std::string mattePath(const std::string& root, const std::string& key);
// cvt1.34: has this node a matte for `frame` in any slot? For the check when a render starts,
// which has no picture yet, so it can't tell whether the picture has changed since Process.
bool frameHasMatte(const std::string& root, const std::string& nodeId, int frame, const std::string& paramSignature);
std::string pendingPath(const std::string& root, const std::string& key);

// Reads one matte (w*h halves, rows top-down) and refreshes its mtime for the LRU sweep.
bool readMatte(const std::string& root, const std::string& key, uint32_t w, uint32_t h,
               std::vector<uint16_t>& out);

// True while a live job holds the key's pending marker.
bool isPending(const std::string& root, const std::string& key);

// Written by the plugin just before it launches a job (with Flame's pid), so a render that
// arrives before job.py has marked the keys still shows PROCESSING; job.py then rewrites them
// with its own pid and removes them at the end. clearPending undoes it if the launch fails.
void markPending(const std::string& root, const std::vector<std::string>& keys, int pid);
void clearPending(const std::string& root, const std::vector<std::string>& keys);

size_t purgeKeys(const std::string& root, const std::vector<std::string>& keys);
uint64_t cacheBytes(const std::string& root);
uint64_t purgeAll(const std::string& root);  // returns bytes freed

struct SweepResult {
  size_t aged = 0, capped = 0, kept = 0;
  uint64_t bytesFreed = 0, bytesKept = 0;
};
// Deletes mattes unused for maxAgeDays, then least-recently-used until under capGB.
// Also clears pending markers of dead jobs.
SweepResult sweepCache(const CacheSettings& s);

// Removes stagingDir()/ltxag_* files whose owner is gone (test 1 left a 1.5 GB orphan).
size_t sweepStaleStaging();

bool pidAlive(int pid);

}  // namespace ltxag
