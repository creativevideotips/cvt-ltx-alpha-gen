#include "cache.h"

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>

#include "oscompat.h"
#include "platform.h"
#include "transport.h"

namespace ltxag {

namespace {

std::vector<std::string> listDir(const std::string& dir) {
  std::vector<std::string> out;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] != '.') out.emplace_back(e->d_name);
    }
    closedir(d);
  }
  return out;
}

bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Makes a folder with every level above it. The cache folder may not be there at a Process (it
// was deleted, or a new Cache Folder was typed in): without it the thumbnails and the pending
// markers could not be written, and that Process ended in MATTE MISSING (found on Windows, where
// a first Process in a dev build ran before any Setup had made the folder).
void makeDirs(const std::string& dir) {
#ifdef _WIN32
  const char* const separators = "/\\";
#else
  const char* const separators = "/";
#endif
  for (size_t pos = 1; (pos = dir.find_first_of(separators, pos)) != std::string::npos; ++pos)
    mkdir(dir.substr(0, pos).c_str(), 0755);
  mkdir(dir.c_str(), 0755);
}

// cvt1.18: deleting a matte also deletes its thumbnail (<key>.thumb beside <key>.matte).
void unlinkThumbOf(const std::string& mattePathStr) {
  if (endsWith(mattePathStr, ".matte"))
    unlink((mattePathStr.substr(0, mattePathStr.size() - 6) + ".thumb").c_str());
}

int readPid(const std::string& path) {
  std::ifstream in(fsPath(path));
  int pid = 0;
  in >> pid;
  return pid;
}

}  // namespace

#ifdef _WIN32
bool pidAlive(int pid) { return processAlive(pid); }
#else
bool pidAlive(int pid) { return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM); }
#endif

CacheSettings cacheSettings(const UserConfig& cfg) {
  CacheSettings s;
  s.root = cfg.count("cache_dir") ? cfg.at("cache_dir") : homeDirectory() + "/.cache/ltx-alphagen/mattes";
  if (cfg.count("cache_max_days")) s.maxAgeDays = std::atoi(cfg.at("cache_max_days").c_str());
  if (cfg.count("cache_cap_gb")) s.capGB = std::atof(cfg.at("cache_cap_gb").c_str());
  return s;
}

std::string matteKey(uint64_t sourceFrameHash, const std::string& paramSignature) {
  uint64_t h = 1469598103934665603ULL;
  for (int i = 0; i < 8; ++i) h = (h ^ ((sourceFrameHash >> (8 * i)) & 0xff)) * 1099511628211ULL;
  for (unsigned char c : paramSignature) h = (h ^ c) * 1099511628211ULL;
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

bool frameHasMatte(const std::string& root, const std::string& nodeId, int frame, const std::string& paramSignature) {
  for (int s = 0; s < kThumbSlots; ++s) {
    struct stat st {};
    if (stat(mattePath(root, frameKey(nodeId, frame, paramSignature, s)).c_str(), &st) == 0) return true;
  }
  return false;
}

std::string mattePath(const std::string& root, const std::string& key) {
  return root + "/" + key.substr(0, 2) + "/" + key + ".matte";
}

std::string frameKey(const std::string& nodeId, int frame, const std::string& paramSignature, int slot) {
  const std::string id = "v2|" + nodeId + "|" + std::to_string(frame) + "|" + paramSignature;
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : id) h = (h ^ c) * 1099511628211ULL;
  char buf[24];
  std::snprintf(buf, sizeof buf, "%016llxs%d", static_cast<unsigned long long>(h), slot);
  return buf;
}

std::string thumbPath(const std::string& root, const std::string& key) {
  return root + "/" + key.substr(0, 2) + "/" + key + ".thumb";
}

bool writeThumb(const std::string& root, const std::string& key, const std::vector<float>& thumb) {
  if (thumb.size() != size_t(kThumbW) * kThumbH * kThumbC) return false;
  makeDirs(root + "/" + key.substr(0, 2));
  const std::string path = thumbPath(root, key), tmp = path + ".tmp" + std::to_string(getpid());
  std::ofstream f(fsPath(tmp), std::ios::binary | std::ios::trunc);
  const uint32_t head[5] = {0x5458544Cu /* "LTXT" */, 1u, uint32_t(kThumbW), uint32_t(kThumbH), uint32_t(kThumbC)};
  f.write(reinterpret_cast<const char*>(head), sizeof head);
  f.write(reinterpret_cast<const char*>(thumb.data()), std::streamsize(thumb.size() * sizeof(float)));
  f.close();
  if (!f) {
    unlink(tmp.c_str());
    return false;
  }
  chmod(tmp.c_str(), 0644);  // cvt1.43: written inside Resolve, whose umask is 000
  return renameOver(tmp.c_str(), path.c_str()) == 0;  // atomic: a render never reads half a thumbnail
}

bool readThumb(const std::string& root, const std::string& key, std::vector<float>& thumb) {
  std::ifstream f(fsPath(thumbPath(root, key)), std::ios::binary);
  uint32_t head[5] = {0, 0, 0, 0, 0};
  if (!f.read(reinterpret_cast<char*>(head), sizeof head)) return false;
  if (head[0] != 0x5458544Cu || head[1] != 1u || head[2] != uint32_t(kThumbW) || head[3] != uint32_t(kThumbH) ||
      head[4] != uint32_t(kThumbC))
    return false;
  thumb.assign(size_t(kThumbW) * kThumbH * kThumbC, 0.0f);
  return bool(f.read(reinterpret_cast<char*>(thumb.data()), std::streamsize(thumb.size() * sizeof(float))));
}

void thumbnailOf(const float* rgba, int w, int h, std::vector<float>& out) {
  out.assign(size_t(kThumbW) * kThumbH * kThumbC, 0.0f);
  if (!rgba || w <= 0 || h <= 0) return;
  std::vector<double> sum(out.size(), 0.0);
  std::vector<uint32_t> count(size_t(kThumbW) * kThumbH, 0);
  for (int y = 0; y < h; ++y) {
    const int cy = int(int64_t(y) * kThumbH / h);
    const float* row = rgba + size_t(y) * w * 4;
    for (int x = 0; x < w; ++x) {
      const int cell = cy * kThumbW + int(int64_t(x) * kThumbW / w);
      ++count[size_t(cell)];
      for (int c = 0; c < kThumbC; ++c) {
        const float v = row[size_t(x) * 4 + c];
        sum[size_t(cell) * kThumbC + c] += (v == v) ? std::min(2.0f, std::max(0.0f, v)) : 0.0f;  // NaN -> 0
      }
    }
  }
  for (size_t cell = 0; cell < count.size(); ++cell)
    for (int c = 0; c < kThumbC; ++c)
      if (count[cell]) out[cell * kThumbC + c] = float(sum[cell * kThumbC + c] / count[cell]);
}

bool thumbsMatch(const std::vector<float>& a, const std::vector<float>& b, double* meanDiff, double* maxDiff) {
  double total = 0.0, worst = 0.0;
  const bool sameSize = a.size() == b.size() && !a.empty();
  if (sameSize) {
    for (size_t i = 0; i < a.size(); ++i) {
      const double d = std::fabs(double(a[i]) - double(b[i]));
      total += d;
      worst = std::max(worst, d);
    }
  }
  const double mean = sameSize ? total / double(a.size()) : 1.0;
  if (!sameSize) worst = 1.0;
  if (meanDiff) *meanDiff = mean;
  if (maxDiff) *maxDiff = worst;
  return sameSize && mean <= 0.02 && worst <= 0.25;
}

std::string pendingPath(const std::string& root, const std::string& key) {
  return root + "/pending/" + key;
}

bool readMatte(const std::string& root, const std::string& key, uint32_t w, uint32_t h,
               std::vector<uint16_t>& out) {
  const std::string path = mattePath(root, key);
  std::string err;
  if (!readMattes(path, w, h, 1, out, err)) return false;
  utimensat(AT_FDCWD, path.c_str(), nullptr, 0);  // mark as recently used
  return true;
}

bool isPending(const std::string& root, const std::string& key) {
  const std::string p = pendingPath(root, key);
  if (access(p.c_str(), F_OK) != 0) return false;
  return pidAlive(readPid(p));
}

void markPending(const std::string& root, const std::vector<std::string>& keys, int pid) {
  makeDirs(root + "/pending");
  const std::string text = std::to_string(pid);
  for (const auto& k : keys) {
    const int fd = open(pendingPath(root, k).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) continue;
    (void)!write(fd, text.data(), text.size());
    close(fd);
  }
}

void clearPending(const std::string& root, const std::vector<std::string>& keys) {
  for (const auto& k : keys) unlink(pendingPath(root, k).c_str());
}

size_t purgeKeys(const std::string& root, const std::vector<std::string>& keys) {
  size_t n = 0;
  for (const auto& k : keys) {
    if (unlink(mattePath(root, k).c_str()) == 0) ++n;
    unlink(thumbPath(root, k).c_str());  // cvt1.18
  }
  return n;
}

uint64_t cacheBytes(const std::string& root) {
  uint64_t total = 0;
  for (const auto& sub : listDir(root)) {
    if (sub.size() != 2) continue;
    for (const auto& f : listDir(root + "/" + sub)) {
      struct stat st {};
      if ((endsWith(f, ".matte") || endsWith(f, ".thumb")) && stat((root + "/" + sub + "/" + f).c_str(), &st) == 0)
        total += uint64_t(st.st_size);
    }
  }
  return total;
}

uint64_t purgeAll(const std::string& root) {
  uint64_t freed = 0;
  for (const auto& sub : listDir(root)) {
    if (sub.size() != 2) continue;
    for (const auto& f : listDir(root + "/" + sub)) {
      const std::string p = root + "/" + sub + "/" + f;
      struct stat st {};
      if ((endsWith(f, ".matte") || endsWith(f, ".thumb")) && stat(p.c_str(), &st) == 0 && unlink(p.c_str()) == 0)
        freed += uint64_t(st.st_size);
    }
  }
  return freed;
}

SweepResult sweepCache(const CacheSettings& s) {
  SweepResult r;
  struct Entry {
    std::string path;
    time_t mtime;
    uint64_t size;
  };
  std::vector<Entry> entries;
  const time_t now = time(nullptr);
  const time_t maxAge = time_t(s.maxAgeDays) * 86400;
  for (const auto& sub : listDir(s.root)) {
    if (sub.size() != 2) continue;
    for (const auto& f : listDir(s.root + "/" + sub)) {
      const std::string p = s.root + "/" + sub + "/" + f;
      struct stat st {};
      if (endsWith(f, ".thumb")) {  // cvt1.18: a thumbnail without its matte (job failed) after a day
        const std::string matte = p.substr(0, p.size() - 6) + ".matte";
        if (access(matte.c_str(), F_OK) != 0 && stat(p.c_str(), &st) == 0 && now - st.st_mtime > 86400)
          unlink(p.c_str());
        continue;
      }
      if (!endsWith(f, ".matte")) continue;
      if (stat(p.c_str(), &st) != 0) continue;
      if (s.maxAgeDays > 0 && now - st.st_mtime > maxAge) {
        if (unlink(p.c_str()) == 0) {
          unlinkThumbOf(p);
          ++r.aged;
          r.bytesFreed += uint64_t(st.st_size);
        }
        continue;
      }
      entries.push_back({p, st.st_mtime, uint64_t(st.st_size)});
    }
  }
  uint64_t total = 0;
  for (const auto& e : entries) total += e.size;
  const uint64_t cap = uint64_t(s.capGB * 1024.0 * 1024.0 * 1024.0);
  if (s.capGB > 0 && total > cap) {
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.mtime < b.mtime; });  // oldest first
    for (const auto& e : entries) {
      if (total <= cap) break;
      if (unlink(e.path.c_str()) == 0) {
        unlinkThumbOf(e.path);
        total -= e.size;
        ++r.capped;
        r.bytesFreed += e.size;
      }
    }
  }
  r.bytesKept = total;
  r.kept = entries.size() - r.capped;
  // Pending markers of jobs that no longer run.
  for (const auto& f : listDir(s.root + "/pending")) {
    const std::string p = s.root + "/pending/" + f;
    if (!pidAlive(readPid(p))) unlink(p.c_str());
  }
  return r;
}

size_t sweepStaleStaging() {
  // ltxag_<flame pid>_<seq>_*.bin : transient files of a Flame process -> owner must be alive.
  // ltxag_job_<id>[_...|.ext]     : handed to a job (<id> has no '_' or '.');
  //                                 ltxag_job_<id>.pid names the job's pid.
  size_t removed = 0;
  const std::string dir = stagingDir();
  const auto files = listDir(dir);
  const time_t now = time(nullptr);
  const std::string jobPrefix = "ltxag_job_";
  auto jobBase = [&](const std::string& f) {
    return f.substr(0, f.find_first_of("_.", jobPrefix.size()));
  };
  // Decide every job's fate before deleting anything: removing a .pid file first
  // would make its sibling files look like a job that has not started yet.
  std::vector<std::string> deadJobs, liveJobs;
  for (const auto& f : files)
    if (f.rfind(jobPrefix, 0) == 0 && endsWith(f, ".pid"))
      (pidAlive(readPid(dir + "/" + f)) ? liveJobs : deadJobs).push_back(jobBase(f));
  auto contains = [](const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
  };
  for (const auto& f : files) {
    if (f.rfind("ltxag_", 0) != 0) continue;
    const std::string p = dir + "/" + f;
    bool stale = false;
    if (f.rfind(jobPrefix, 0) == 0) {
      const std::string base = jobBase(f);
      struct stat st {};
      const bool young = stat(p.c_str(), &st) == 0 && now - st.st_mtime < 600;
      // No pid file yet: a job being launched (young) or one that never started (old).
      stale = contains(deadJobs, base) || (!contains(liveJobs, base) && !young);
    } else {
      stale = !pidAlive(std::atoi(f.c_str() + 6));
    }
    if (stale && unlink(p.c_str()) == 0) ++removed;
  }
  return removed;
}

}  // namespace ltxag
