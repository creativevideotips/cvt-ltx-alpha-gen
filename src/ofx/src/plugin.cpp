// LTX Alpha Gen OFX — PROTOTYPE round 3 (upstream-notes/overview.md, decisions 35–43).
// - Render never runs the model: it serves mattes from the disk cache, else a faint
//   "no media"-style marker (Flame calls render on its UI thread — Flame test 1).
// - Process stages the clip's frames in stagingDir() and launches a detached job
//   (worker/job.py) with its own progress window and Cancel button.
// - A watcher thread follows the job and nudges Flame to re-render (decision 42, option a:
//   setting params outside an action breaks the OFX threading rules — experiment, toggleable).
// - Layout follows Flame's observed rules: one group = one column of <= 5 rows.
// - Licence + model gating (bypassable only in dev builds). Logging per decision 39.
// Not in the prototype: OCIO (colour space only feeds the cache key), real model, resident worker.

#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <signal.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

#include "cache.h"
#include "chunk_plan.h"
#include "motion.h"
#include "env_info.h"
#include "half.h"
#include "log.h"
#include "marker.h"
#include "ofxColour.h"
#include "ofxsImageEffect.h"
#include "ofxsSupportPrivate.h"
#include "platform.h"
#include "process.h"
#include "roi.h"
#include "transport.h"
#include "user_config.h"
#include "vram_estimate.h"

#ifndef LTXAG_VERSION  // from CMakeLists.txt
#define LTXAG_VERSION "0.0.0"
#endif
#ifndef LTXAG_GIT
#define LTXAG_GIT "unknown"
#endif
#ifndef LTXAG_VERSION_MAJOR  // from CMakeLists.txt LTXAG_VERSION
#define LTXAG_VERSION_MAJOR 0
#define LTXAG_VERSION_MINOR 0
#endif
#ifndef LTXAG_DEV_BUILD
#define LTXAG_DEV_BUILD 0
#endif

namespace {

using namespace ltxag;

// CVT Resolve port: its own id and menu group, so it never clashes with the Flame build.
constexpr const char* kPluginId = "com.creativevideotips.ltx.alphagen";
constexpr const char* kPluginLabel = "LTX Alpha Gen";
constexpr const char* kPluginGroup = "Creative Video Tips";  // cvt1.36: was CVT; the panel's category
// Part of every cache key: which worker/model produced a matte (dummy and real never mix).
constexpr const char* kModelVersion = "ltx2.5-alphagen0.9-recipe1";
constexpr const char* kDummyVersion = "dummy-0.3";

// Decision 34(a): bf16 is the built-in default only if a full HD chunk fits (vram_estimate.h).
constexpr int kDefaultCheckW = 1920, kDefaultCheckH = 1080, kDefaultCheckFrames = 145;
// Decision 72: Process refuses a crop whose long side reaches this (every 6K-class format;
// 3840x2176 is measured to work, 6144x3264 fails in the LTX decoder).
constexpr int kMaxLongSide = 6000;

// Input Colour (docs/colour-auto.md): Auto asks the host (OpenFX 1.5 colour management) for the
// source in kAutoColourspace; the worker then applies the fixed ACES 2.0 SDR view, as the old
// dropdown's ACEScg entry did. Bypass: no request, no conversion. Setups store the index.
const char* const kColourModeNames[] = {"Auto", "Bypass"};
constexpr int kColourAuto = 0, kColourBypass = 1;
constexpr const char* kAutoColourspace = "ACEScg";
// Auto: the image output is display-referred (acescgToDisplay, marker.h), decision 76.
constexpr const char* kOutputDisplay = "Rec.1886 Rec.709 - Display";
// The old Input Colour dropdown (until 0.2.1): kept defined and hidden so saved setups load; a
// node saved with a colour space other than Bypass is moved to Auto once (createInstance).
const char* const kLegacyColourSpaces[] = {"Bypass", "ACEScg", "LogC4", "Log3G10", "LogC3", "BMD Film Gen5"};
const char* const kPrecisions[] = {"bf16", "fp8"};
const char* const kSimulations[] = {"none", "oom", "crash", "hang"};
// Chunk Size (decision 61): index 0 is Auto, then chunk_plan.h kSizeLen. Setups store the index.
// cvt1.29: Full (145) is gone from the choice: at HD it is over the Apple Silicon size limit
// (vram_estimate.h). Auto still uses 145 when the picture is small enough (an ROI crop, 720p).
// A new param name, as setups store the index; the old param stays hidden and ignored.
const char* const kChunkSizeNames[] = {"Auto", "Half (73)", "Quarter (33)"};
constexpr int kChunkSizeChoices = 3;
const char* const kLegacyChunkSizeNames[] = {"Auto", "Full (145)", "Half (73)", "Quarter (33)"};
// Merge (decision 62): how overlapping chunks are joined.
// cvt1.19: "Dissolve" (was "Smooth"); the index is what setups store, so saved comps keep it.
const char* const kMergeNames[] = {"Dissolve", "Flow Warp"};
// Frame Hold (decision 74): each frame repeated 1/2/4 times for fast motion. cvt1.26: Off, 2x
// and 4x only, under a new param name (setups store the index). Auto (75) planned chunks it
// couldn't size well on a Mac and its cost couldn't be predicted; 8x was barely better than 4x
// at twice the time. The old param (Off, Auto, 2x, 4x, 8x) stays defined and hidden, so comps
// saved with it load; it is ignored. The Auto code (motion.h, planAutoChunks) stays, unused.
const char* const kFrameHoldNames[] = {"Off", "2x", "4x"};
const int kFrameHolds[] = {1, 2, 4};
constexpr int kFrameHoldChoices = 3;
const char* const kLegacyFrameHoldNames[] = {"Off", "Auto", "2x", "4x", "8x"};
constexpr int kAutoMaxHold = 4;
// Output (CVT Resolve port): what the RGB channels carry; the alpha is always the matte.
// Source: the picture untouched, ready for a Merge or a mask in Fusion. Matte: the matte in RGB
// too (to view it, or for the Edit and Color pages). Overlay: the upstream magenta preview.
const char* const kOutputNames[] = {"Source + Matte Alpha", "Matte", "Overlay"};
constexpr int kOutSource = 0, kOutMatte = 1, kOutOverlay = 2;

// Params. Column (group) in brackets; see upstream-notes/ofx.md (layout, decisions 37/52).
// [Alpha Gen]
constexpr const char* kPColourMode = "inputColour";
constexpr const char* kPOutput = "outputMode";        // CVT Resolve port
constexpr const char* kPRefresh = "refreshViewer";    // CVT Resolve port
constexpr const char* kPWebsite = "openWebsite";      // CVT port (cvt1.06)
constexpr const char* kPModelPage = "openModelPage";  // CVT port (cvt1.39)
constexpr const char* kPLoraPage = "openLoraPage";    // CVT port (cvt1.39)
constexpr const char* kPTokenPage = "openTokenPage";  // CVT port (cvt1.40)
constexpr const char* kPShowSetup = "showSetupOptions";  // CVT port (cvt1.10)
constexpr const char* kPPostMultiply = "postMultiply";   // CVT port (cvt1.10)
constexpr const char* kPSeams = "seamsInfo";             // CVT port (cvt1.20)
constexpr const char* kPBlackClip = "alphaBlackClip";     // CVT port (cvt1.26)
constexpr const char* kPWhiteClip = "alphaWhiteClip";     // CVT port (cvt1.26)
constexpr const char* kWebsiteUrl = "https://creativevideotips.com";
constexpr const char* kModelPageUrl = "https://huggingface.co/Lightricks/LTX-2.5";
constexpr const char* kTokenPageUrl = "https://huggingface.co/settings/tokens";
constexpr const char* kLoraPageUrl =
    "https://huggingface.co/Lightricks/LTX-2.5-22b-IC-LoRA-Alpha-Gen";
constexpr const char* kPProcess = "process";
constexpr const char* kPStatus = "status";
constexpr const char* kPShowMarker = "showMarker";
// [Performance]
constexpr const char* kPPrecision = "precision";
constexpr const char* kPUnload = "unloadAfterRun";
constexpr const char* kPHeadroom = "headroomGB";
constexpr const char* kPSaveDefault = "saveAsDefault";
// [Licence] — per user: not saved in setups, mirrored to the user config.
constexpr const char* kPViewLicence = "viewLicence";
constexpr const char* kPAccept = "acceptLicence";
constexpr const char* kPTokenCaption = "tokenCaption";
constexpr const char* kPToken = "hfToken";
constexpr const char* kPDownload = "download";
// [Models] — per user.
constexpr const char* kPModelDir = "modelDir";
constexpr const char* kPExistingDir = "existingModels";
constexpr const char* kPLinkExisting = "linkExisting";
constexpr const char* kPChooseExisting = "chooseExistingModels";
constexpr const char* kPChooseModel = "chooseModelFolder";
constexpr const char* kPChooseCache = "chooseCacheFolder";
constexpr const char* kPSetupStatus = "setupStatus";
constexpr const char* kPDebugLog = "debugLogging";
constexpr const char* kPCollectLogs = "collectLogs";
// [Chunking]
constexpr const char* kPChunkSize = "chunkSizeMode";      // cvt1.29: Auto, Half, Quarter
constexpr const char* kPLegacyChunkSize = "chunkSize";    // before cvt1.29: hidden, ignored
constexpr const char* kPMerge = "mergeMode";
// [Motion]
constexpr const char* kPFrameHold = "frameHoldMode";  // cvt1.26: Off, 2x, 4x
constexpr const char* kPLegacyFrameHold = "frameHold";  // before cvt1.26: hidden, ignored
constexpr const char* kPSkipMemCheck = "skipMemoryCheck";  // cvt1.27
constexpr const char* kPFirstFrame = "processFirstFrame";  // cvt1.35, Resolve only
constexpr const char* kPLastFrame = "processLastFrame";    // cvt1.35, Resolve only
// [ROI]
constexpr const char* kPRoiMargin = "roiMargin";
constexpr const char* kPRoiMask = "roiMaskOutside";
constexpr const char* kPRoiLimit = "roiLimit";
constexpr const char* kPRoiInfo = "roiInfo";
// [Cache] — per user, except Purge Cache (this node).
constexpr const char* kPCacheDir = "cacheDir";
constexpr const char* kPCacheInfo = "cacheInfo";
constexpr const char* kPCacheLimit = "cacheLimitGB";  // CVT port (cvt1.14), per user
constexpr const char* kPPurgeNode = "purgeCache";
constexpr const char* kPPurgeAll = "purgeAll";
// [Prototype] — dev builds only.
constexpr const char* kPSimSeconds = "simSeconds";
constexpr const char* kPSimulate = "simulate";
constexpr const char* kPSimNeed = "simNeedGB";
constexpr const char* kPPython = "pythonPath";
constexpr const char* kPEnforceGate = "enforceGate";
constexpr const char* kPAutoRefresh = "autoRefresh";
constexpr const char* kPUseDummy = "useDummy";
// Hidden.
constexpr const char* kPInitialized = "initialized";
constexpr const char* kPNodeId = "nodeId";
constexpr const char* kPCreditPort = "creditPort";  // cvt1.24: its change means Fusion's Reset
constexpr const char* kPRefreshTick = "refreshTick";
constexpr const char* kPRoiState = "roiState";
constexpr const char* kPCanvas = "canvasState";  // CVT port (cvt1.07): the canvas of the last Process
constexpr const char* kPColour = "colourSpace";  // the old Input Colour dropdown (hidden)

constexpr const char* kTokenCaptionDefault = "Hugging Face token:";

std::string resourcesDir() {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(&resourcesDir), &info) || !info.dli_fname) return "";
  std::string p = info.dli_fname;  // .../Contents/{Linux-x86-64,MacOS}/LTXAlphaGen.ofx
  p = p.substr(0, p.rfind('/'));
  p = p.substr(0, p.rfind('/'));
  return p + "/Resources";
}

// cvt1.42: are Apple's Command Line Tools (or Xcode) installed? /usr/bin/python3 is only a stub
// that needs them, so without them Setup's helper can't start and can't report it. Looks on disk
// because running the stub, or xcrun, can open Apple's install window. Any real python3 counts, so
// an unusual Xcode location that xcode-select knows about passes.
// LTXAG_TEST_NO_APPLE_TOOLS: the system test's way to see the failure on a Mac that has them.
bool appleToolsInstalled() {
  if (std::getenv("LTXAG_TEST_NO_APPLE_TOOLS")) return false;
  std::vector<std::string> roots = {"/Library/Developer/CommandLineTools",
                                    "/Applications/Xcode.app/Contents/Developer"};
  for (const char* selected : {"/var/select/developer_dir", "/var/db/xcode_select_link"}) {
    char link[1024];
    const ssize_t n = readlink(selected, link, sizeof(link));
    if (n > 0) roots.emplace_back(link, size_t(n));
  }
  for (const auto& root : roots)
    if (access((root + "/usr/bin/python3").c_str(), X_OK) == 0) return true;
  return false;
}

std::string homeDir() {
  const char* h = std::getenv("HOME");
  return h ? h : "/tmp";
}

std::string timeStr(const char* fmt) {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  char b[32];
  std::strftime(b, sizeof b, fmt, &tm);
  return b;
}

// "/a/b/" -> "/a/b" (but "/" stays "/").
std::string trimSlashes(std::string p) {
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p;
}

std::string fmtGB(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.1f", v);
  return b;
}

uint64_t fnv(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ULL;
  return h;
}

std::string randomId() {
  std::random_device rd;
  char b[9];
  std::snprintf(b, sizeof b, "%08x", rd());
  return b;
}

// Value of "key" in the job status JSON written by Python's json.dump.
std::string jsonField(const std::string& j, const std::string& key) {
  const std::string k = "\"" + key + "\": ";
  const auto p = j.find(k);
  if (p == std::string::npos) return "";
  const size_t b = p + k.size();
  if (b < j.size() && j[b] == '"') return j.substr(b + 1, j.find('"', b + 1) - b - 1);
  const auto e = j.find_first_of(",}", b);
  return j.substr(b, e - b);
}

std::string readFile(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Licence text shown by "View Licence"; its hash is stored on acceptance (decision 38).
std::string licencePath() { return resourcesDir() + "/licence.txt"; }
std::string licenceHash() {
  char b[17];
  std::snprintf(b, sizeof b, "%016llx",
                static_cast<unsigned long long>(fnv(readFile(licencePath()))));
  return b;
}

// The Model Folder LTX Inpaint Still was set up with, "" if it hasn't been. They share the models.
std::string inpaintStillModelFolder() {
  std::ifstream in(homeDir() + "/.config/ltx-inpaint-still/config.conf");
  std::string line;
  while (std::getline(in, line))
    if (line.rfind("model_dir=", 0) == 0) {
      struct stat st {};
      return stat(line.c_str() + 10, &st) == 0 ? line.substr(10) : "";
    }
  return "";
}

struct SetupState {
  bool licenceAccepted = false;
  bool modelsReady = false;
  bool ready() const { return licenceAccepted && modelsReady; }
};

SetupState setupState() {
  const UserConfig cfg = loadUserConfig();
  SetupState s;
  s.licenceAccepted = cfg.count("licence_accepted") && cfg.count("licence_hash") &&
                      cfg.at("licence_hash") == licenceHash();
  // Ready = the setup helper finished (models verified, prompt cache built) AND the runtime exists.
  struct stat st {};
  s.modelsReady = cfg.count("models_ready") > 0 && cfg.count("ltx_python") > 0 &&
                  stat(cfg.at("ltx_python").c_str(), &st) == 0;
  return s;
}

// Built-in defaults, overridden by the user config (decisions 21, 31, 32, 34).
struct Defaults {
  int precision = 0;
  double headroomGB = 0.0;
  bool unload = true;
  std::string source;
};

// Apple Silicon runs the model on PyTorch-MPS (decision 65): bf16 only (MPS has no fp8 dtype)
// and its own memory model (vram_estimate.h).
#ifdef __APPLE__
constexpr bool kMps = true;
#else
constexpr bool kMps = false;
#endif

// CVT port (cvt1.05): which Mac memory fit the estimate uses (vram_estimate.h): 2 for M5 and
// newer ("Apple M5 Max"), 1 (the cautious M1 fit) for older or unknown chips, 0 off a Mac.
int mpsFit() {
  if (!kMps) return 0;
  static const int fit = [] {
    const std::string name = queryVram().name;
    const size_t at = name.find("Apple M");
    const int gen = at == std::string::npos ? 0 : std::atoi(name.c_str() + at + 7);
    LOGI("memory estimate: GPU '%s', chip generation %d -> %s fit", name.c_str(), gen, gen >= 5 ? "M5" : "M1");
    return gen >= 5 ? 2 : 1;
  }();
  return fit;
}

// Decision 68: on a Mac where Setup made the fp16 copy of the transformer (because fp16 is faster
// there), the Precision choice "bf16" runs as "fp16": same matte (mean difference 0.0002), its
// own cache entries and worker. The dropdown itself stays bf16 / fp8.
std::string effectivePrecision(int prec) {
  if (kMps && prec == 0) {
    const UserConfig cfg = loadUserConfig();
    if (cfg.count("transformer_fp16") && cfg.at("transformer_fp16") == "1") return "fp16";
  }
  return kPrecisions[prec];
}

// cvt1.28: the memory fit for this precision: an M1-class Mac running the fp16 copy has its own
// (measured on a 64 GB M1 Max, vram_estimate.h); otherwise mpsFit().
int memoryFit(int prec) {
  const int fit = mpsFit();
  return fit == 1 && effectivePrecision(prec) == "fp16" ? 3 : fit;
}

Defaults computeDefaults() {
  Defaults d;
  const VramInfo v = queryVram();
  d.headroomGB = v.ok ? std::round(v.freeGB * 0.05 * 10.0) / 10.0 : 2.0;
  const double bf16Need =
      estimateVramNeed(false, kDefaultCheckW, kDefaultCheckH, kDefaultCheckFrames, false, mpsFit()).gb;
  d.precision = (!kMps && v.ok && v.freeGB - d.headroomGB < bf16Need) ? 1 : 0;
  std::string src = v.ok ? "built-in (free " + fmtGB(v.freeGB) + " of " + fmtGB(v.totalGB) + " GB)"
                         : "built-in (GPU memory unavailable)";
  const UserConfig cfg = loadUserConfig();
  if (cfg.count("precision")) d.precision = cfg.at("precision") == "fp8" ? 1 : 0;
  if (cfg.count("headroom_gb")) d.headroomGB = std::atof(cfg.at("headroom_gb").c_str());
  if (cfg.count("unload")) d.unload = cfg.at("unload") == "1";
  if (cfg.count("precision") || cfg.count("headroom_gb") || cfg.count("unload"))
    src += " + user config";
  d.source = src;
  return d;
}

uint64_t hashImage(const OFX::Image& img) {
  const OfxRectI b = img.getBounds();
  const size_t rowBytes = size_t(b.x2 - b.x1) * 4 *
                          (img.getPixelComponents() == OFX::ePixelComponentRGBA ? 4 : 3);
  uint64_t h = 1469598103934665603ULL;
  for (int y = b.y1; y < b.y2; ++y) {
    const auto* row = static_cast<const unsigned char*>(img.getPixelAddress(b.x1, y));
    size_t i = 0;
    for (; i + 8 <= rowBytes; i += 8) {
      uint64_t w;
      std::memcpy(&w, row + i, 8);
      h = (h ^ w) * 1099511628211ULL;
    }
    for (; i < rowBytes; ++i) h = (h ^ row[i]) * 1099511628211ULL;
  }
  return h;
}

std::string jsonStr(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o + "\"";
}

// OpenFX 1.5 colour properties through the raw property suite (the 1.4 Support library has no
// colour calls; same approach as Matte Refine ML, plugin_ml.cpp). A string property's first
// value, or "" when the host does not set it (a host without colour management).
std::string propString(OfxPropertySetHandle h, const char* prop) {
  char* s = nullptr;
  if (!h || OFX::Private::gPropSuite->propGetString(h, prop, 0, &s) != kOfxStatOK || !s) return "";
  return s;
}

HostInfo hostInfo() {
  HostInfo h;
  if (const auto* d = OFX::getImageEffectHostDescription()) {
    h.name = d->hostName;
    h.label = d->hostLabel;
    h.version = d->versionLabel + " (" + std::to_string(d->versionMajor) + "." +
                std::to_string(d->versionMinor) + "." + std::to_string(d->versionMicro) + ")";
    h.background = d->hostIsBackground;
  }
  return h;
}

// CVT Resolve port: the host's short name for messages and hints ("Resolve", "Flame").
bool containsNoCase(std::string s, const char* what) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s.find(what) != std::string::npos;
}

bool hostIsResolve() {
  const HostInfo h = hostInfo();
  return containsNoCase(h.name, "resolve") || containsNoCase(h.label, "resolve");
}

bool hostIsFusion() {
  const HostInfo h = hostInfo();
  return !hostIsResolve() && (containsNoCase(h.name, "fusion") || containsNoCase(h.label, "fusion"));
}

// CVT port (cvt1.05): Resolve and Fusion Studio share the Fusion OFX host, so the Resolve
// workarounds (no colour management, label params drawn by name) apply to both.
bool hostIsBlackmagic() { return hostIsResolve() || hostIsFusion(); }

std::string hostShort() {
  const HostInfo h = hostInfo();
  if (hostIsResolve()) return "Resolve";
  if (hostIsFusion()) return "Fusion";
  if (containsNoCase(h.name, "flame") || containsNoCase(h.label, "flame")) return "Flame";
  return h.label.empty() ? std::string("the host") : h.label;
}

// What the last Process decided about the ROI, saved with the setup (hidden param roiState).
// Render needs it for the cache key without scanning the clip (decision 63).
struct RoiState {
  bool valid = false;    // a state exists and matches the current ROI settings
  bool active = false;   // the source alpha was a matte: the model saw `box` only
  RoiBox box{0, 0, 0, 0};
  bool mask = false;     // Black Out Outside ROI was on
  std::string settings;  // roiSettings() at that time; a mismatch means "reprocess"
};

std::string formatRoiState(const RoiState& r) {
  char b[160];
  std::snprintf(b, sizeof b, "%d,%d,%d,%d,%d,%d;%s", int(r.active), r.box.x, r.box.y, r.box.w,
                r.box.h, int(r.mask), r.settings.c_str());
  return b;
}

RoiState parseRoiState(const std::string& v) {
  RoiState r;
  int a = 0, k = 0;
  char set[64] = {0};
  if (std::sscanf(v.c_str(), "%d,%d,%d,%d,%d,%d;%63s", &a, &r.box.x, &r.box.y, &r.box.w, &r.box.h,
                  &k, set) == 7) {
    r.valid = true;
    r.active = a != 0;
    r.mask = k != 0;
    r.settings = set;
  }
  return r;
}

// True if the image has an alpha channel with any value below 0.99 (a matte, not just opaque).
bool alphaVaries(const OFX::Image& img) {
  if (img.getPixelComponents() != OFX::ePixelComponentRGBA ||
      img.getPixelDepth() != OFX::eBitDepthFloat)
    return false;
  const OfxRectI b = img.getBounds();
  for (int y = b.y1; y < b.y2; ++y) {
    const auto* p = static_cast<const float*>(img.getPixelAddress(b.x1, y));
    for (int x = 0; x < b.x2 - b.x1; ++x)
      if (p[size_t(x) * 4 + 3] < 0.99f) return true;
  }
  return false;
}

int componentsOf(const OFX::Image& img) {
  switch (img.getPixelComponents()) {
    case OFX::ePixelComponentRGBA: return 4;
    case OFX::ePixelComponentRGB: return 3;
    default: return 1;
  }
}

// CVT port (cvt1.07): Fusion (and Resolve's Fusion page) trims each image to its Domain of
// Definition: after a Crop, a Magic Mask or a garbage matte the image is smaller than the
// format, and its size changes from frame to frame ("Source frames differ in size", Fusion
// Studio: 874x603 at one frame). Process works on one fixed canvas, the union of every frame's
// region of definition; each frame is placed on it at its own position, transparent black
// around it. The canvas is saved (hidden canvasState) so render places the matte the same way.
struct Canvas {
  int x = 0, y = 0, w = 0, h = 0;  // OFX pixel coordinates, bottom-left origin
  bool valid() const { return w > 0 && h > 0; }
};

std::string formatCanvas(const Canvas& c) {
  return std::to_string(c.x) + "," + std::to_string(c.y) + "," + std::to_string(c.w) + "," + std::to_string(c.h);
}

Canvas parseCanvas(const std::string& v) {
  Canvas c;
  if (std::sscanf(v.c_str(), "%d,%d,%d,%d", &c.x, &c.y, &c.w, &c.h) != 4) c = Canvas{};
  return c;
}

Canvas canvasOf(const OfxRectI& b) {
  Canvas c;
  c.x = b.x1, c.y = b.y1, c.w = b.x2 - b.x1, c.h = b.y2 - b.y1;
  return c;
}

// The image on the canvas: c.w * c.h RGBA floats, rows top-down; alpha 1 where the image has
// none, 0 (and black) outside the image. Float RGB/RGBA images only.
void placeOnCanvas(const OFX::Image& img, const Canvas& c, std::vector<float>& buf) {
  buf.assign(size_t(c.w) * c.h * 4, 0.0f);
  const OfxRectI b = img.getBounds();
  const int comps = componentsOf(img);
  const int x0 = std::max(b.x1, c.x), x1 = std::min(b.x2, c.x + c.w);
  if (x0 >= x1 || comps < 3) return;
  for (int y = std::max(b.y1, c.y); y < std::min(b.y2, c.y + c.h); ++y) {
    const auto* p = static_cast<const float*>(img.getPixelAddress(x0, y));
    float* q = buf.data() + (size_t(c.y + c.h - 1 - y) * c.w + (x0 - c.x)) * 4;
    for (int x = x0; x < x1; ++x, p += comps, q += 4) {
      q[0] = p[0], q[1] = p[1], q[2] = p[2];
      q[3] = comps == 4 ? p[3] : 1.0f;
    }
  }
}

std::atomic<unsigned> gJobSeq{0};

class AlphaGenEffect : public OFX::ImageEffect {
 public:
  explicit AlphaGenEffect(OfxImageEffectHandle handle) : ImageEffect(handle) {
    src_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
    dst_ = fetchClip(kOfxImageEffectOutputClipName);
    chunkSize_ = fetchChoiceParam(kPChunkSize);
    frameHold_ = fetchChoiceParam(kPFrameHold);
    merge_ = fetchChoiceParam(kPMerge);
    roiMargin_ = fetchIntParam(kPRoiMargin);
    roiMask_ = fetchBooleanParam(kPRoiMask);
    roiLimit_ = fetchBooleanParam(kPRoiLimit);
    roiState_ = fetchStringParam(kPRoiState);
    canvasState_ = fetchStringParam(kPCanvas);
    showSetup_ = fetchBooleanParam(kPShowSetup);
    roiInfo_ = fetchStringParam(kPRoiInfo);
    colourMode_ = fetchChoiceParam(kPColourMode);
    output_ = fetchChoiceParam(kPOutput);
    postMultiply_ = fetchBooleanParam(kPPostMultiply);
    blackClip_ = fetchDoubleParam(kPBlackClip);
    skipMemCheck_ = fetchBooleanParam(kPSkipMemCheck);
    firstFrame_ = fetchIntParam(kPFirstFrame);
    lastFrame_ = fetchIntParam(kPLastFrame);
    whiteClip_ = fetchDoubleParam(kPWhiteClip);
    seams_ = fetchStringParam(kPSeams);
    legacyColour_ = fetchChoiceParam(kPColour);
    status_ = fetchStringParam(kPStatus);
    showMarker_ = fetchBooleanParam(kPShowMarker);
    precision_ = fetchChoiceParam(kPPrecision);
    unload_ = fetchBooleanParam(kPUnload);
    headroom_ = fetchDoubleParam(kPHeadroom);
    accept_ = fetchBooleanParam(kPAccept);
    tokenCaption_ = fetchStringParam(kPTokenCaption);
    token_ = fetchStringParam(kPToken);
    modelDir_ = fetchStringParam(kPModelDir);
    existingDir_ = fetchStringParam(kPExistingDir);
    linkExisting_ = fetchBooleanParam(kPLinkExisting);
    setupStatus_ = fetchStringParam(kPSetupStatus);
    debugLog_ = fetchBooleanParam(kPDebugLog);
    cacheDir_ = fetchStringParam(kPCacheDir);
    cacheInfo_ = fetchStringParam(kPCacheInfo);
    cacheLimit_ = fetchDoubleParam(kPCacheLimit);
    simSeconds_ = fetchDoubleParam(kPSimSeconds);
    simulate_ = fetchChoiceParam(kPSimulate);
    simNeed_ = fetchDoubleParam(kPSimNeed);
    python_ = fetchStringParam(kPPython);
    enforceGate_ = fetchBooleanParam(kPEnforceGate);
    autoRefresh_ = fetchBooleanParam(kPAutoRefresh);
    useDummy_ = fetchBooleanParam(kPUseDummy);
    initialized_ = fetchBooleanParam(kPInitialized);
    nodeIdParam_ = fetchStringParam(kPNodeId);
    refreshTick_ = fetchIntParam(kPRefreshTick);

    try {
      nodeIdParam_->getValue(nodeId_);
      if (nodeId_.empty()) {
        nodeId_ = randomId();  // saved with the setup; copies of a node share it
        nodeIdParam_->setValue(nodeId_);
      }
      LogScope scope(nodeId_);
      bool init = false;
      initialized_->getValue(init);
      LOGI("createInstance %p: initialized=%d context=%d", static_cast<void*>(this), int(init),
           int(getContext()));
      if (!init) {
        // New node (decision 21): take the current user defaults; a node loaded from a
        // saved setup keeps its own values because it carries initialized=true.
        const Defaults d = computeDefaults();
        precision_->setValue(d.precision);
        headroom_->setValue(d.headroomGB);
        unload_->setValue(d.unload);
        // Input Colour: new nodes start on Auto. The param's default is Bypass so that a setup
        // saved before it existed loads as Bypass (the old default); see below for the others.
        // CVT Resolve port: Resolve has no OpenFX colour management, so Auto could only fall
        // back to no conversion; new nodes there stay on Bypass (the frames as the timeline
        // delivers them, which the model expects display-referred).
        const bool resolve = hostIsBlackmagic();
        colourMode_->setValue(resolve ? kColourBypass : kColourAuto);
        initialized_->setValue(true);
        LOGI("new node: defaults [%s] precision=%s headroom=%.1f unload=%d input colour=%s",
             d.source.c_str(), kPrecisions[d.precision], d.headroomGB, int(d.unload),
             resolve ? "Bypass" : "Auto");
      } else {
        // A setup saved with the old dropdown on a colour space (not Bypass): it wanted a
        // conversion, so it moves to Auto once; the old param is reset so this runs only once.
        int legacy = 0;
        legacyColour_->getValue(legacy);
        if (legacy > 0) {
          colourMode_->setValue(kColourAuto);
          legacyColour_->setValue(0);
          LOGI("input colour: saved setup had the old Input Colour '%s': now Auto",
               kLegacyColourSpaces[std::min(legacy, 5)]);
        }
      }
      // Per-user values are not stored in setups; mirror them from the user config.
      const UserConfig cfg = loadUserConfig();
      accept_->setValue(setupState().licenceAccepted);
      // cvt1.46: someone who set up LTX Inpaint Still first keeps that one folder for both.
      // cvt1.49: a folder that was set up and has since been deleted is left over, not a choice. A folder on
      // a drive that's away may come back, and one typed in but not made yet is made by Setup.
      struct stat modelStat {};
      const std::string ownModels = cfg.count("model_dir") ? cfg.at("model_dir") : "";
      const bool leftOver = !ownModels.empty() && cfg.count("models_ready") && stat(ownModels.c_str(), &modelStat) != 0 &&
                            ownModels.rfind("/Volumes/", 0) != 0;
      const std::string otherModels = ownModels.empty() || leftOver ? inpaintStillModelFolder() : "";
      if (!ownModels.empty() && !leftOver) modelDir_->setValue(ownModels);
      else if (!otherModels.empty()) modelDir_->setValue(otherModels);
      else if (leftOver) modelDir_->setValue(homeDir() + "/LTX-2.5");
      if (cfg.count("existing_models")) existingDir_->setValue(cfg.at("existing_models"));
      cacheDir_->setValue(cacheSettings(cfg).root);
      cacheLimit_->setValue(cacheSettings(cfg).capGB);  // cvt1.14: per user, like the folder
      debugLog_->setValue(logLevel() == LogLevel::Debug);
      refreshSetupUi();
      refreshCacheInfo();
    } catch (const std::exception& e) {
      LOGE("createInstance: setting param values FAILED: %s [%s]", e.what(), typeid(e).name());
    }
  }

  ~AlphaGenEffect() override {
    LogScope scope(nodeId_);
    stopWatcher();
    stopSetupWatcher();
    stopChoosingFolder();
    LOGI("destroyInstance %p", static_cast<void*>(this));
  }

  void getClipPreferences(OFX::ClipPreferencesSetter& prefs) override {
    // Decision 43 asked for a matte-only output (Flame ignores it, decision 48). CVT Resolve
    // port: the Output choice needs RGB (Source + Matte Alpha keeps the picture), so RGBA.
    prefs.setClipComponents(*dst_, OFX::ePixelComponentRGBA);
    // cvt1.10: tell the host whether RGB is premultiplied by the alpha (Post-Multiply Image, or
    // Output = Matte, whose RGB is the matte), so Fusion's Merge treats the edges right.
    const bool premult = outputMode() == kOutMatte || (outputMode() == kOutSource && postMultiply());
    prefs.setOutputPremultiplication(premult ? OFX::eImagePreMultiplied : OFX::eImageUnPreMultiplied);
    LOGD("getClipPreferences: output RGBA, %s", premult ? "premultiplied" : "straight");
  }

  bool postMultiply() {
    bool v = true;
    postMultiply_->getValue(v);
    return v;
  }

  int outputMode() {
    int m = kOutSource;
    output_->getValue(m);
    return (m == kOutMatte || m == kOutOverlay) ? m : kOutSource;
  }

  int colourMode() {
    int m = kColourAuto;
    colourMode_->getValue(m);
    return m == kColourBypass ? kColourBypass : kColourAuto;
  }

  // The colourspace the host delivers the Source clip in (OpenFX 1.5 kOfxImageClipPropColourspace,
  // ofxColour.h); "" from a host without colour management.
  std::string deliveredColourspace() {
    return propString(src_->getPropertySet().propSetHandle(), kOfxImageClipPropColourspace);
  }

  // Input Colour (called from the factory's entry after the Support library's GetClipPreferences,
  // whose status is st). Auto requests kAutoColourspace for the Source clip on the out args
  // (OfxImageClipPropPreferredColourspaces_Source, ofxColour.h kOfxImageClipPropPreferredColourspaces;
  // Flame 2027.2 honours it there and refuses it on the clip descriptor: Matte Refine ML,
  // PHASE0_RESULTS.md "Colour probe"). The host reads out args only on kOfxStatOK. Bypass: nothing.
  OfxStatus requestColour(OfxPropertySetHandle out, OfxStatus st) {
    LogScope scope(nodeId_);
    const int mode = colourMode();
    std::string req = "no request";
    if (mode == kColourAuto) {
      const std::string prop = std::string(kOfxImageClipPropPreferredColourspaces) + "_" +
                               kOfxImageEffectSimpleSourceClipName;
      const char* names[] = {kAutoColourspace};
      const OfxStatus s = OFX::Private::gPropSuite->propSetStringN(out, prop.c_str(), 1, names);
      req = std::string("requested '") + kAutoColourspace + "' on " + prop + ": " + OFX::mapStatusToString(s);
      if (st == kOfxStatReplyDefault) st = kOfxStatOK;
    }
    LOGI("colour: clip preferences, Input Colour %s, style '%s', %s; Source clip colourspace now '%s'",
         kColourModeNames[mode],
         getPropertySet().propGetString(kOfxImageEffectPropColourManagementStyle, false).c_str(), req.c_str(),
         deliveredColourspace().c_str());
    return st;
  }

  // GetOutputColourspace (OpenFX 1.5, unknown to the 1.4 Support library): the output is the Source
  // picture with the overlay (decision 48). Auto: display-referred (we convert ACEScg to display before
  // the overlay, so the overlay looks the same on any clip); Bypass: the clip's colourspace is unknown
  // to us, so no picture and no overlay: the matte goes out on both outputs, tagged "Matte" (data). Without an answer Flame mis-tags the Auto picture (looks converted twice;
  // Flame test, 2026-10-27). "" = no answer (host default).
  std::string outputColourspace() {
    LogScope scope(nodeId_);
    if (!src_->isConnected()) return "";
    if (outputMode() == kOutSource) return "";  // CVT Resolve port: the source picture, as delivered
    if (outputMode() == kOutMatte || colourMode() == kColourBypass) {  // the image output carries the matte: a data colourspace
      const std::string style = getPropertySet().propGetString(kOfxImageEffectPropColourManagementStyle, false);
      return style == kOfxImageEffectColourManagementOCIO ? "Matte" : "";  // "Matte": Flame's aces2.0 config
    }
    const std::string cs = kOutputDisplay;
    LOGD("colour: output colourspace %s", cs.c_str());
    return cs;
  }

  void changedParam(const OFX::InstanceChangedArgs& args, const std::string& name) override {
    // Flame reports our own setValue() calls back synchronously as changedParam (reason =
    // plugin edit); reacting to those recursed until Flame crashed (Flame test 2). Only user
    // edits are handled, and never re-entrantly.
    if (args.reason != OFX::eChangeUserEdit || inChangedParam_) return;
    struct Guard {
      bool& b;
      explicit Guard(bool& b) : b(b) { b = true; }
      ~Guard() { b = false; }
    } guard(inChangedParam_);
    LogScope scope(nodeId_);
    LOGI("changedParam %s (user) time=%g", name.c_str(), args.time);
    try {
      handleParam(name, args.time);
    } catch (const std::exception& e) {
      LOGE("changedParam %s FAILED: %s [%s]", name.c_str(), e.what(), typeid(e).name());
      // CVT Resolve port: Resolve showed nothing for the thrown error, so say it in Status.
      if (name == kPProcess) error(std::string("Process failed (") + e.what() + "): see the log.");
      throw;
    }
    refreshSetupUi();
  }

  // cvt1.38: Resolve's Edit and Color pages give the output clip a frame range ([0,10] on an
  // 11-frame clip); the Fusion page and Fusion Studio give none ([0,-1]).
  // Open a web page in the default browser (cvt1.39: shared by the page buttons).
  // The Mac's own folder picker. Fusion's browse control for a folder field only picks files. It runs on its
  // own thread so the host keeps drawing while the window is open, and Cancel changes nothing.
  void chooseFolder(OFX::StringParam* field, const char* prompt, const char* configKey) {
    if (browsing_.exchange(true)) return;
    if (browseThread_.joinable()) browseThread_.join();
    std::string start;
    field->getValue(start);
    struct stat st {};
    if (start.empty() || start[0] != '/' || stat(start.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) start = homeDir();
    const std::string node = nodeId_, title = prompt, key = configKey;
    browseThread_ = std::thread([this, field, node, title, start, key] {
      LogScope scope(node);
      auto proc = std::make_shared<WorkerProcess>();
      {
        std::lock_guard<std::mutex> lock(browseMutex_);
        browseProc_ = proc;
      }
      std::string err;
      std::vector<std::string> lines;
      if (proc->start({"/usr/bin/osascript", "-e", "on run argv", "-e", "activate", "-e",
                       "return POSIX path of (choose folder with prompt (item 1 of argv) default location "
                       "(POSIX file (item 2 of argv) as alias))",
                       "-e", "end run", title, start},
                      cleanEnvironment(), err)) {
        while (proc->readLines(200, lines)) {
        }
        proc->wait();
      } else {
        LOGW("choose folder: %s", err.c_str());
      }
      {
        std::lock_guard<std::mutex> lock(browseMutex_);
        browseProc_.reset();
      }
      std::string chosen = lines.empty() ? "" : lines.front();
      while (chosen.size() > 1 && chosen.back() == '/') chosen.pop_back();
      if (!chosen.empty() && chosen[0] == '/') {
        try {
          field->setValue(chosen);
          if (!key.empty()) {  // a change made here isn't a user edit, so it is saved here
            UserConfig cfg = loadUserConfig();
            cfg[key] = chosen;
            saveUserConfig(cfg);
          }
          refreshSetupUi();
          refreshCacheInfo();
        } catch (const std::exception& e) {
          LOGE("choose folder: %s", e.what());
        }
      }
      browsing_ = false;
    });
  }

  void stopChoosingFolder() {
    {
      std::lock_guard<std::mutex> lock(browseMutex_);
      if (browseProc_) browseProc_->kill();
    }
    if (browseThread_.joinable()) browseThread_.join();
  }

  void openPage(const char* url) {
    std::string err;
    if (launchDetached({"/usr/bin/open", url}, cleanEnvironment(), err))
      LOGI("web page: opened %s", url);
    else
      LOGE("web page: FAILED %s: %s", url, err.c_str());
  }

  bool onEditOrColorPage() {
    if (!hostIsResolve()) return false;
    try {
      const OfxRangeD r = dst_->getFrameRange();
      return r.max >= r.min;
    } catch (const std::exception&) {
      return false;
    }
  }

  // cvt1.33: render()'s interactive flag is 0 for viewer renders too in Fusion, but this action's
  // tells them apart (a final render 1005-1029 interactive=0, the viewer 1021-1021 interactive=1).
  // cvt1.34: Fusion Studio: a final render stops here if a frame has no matte, as it reuses frames
  // the viewer already drew with the marker and would save them, without calling render() again.
  // Not in Resolve: what range a Deliver reports is not known, and a good delivery must not fail.
  void beginSequenceRender(const OFX::BeginSequenceRenderArguments& args) override {
    LogScope scope(nodeId_);
    const bool final = !args.isInteractive;
    if (final != finalRender_.exchange(final))
      LOGI("render: sequence %g-%g, interactive=%d", args.frameRange.min, args.frameRange.max, int(args.isInteractive));
    if (!final || !hostIsFusion() || !processedBefore()) return;
    const int missing = firstFrameWithoutMatte(int(std::lround(args.frameRange.min)), int(std::lround(args.frameRange.max)));
    if (missing == kNoFrame) return;
    LOGE("render: stopped before it began: no matte for frame %d (range %g-%g); press Process", missing,
         args.frameRange.min, args.frameRange.max);
    OFX::throwSuiteStatusException(kOfxStatFailed);
  }

  static constexpr int kNoFrame = -1000000;

  // The first frame of the range, within the source clip, with no matte; kNoFrame if none, or
  // if the node's canvas isn't known (render() still fails such a frame when it is rendered).
  int firstFrameWithoutMatte(int first, int last) {
    std::string v;
    canvasState_->getValue(v);
    const Canvas cv = parseCanvas(v);
    if (!cv.valid()) return kNoFrame;
    const OfxRangeD clip = src_->getFrameRange();
    first = std::max(first, int(std::lround(clip.min)));
    last = std::min(last, int(std::lround(clip.max)));
    const std::string root = cacheRoot();
    const std::string sig = canvasSignature(paramSignature(), cv);
    for (int f = first; f <= last; ++f)
      if (!frameHasMatte(root, nodeId_, f, sig)) return f;
    return kNoFrame;
  }

  void render(const OFX::RenderArguments& args) override {
    LogScope scope(nodeId_);
    LOGD("render t=%g interactive=%d sequential=%d window=(%d,%d)-(%d,%d)", args.time,
         int(args.interactiveRenderStatus), int(args.sequentialRenderStatus),
         args.renderWindow.x1, args.renderWindow.y1, args.renderWindow.x2, args.renderWindow.y2);
    std::unique_ptr<OFX::Image> out(dst_->fetchImage(args.time));
    if (!out) {
      LOGE("render t=%g: host returned no output image", args.time);
      OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    std::vector<float> matte;
    std::string message, caption;
    bool passThrough = false;  // never processed: source + white matte (decision 71)
    bool failRender = false;   // processed, but no matte: loud marker, then fail the render
    const OfxRectI db = out->getBounds();
    OfxRectI sb = db;
    Canvas cv;  // where the matte sits (cvt1.07)
    std::unique_ptr<OFX::Image> src;
    const bool setupRequired = gateClosed();
    if (gateWasClosed_ && !setupRequired) {  // Setup finished in the background since the last look
      LOGI("render: setup is now complete; enabling the node's params");
      refreshSetupUi();
    }
    gateWasClosed_ = setupRequired;

    if (setupRequired) {
      message = "Setup required: see the Licence and Models settings (Setup).";
      caption = "SETUP REQUIRED";
    } else {
      src.reset(src_->fetchImage(args.time));
      if (!src) {
        message = "No source image (is the input connected?).";
        caption = "NOT CONNECTED";
      } else if (src->getPixelDepth() != OFX::eBitDepthFloat) {
        message = "Only 32-bit float images are supported.";
        caption = "NOT SUPPORTED";
      } else {
        sb = src->getBounds();
        cv = renderCanvas(sb);
        const int w = cv.w, h = cv.h;
        const std::string cs = deliveredColourspace();
        const std::string outCs = propString(dst_->getPropertySet().propSetHandle(), kOfxImageClipPropColourspace);
        if (!loggedDeliveredCs_ || cs != lastDeliveredCs_ || outCs != lastOutputCs_) {  // what Flame did
          LOGI("colour: render: Input Colour %s, Source delivered in '%s', output clip tagged '%s'",
               kColourModeNames[colourMode()], cs.c_str(), outCs.c_str());
          lastDeliveredCs_ = cs;
          lastOutputCs_ = outCs;
          loggedDeliveredCs_ = true;
        }
        // cvt1.18: the matte of this node and frame whose source thumbnail matches the picture
        // (lookupMatte); a node processed before cvt1.18 has no thumbnails: the old pixel key.
        const std::string sig = paramSignature(nullptr, src.get());
        const std::string root = cacheRoot();
        bool pictureChanged = false;
        std::string key = lookupMatte(root, int(std::lround(args.time)), canvasSignature(sig, cv), *src, cv,
                                      pictureChanged);
        if (key.empty()) key = matteKey(hashImage(*src), sig);
        std::vector<uint16_t> half;
        if (!pictureChanged && readMatte(root, key, w, h, half)) {
          matte.resize(half.size());
          halfToFloat(half.data(), matte.data(), matte.size());
          applyAlphaClip(matte);  // cvt1.26, before the ROI limit so the input alpha's edges stay soft
          applyRoiLimit(*src, cv, matte);
        } else if (!pictureChanged && isPending(root, key)) {
          message = "Processing: this frame is not ready yet.";
          caption = "MAKING MAGIC";  // cvt1.21 (was PROCESSING)
        } else if (!processedBefore()) {
          message = "Not processed: press Process.";
          caption = "NOT PROCESSED";
          passThrough = true;
        } else {
          // Decision 71: this node had mattes, so a miss must not pass silently through a
          // big setup (cache purged or swept, another machine, source or settings changed).
          failRender = true;
          const std::string status = lastJobStatus();
          const std::string state = jsonField(status, "state");
          if (state == "failed") {
            message = "Processing failed: " + jsonField(status, "message") +
                      ". Press Process to try again.";
            caption = "FAILED";
          } else if (state == "cancelled") {
            message = "Processing was cancelled. Press Process to try again.";
            caption = "CANCELLED";
          } else if (pictureChanged) {  // cvt1.18: thumbnails exist, none matches the picture
            message = "The picture coming in has changed since Process (a change upstream of the "
                      "node, or another clip). Press Process.";
            caption = "PICTURE CHANGED";  // cvt1.33
          } else {
            message = "Matte missing: no cached matte for this frame (cache purged, another "
                      "machine, or the source or settings changed since Process). Press Process.";
            caption = "MATTE MISSING";
          }
        }
        LOGD("render t=%g key=%s %s", args.time, key.c_str(),
             matte.empty() ? caption.c_str() : "cache hit");
      }
    }

    if (!cv.valid()) cv = canvasOf(sb);  // no source: the output's own area
    const int w = cv.w, h = cv.h;  // the matte's size: the canvas (cvt1.07)
    const bool realMatte = !matte.empty();  // before drawMarker() fills it with the marker
    if (passThrough) {
      matte.assign(size_t(w) * h, 1.0f);
    } else if (matte.empty()) {
      bool marker = true;
      showMarker_->getValue(marker);
      if (marker || failRender) drawMarker(matte, w, h, caption);
    }
    const bool loud = setupRequired || failRender;  // the marker on the image output too
    // Flame always gives an RGBA output and shows it as an image output plus a matte output
    // (decision 43 attempt failed). Decision 48: the alpha (Flame's matte output) carries the
    // matte or marker. Decision 58: the image is the source with the matte overlaid in magenta
    // at 30% (overlayPixel, marker.h); frames without a matte show the plain source, except
    // SETUP REQUIRED, which shows the marker on the image too (decision 59: a node viewed on its
    // image output must not look like a working pass-through), and a missing matte on a
    // processed node (decision 71). A 1-channel output gets the matte.
    const bool rgbFromSource = src && src->getPixelDepth() == OFX::eBitDepthFloat;
    const OfxRectI& win = args.renderWindow;
    const int comps = dst_->getPixelComponents() == OFX::ePixelComponentRGBA  ? 4
                      : dst_->getPixelComponents() == OFX::ePixelComponentRGB ? 3
                                                                               : 1;
    if (!loggedOutput_) {
      LOGI("render: output components %s (%d channel(s))",
           dst_->getPixelComponentsProperty().c_str(), comps);
      loggedOutput_ = true;
    }
    // CVT Resolve port: the Output choice replaces decision 76's "Bypass: matte on both outputs"
    // (in Resolve the source is in the timeline's colour space, so the overlay is fine there).
    const int outMode = outputMode();
    const bool displayOut = colourMode() == kColourAuto && deliveredColourspace() == kAutoColourspace;
    // cvt1.10: Post-Multiply Image, only with a real matte (a marker or the not-processed white
    // matte leaves the picture as it is).
    const bool premultiply = outMode == kOutSource && realMatte && postMultiply();
    for (int y = std::max(win.y1, db.y1); y < std::min(win.y2, db.y2); ++y) {
      auto* px = static_cast<float*>(out->getPixelAddress(std::max(win.x1, db.x1), y));
      const int row = (h - 1) - (y - cv.y);  // OFX is bottom-up, the matte top-down
      const bool rowInSrc = y >= sb.y1 && y < sb.y2;
      for (int x = std::max(win.x1, db.x1); x < std::min(win.x2, db.x2); ++x) {
        const int col = x - cv.x;
        const bool inside = row >= 0 && row < h && col >= 0 && col < w;
        const float v = (!matte.empty() && inside) ? matte[size_t(row) * w + col] : 0.0f;
        if (comps == 1) {
          *px++ = v;
          continue;
        }
        const float* sp = (rgbFromSource && rowInSrc && x >= sb.x1 && x < sb.x2)
                              ? static_cast<const float*>(src->getPixelAddress(x, y))
                              : nullptr;
        if (loud || outMode == kOutMatte) {
          px[0] = px[1] = px[2] = v;  // the marker, grey on black; Matte: the matte on both outputs
        } else if (outMode == kOutSource) {
          const float k = premultiply ? v : 1.0f;  // cvt1.10: Post-Multiply Image
          for (int c = 0; c < 3; ++c) px[c] = sp ? sp[c] * k : 0.0f;  // the picture (times the matte)
        } else {
          static const float kBlack[3] = {0.0f, 0.0f, 0.0f};
          float disp[3];
          if (sp && displayOut) {  // Auto: ACEScg to display first, then the overlay (decision 76)
            acescgToDisplay(sp, disp);
            sp = disp;
          }
          overlayPixel(sp ? sp : kBlack, realMatte ? v : 0.0f, px);
          if (!realMatte && !passThrough && v > 0.0f)  // cvt1.23: the marker (MAKING MAGIC) in white
            for (int c = 0; c < 3; ++c) px[c] = px[c] * (1.0f - v) + v;
        }
        px += 3;
        // cvt1.23: Overlay is a preview, so its alpha is solid and the whole picture shows (it
        // used to carry the matte, which cut the background away in the viewer).
        if (comps == 4) *px++ = (outMode == kOutOverlay && !loud) ? 1.0f : v;
      }
    }
    try {
      if (hostIsBlackmagic()) {
        // cvt1.33/1.36: Fusion and Resolve refuse persistent messages (setting and clearing), which
        // logged a warning on every frame; the marker says it instead.
      } else if (caption.empty())
        clearPersistentMessage();
      else
        setPersistentMessage(failRender ? OFX::Message::eMessageError : OFX::Message::eMessageWarning,
                             "", message);
    } catch (const std::exception& e) {
      LOGW("render: persistent message FAILED: %s", e.what());
    }
    // cvt1.33: Fusion and Resolve throw a failed render's picture away (red node, frozen viewer),
    // so the viewer gets the marker; a final render still fails (decision 71): Fusion stops it.
    // cvt1.38: on the Edit and Color pages a failure is silent: the viewer shows, and a Deliver
    // reports Complete with, the plain footage. So there the marker always goes out.
    if (failRender && hostIsBlackmagic() && (!finalRender_ || onEditOrColorPage())) {
      LOGD("render t=%g: %s, shown in the viewer", args.time, caption.c_str());
    } else if (failRender) {  // after filling the output, in case the host shows it anyway
      LOGD("render t=%g: %s, failing the render (decision 71)", args.time, caption.c_str());
      OFX::throwSuiteStatusException(kOfxStatFailed);
    }
  }

 private:
  // cvt1.26: Alpha Black Clip / Alpha White Clip on a real matte (not on markers or the
  // not-processed white matte). Render-time only, so it changes without reprocessing.
  void applyAlphaClip(std::vector<float>& matte) {
    double black = 0.0, white = 1.0;
    blackClip_->getValue(black);
    whiteClip_->getValue(white);
    clipMatte(matte, float(black), float(white));
  }

  // Multiply Result by ROI (decision 63): the cached matte times this frame's source alpha.
  // Render-time only, so it can be toggled without reprocessing.
  void applyRoiLimit(const OFX::Image& src, const Canvas& cv, std::vector<float>& matte) {
    bool limit = false;
    roiLimit_->getValue(limit);
    if (!limit) return;
    const RoiState st = currentRoiState();
    if (!st.valid || !st.active) return;
    std::vector<float> buf;  // the source on the canvas: alpha 0 outside the image (cvt1.07)
    placeOnCanvas(src, cv, buf);
    for (size_t i = 0; i < matte.size(); ++i) matte[i] *= std::min(1.0f, std::max(0.0f, buf[i * 4 + 3]));
  }

  // cvt1.20: the overlaps of neighbouring chunks in comp frames, e.g. "1019-1033, 1037-1051",
  // or "none (one chunk)".
  static std::string seamsText(const std::vector<Chunk>& chunks, int first) {
    if (chunks.size() < 2) return "none (one chunk)";
    std::string out;
    for (size_t i = 1; i < chunks.size(); ++i) {
      const int a = first + chunks[i].start, b = first + std::min(chunks[i - 1].end(), chunks[i].end());
      if (b < a) continue;  // chunks that only touch: no overlap
      out += (out.empty() ? "" : ", ") + std::to_string(a) + "-" + std::to_string(b);
    }
    return out.empty() ? "none" : out;
  }

  // cvt1.18: the settings part of a frame key, with the canvas (a different canvas is a
  // different matte layout).
  static std::string canvasSignature(const std::string& sig, const Canvas& cv) {
    return sig + "|c=" + formatCanvas(cv);
  }

  // cvt1.18: the key of this node's matte for `frame` whose source thumbnail matches `src` (on
  // the canvas), or "". `changed` = thumbnails exist for the frame but none matches: the picture
  // coming in is not the one processed.
  std::string lookupMatte(const std::string& root, int frame, const std::string& sigC, const OFX::Image& src,
                          const Canvas& cv, bool& changed) {
    changed = false;
    std::vector<float> buf, now, then;
    placeOnCanvas(src, cv, buf);
    thumbnailOf(buf.data(), cv.w, cv.h, now);
    bool any = false;
    double best = 1e9;
    for (int s = 0; s < kThumbSlots; ++s) {
      const std::string k = frameKey(nodeId_, frame, sigC, s);
      if (!readThumb(root, k, then)) continue;
      any = true;
      double mean = 0.0, worst = 0.0;
      if (thumbsMatch(now, then, &mean, &worst)) {
        LOGD("render frame %d: slot %d matches (mean %.4f, max %.3f)", frame, s, mean, worst);
        return k;
      }
      best = std::min(best, mean);
    }
    changed = any;
    if (any) LOGD("render frame %d: no slot matches the picture (best mean %.4f)", frame, best);
    return "";
  }

  // cvt1.18: Process: the key for `frame` (writes its thumbnail). The slot whose thumbnail matches
  // this picture (processing the same clip again), else a free slot, else slot 0. Taking over a
  // slot of another picture deletes its matte first, so a render never shows that old matte
  // under the new thumbnail before the job has written the new one.
  std::string takeSlot(const std::string& root, int frame, const std::string& sigC, const std::vector<float>& thumb) {
    int match = -1, freeSlot = -1;
    std::vector<float> then;
    for (int s = 0; s < kThumbSlots && match < 0; ++s) {
      if (readThumb(root, frameKey(nodeId_, frame, sigC, s), then)) {
        if (thumbsMatch(thumb, then)) match = s;
      } else if (freeSlot < 0) {
        freeSlot = s;
      }
    }
    const int slot = match >= 0 ? match : freeSlot >= 0 ? freeSlot : 0;
    const std::string k = frameKey(nodeId_, frame, sigC, slot);
    if (match < 0) unlink(mattePath(root, k).c_str());
    if (!writeThumb(root, k, thumb)) LOGW("process: could not write the thumbnail for frame %d", frame);
    return k;
  }

  // The canvas render places the matte on: the one the last Process used (cvt1.07), or the
  // source's own bounds for a node processed before that (or never).
  Canvas renderCanvas(const OfxRectI& sb) {
    std::string v;
    canvasState_->getValue(v);
    const Canvas c = parseCanvas(v);
    return c.valid() ? c : canvasOf(sb);
  }

  // Scans the source's alpha over the whole clip (main thread, like the staging fetch). If the
  // alpha is opaque in every frame there is no ROI and the model sees the whole frame; else the
  // crop box is the union of the matte, grown by the margin (roi.h).
  // CVT Resolve port: reading the clip blocks the host's UI thread (Resolve shows a beach ball),
  // so show the host's progress bar where it has one, honour its Cancel, and log the pace.
  // `fraction` is 0-1 over the whole read; returns false when the user cancelled.
  bool stagingProgress(double fraction, int f, int n, const char* what) {
    if (f == 0) stagingT0_ = std::chrono::steady_clock::now();
    if (f > 0 && (f % 25 == 0)) {
      const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - stagingT0_).count();
      LOGI("process: %s %d/%d frames, %.0f ms/frame", what, f, n, 1000.0 * sec / f);
    }
    try {
      return progressUpdate(std::min(1.0, std::max(0.0, fraction)));
    } catch (const std::exception&) {
      return true;
    }
  }

  bool computeRoi(int first, int n, const Canvas& cv, RoiState& out, std::string& err) {
    const int W = cv.w, H = cv.h;
    int margin = 64;
    roiMargin_->getValue(margin);
    roiMask_->getValue(out.mask);
    out.settings = roiSettings();
    out.valid = true;
    out.box = {0, 0, W, H};
    // CVT Resolve port: every fetch makes Fusion render the frame on the UI thread, so the full
    // scan (a second pass over the clip) runs only if one of 5 frames spread over the clip has a
    // varying alpha. A Fusion MediaIn's alpha is always opaque, so that is the usual answer.
    // Assumption: an alpha that is opaque in all 5 is opaque throughout (else no ROI is used).
    bool varies = false;
    for (int i = 0; i < 5 && !varies; ++i) {
      const int f = n <= 1 ? 0 : int(std::lround(double(i) * (n - 1) / 4.0));
      std::unique_ptr<OFX::Image> img;
      try {
        img.reset(src_->fetchImage(double(first + f)));
      } catch (const std::exception& e) {
        LOGE("process: fetchImage(%d) threw: %s", first + f, e.what());
      }
      if (!img) {
        err = hostShort() + " did not provide source frame " + std::to_string(first + f) + ".";
        return false;
      }
      // An image smaller than the canvas is transparent around it: an alpha that varies.
      const OfxRectI b = img->getBounds();
      varies = alphaVaries(*img) || b.x2 - b.x1 < W || b.y2 - b.y1 < H;
    }
    if (!varies) {
      LOGI("process: source alpha is opaque in 5 sampled frames of %d: no ROI, whole frame", n);
      return true;
    }
    LOGI("process: source alpha varies: scanning all %d frames for the ROI", n);
    RoiScan scan;
    std::vector<float> buf;
    for (int f = 0; f < n; ++f) {
      if (!stagingProgress(0.5 * f / n, f, n, "ROI scan")) {
        err = "Cancelled.";
        return false;
      }
      std::unique_ptr<OFX::Image> img;
      try {
        img.reset(src_->fetchImage(double(first + f)));
      } catch (const std::exception& e) {
        LOGE("process: fetchImage(%d) threw: %s", first + f, e.what());
      }
      if (!img) {
        err = hostShort() + " did not provide source frame " + std::to_string(first + f) + ".";
        return false;
      }
      if (img->getPixelDepth() != OFX::eBitDepthFloat) {
        err = "Only 32-bit float images are supported.";
        return false;
      }
      placeOnCanvas(*img, cv, buf);  // cvt1.07: frames may differ in size (Crop, Magic Mask)
      for (int row = 0; row < H; ++row) scan.addRow(buf.data() + size_t(row) * W * 4, 4, W, row);
    }
    if (!scan.alphaVaries) {
      LOGI("process: source alpha is opaque in all %d frames: no ROI, whole frame", n);
      return true;
    }
    if (!scan.alpha.any()) {
      err = std::string("The source alpha is empty (nothing above ") +
            std::to_string(kRoiThreshold).substr(0, 4) + " in any frame), so there is no ROI.";
      return false;
    }
    out.active = true;
    out.box = expandRoi(scan.alpha, margin, W, H);
    LOGI("process: ROI from source alpha: bounds x %d-%d y %d-%d (union of %d frames), margin %d -> crop "
         "x %d y %d %dx%d of %dx%d, black out outside %d",
         scan.alpha.x0, scan.alpha.x1, scan.alpha.y0, scan.alpha.y1, n, margin, out.box.x,
         out.box.y, out.box.w, out.box.h, W, H, int(out.mask));
    return true;
  }

  // CVT Resolve port: always a new value (the watcher's own count restarted at 1 for every job,
  // so a second job set the same value and the host saw no change).
  void bumpRefreshTick() {
    int v = 0;
    refreshTick_->getValue(v);
    refreshTick_->setValue(v + 1);
  }

  void handleParam(const std::string& name, double time) {
    if (name == kPProcess) {
      if (jobActive()) {  // cvt1.33: a double click ran the job twice
        LOGI("process: ignored, this node already has a job starting or running");
        return;
      }
      process(time);
    } else if (name == kPRefresh) {
      // CVT Resolve port: a param change makes the host drop its cached frames of this node and
      // render again, picking up mattes a job finished since (if the watcher's nudge was missed).
      bumpRefreshTick();
      LOGI("refresh: viewer refresh requested");
    } else if (name == kPWebsite) {
      openPage(kWebsiteUrl);
    } else if (name == kPModelPage) {
      openPage(kModelPageUrl);
    } else if (name == kPLoraPage) {
      openPage(kLoraPageUrl);
    } else if (name == kPTokenPage) {
      openPage(kTokenPageUrl);
    } else if (name == kPPurgeNode) {
      purgeNode();
    } else if (name == kPCreditPort) {
      resetNode();
    } else if (name == kPSaveDefault) {
      saveAsDefault();
    } else if (name == kPViewLicence) {
      std::string err;
      const bool ok = showTextFile("LTX-2.x Licence", licencePath(), cleanEnvironment(), err);
      if (ok)
        LOGI("viewLicence: window opened");
      else
        LOGE("viewLicence: FAILED %s", err.c_str());
    } else if (name == kPAccept) {
      bool v = false;
      accept_->getValue(v);
      UserConfig cfg = loadUserConfig();
      if (v) {
        cfg["licence_accepted"] = timeStr("%Y-%m-%d");
        cfg["licence_hash"] = licenceHash();
      } else {
        cfg.erase("licence_accepted");
        cfg.erase("licence_hash");
      }
      saveUserConfig(cfg);
      LOGI("licence %s (hash %s)", v ? "accepted" : "withdrawn", licenceHash().c_str());
    } else if (name == kPChooseExisting) {
      chooseFolder(existingDir_, "Choose a folder that has the model files", "existing_models");
    } else if (name == kPChooseModel) {
      chooseFolder(modelDir_, "Choose the folder for the LTX-2.5 models", "model_dir");
    } else if (name == kPChooseCache) {
      chooseFolder(cacheDir_, "Choose the cache folder", "cache_dir");
    } else if (name == kPModelDir || name == kPCacheDir || name == kPExistingDir) {
      std::string v;
      (name == kPModelDir ? modelDir_ : name == kPCacheDir ? cacheDir_ : existingDir_)->getValue(v);
      v = trimSlashes(v);
      UserConfig cfg = loadUserConfig();
      cfg[name == kPModelDir ? "model_dir" : name == kPCacheDir ? "cache_dir" : "existing_models"] = v;
      saveUserConfig(cfg);
      LOGI("%s set to %s", name.c_str(), v.c_str());
      refreshCacheInfo();
    } else if (name == kPCacheLimit) {
      // cvt1.14: the cache's size limit, per user (cache_cap_gb). Applied now, so a lower limit
      // frees space at once (least recently used mattes first); a higher one just lets it grow.
      double gb = 50.0;
      cacheLimit_->getValue(gb);
      UserConfig cfg = loadUserConfig();
      char v[32];
      std::snprintf(v, sizeof v, "%g", gb);
      cfg["cache_cap_gb"] = v;
      saveUserConfig(cfg);
      const SweepResult sw = sweepCache(cacheSettings(cfg));
      LOGI("cache limit set to %s GB: %zu matte(s) removed, %.1f MB freed", v, sw.capped + sw.aged,
           sw.bytesFreed / 1048576.0);
      refreshCacheInfo();
    } else if (name == kPDebugLog) {
      bool v = false;
      debugLog_->getValue(v);
      UserConfig cfg = loadUserConfig();
      cfg["log_level"] = v ? "debug" : "info";
      saveUserConfig(cfg);
      setLogLevel(v ? LogLevel::Debug : LogLevel::Info);
      LOGI("debug logging %s", v ? "on" : "off");
    } else if (name == kPCollectLogs) {
      collectLogs();
    } else if (name == kPDownload) {
      download();
    } else if (name == kPPurgeAll) {
      const std::string root = cacheRoot();
      const uint64_t freed = purgeAll(root);
      LOGI("purgeAll %s: %.1f MB freed", root.c_str(), freed / 1048576.0);
      refreshCacheInfo();
    }
  }

  bool gateClosed() {
    bool enforce = true;
#if LTXAG_DEV_BUILD
    enforceGate_->getValue(enforce);
#endif
    return enforce && !setupState().ready();
  }

  // Disable everything outside Licence/Models/Cache while setup is incomplete (decision 38).
  void refreshSetupUi() {
    const SetupState st = setupState();
    const bool closed = gateClosed();
    for (const char* p : {kPColourMode, kPOutput, kPPostMultiply, kPBlackClip, kPWhiteClip, kPRefresh, kPProcess, kPShowMarker, kPPurgeNode, kPPrecision, kPUnload,
                          kPHeadroom, kPSaveDefault, kPChunkSize, kPMerge, kPSkipMemCheck, kPFirstFrame, kPLastFrame,
                          kPRoiMargin, kPRoiMask, kPRoiLimit}) {
      try {
        getParam(p)->setEnabled(!closed);
      } catch (const std::exception& e) {
        LOGW("setEnabled(%s) FAILED: %s", p, e.what());
      }
    }
    // cvt1.09: in Resolve and Fusion, the controls only needed for the download are hidden once
    // Setup is complete (licence accepted, models ready), and come back if that changes.
    // cvt1.10: or when Show Setup Options is ticked (re-download, other models, the licence).
    bool showSetup = false;
    showSetup_->getValue(showSetup);
    const bool hide = st.ready() && !showSetup;
    if (hostIsBlackmagic() && hide != setupHidden_) {
      for (const char* p : {kPViewLicence, kPAccept, kPModelPage, kPLoraPage, kPTokenPage, kPToken,
                            kPDownload, kPExistingDir, kPLinkExisting, kPChooseExisting}) {
        try {
          getParam(p)->setIsSecret(hide);
        } catch (const std::exception& e) {
          LOGW("setIsSecret(%s) FAILED: %s", p, e.what());
        }
      }
      setupHidden_ = hide;
      LOGI("setup UI: download controls %s", hide ? "hidden (setup complete)" : "shown");
    }
    if (hostIsBlackmagic()) {  // the checkbox itself only matters once Setup is complete
      try {
        showSetup_->setIsSecret(!st.ready());
      } catch (const std::exception& e) {
        LOGW("setIsSecret(showSetup) FAILED: %s", e.what());
      }
    }
    // <= 28 characters: Flame cuts text at ~30 (test 2 screenshots).
    setIfChanged(setupStatus_, std::string("Licence: ") + (st.licenceAccepted ? "OK" : "NO") +
                                   " | Models: " + (st.modelsReady ? "OK" : "NO"));
  }

  // Avoids needless host notifications (each setValue is reported back as a change).
  void setIfChanged(OFX::StringParam* p, const std::string& v) {
    try {
      std::string cur;
      p->getValue(cur);
      if (cur != v) p->setValue(v);
    } catch (const std::exception& e) {
      LOGW("setValue FAILED: %s", e.what());
    }
  }

  std::string cacheRoot() { return cacheSettings(loadUserConfig()).root; }

  void refreshCacheInfo() {
    // cvt1.14: with the limit, e.g. "0.6 of 50 GB" (fmtGB keeps one decimal).
    const CacheSettings s = cacheSettings(loadUserConfig());
    char cap[32];
    std::snprintf(cap, sizeof cap, "%g", s.capGB);
    setIfChanged(cacheInfo_, fmtGB(cacheBytes(s.root) / 1073741824.0) + " of " + cap + " GB");
  }

  bool useDummy() {
    bool dummy = false;
#if LTXAG_DEV_BUILD
    useDummy_->getValue(dummy);
#endif
    return dummy;
  }

  int frameHoldIndex() {
    int idx = 0;
    frameHold_->getValue(idx);
    return idx;
  }

  int frameHold() {
    const int idx = frameHoldIndex();
    return kFrameHolds[std::min(std::max(idx, 0), kFrameHoldChoices - 1)];
  }

  // Decision 63: the ROI box and Black Out Outside ROI change the matte, so they are part of the
  // key (two nodes on one source with different ROIs must not share mattes). Without an ROI (an
  // opaque alpha) the key is the old one, so earlier caches stay valid. Chunk Size and Merge are
  // not in it: Process overwrites the whole clip anyway. With no saved state that matches the
  // current ROI settings, a frame whose alpha is a matte gets "roi=unprocessed" (never a hit).
  // `img` is the frame being keyed; the state is decided by the last Process.
  // Input Colour: Bypass keeps the old key ("cs=Bypass"); Auto keys on the colourspace the host
  // delivered ("cs=Auto:ACEScg", "cs=Auto:" without colour management), which decides the
  // worker's conversion.
  std::string colourKey() {
    return colourMode() == kColourBypass ? std::string("Bypass") : "Auto:" + deliveredColourspace();
  }

  std::string paramSignature(const RoiState* roiOverride = nullptr, const OFX::Image* img = nullptr) {
    int prec = 0;
    precision_->getValue(prec);
    std::string sig = std::string("v1|cs=") + colourKey() + "|prec=" + effectivePrecision(prec) +
                      "|model=" + (useDummy() ? kDummyVersion : kModelVersion);
    // Decisions 74/75; Off = the old key. Auto: one key part per threshold version.
    const int hold = frameHold();
    if (hold != 1) sig += "|hold=" + (hold == 0 ? std::string(kAutoHoldVersion) : std::to_string(hold));
    const RoiState st = roiOverride ? *roiOverride : currentRoiState();
    if (st.valid) {
      if (st.active)
        sig += "|roi=" + std::to_string(st.box.x) + "," + std::to_string(st.box.y) + "," +
               std::to_string(st.box.w) + "," + std::to_string(st.box.h) + ",k" +
               std::to_string(int(st.mask));
    } else if (img && alphaVaries(*img)) {
      sig += "|roi=unprocessed";
    }
    return sig;
  }

  std::string roiSettings() {
    int margin = 64;
    bool mask = false;
    roiMargin_->getValue(margin);
    roiMask_->getValue(mask);
    return "m" + std::to_string(margin) + "k" + std::to_string(int(mask));
  }

  // The saved ROI state, valid only while the ROI settings still match the ones it was made with.
  RoiState currentRoiState() {
    std::string v;
    roiState_->getValue(v);
    RoiState st = parseRoiState(v);
    if (st.valid && st.settings != roiSettings()) st.valid = false;
    return st;
  }

  // Decision 71: roiState doubles as the record that this node was processed. Every launched
  // Process writes it (since 0.1.3), it is saved with the setup and copied with the node, and
  // Purge Cache (node) clears it.
  bool processedBefore() {
    std::string v;
    roiState_->getValue(v);
    return !v.empty();
  }

  // cvt1.33: is this node's last job still starting or running? Its status file says so; a job
  // that died without saying is caught by its pid file, so Process can never stay locked.
  bool jobActive() {
    std::string id;
    std::chrono::steady_clock::time_point launched;
    {
      std::lock_guard<std::mutex> lock(jobMutex_);
      id = lastJobId_;
      launched = jobLaunched_;
    }
    if (id.empty()) return false;
    const std::string state = jsonField(readFile(cacheRoot() + "/jobs/" + id + ".json"), "state");
    if (!state.empty() && state != "running") return false;
    if (std::chrono::steady_clock::now() - launched < std::chrono::seconds(20)) return true;  // just started
    const std::string pid = readFile(stagingDir() + "/ltxag_job_" + id + ".pid");
    return !pid.empty() && kill(pid_t(std::atoi(pid.c_str())), 0) == 0;
  }

  std::string lastJobStatus() {
    std::string id;
    {
      std::lock_guard<std::mutex> lock(jobMutex_);
      id = lastJobId_;
    }
    return id.empty() ? "" : readFile(cacheRoot() + "/jobs/" + id + ".json");
  }

  std::string pythonPath() {
#if LTXAG_DEV_BUILD
    std::string p;
    python_->getValue(p);
    return p.empty() ? "/usr/bin/python3" : p;
#else
    // cvt1.43: fixed in a release. The Worker Python setting is saved with a comp, so a comp from
    // someone else could otherwise choose the program that Process or Download starts.
    return "/usr/bin/python3";
#endif
  }

  void error(const std::string& msg) {
    LOGW("error shown to user: %s", msg.c_str());
    // CVT Resolve port: also in the Status field, which the Inspector always shows (whether
    // Resolve shows sendMessage popups is not known; it refuses persistent messages).
    try {
      setIfChanged(status_, msg);
    } catch (const std::exception&) {
    }
    try {
      sendMessage(OFX::Message::eMessageError, "", msg);
    } catch (const std::exception& e) {
      LOGW("error: sendMessage FAILED: %s", e.what());
    }
  }

  std::string workerSocket() { return homeDir() + "/.cache/ltx-alphagen/run/worker.sock"; }

  // Decisions 12/34(c)/56: never silently downgrade. Size-aware estimate from the measurements
  // (vram_estimate.h). A live resident worker's memory counts as available whatever its
  // precision: a matching one is reused, another one is restarted (which frees it). Flame test 1
  // (2026-10-11) showed two false refusals when this only counted a matching precision.
  // Decision 56 (user, 2026-10-11): too little memory is a warning with Yes/No, not a refusal,
  // because the estimate rests on few measurements. The numbers go into the manifest so job.py
  // can log estimate vs actual (logs/vram_stats.csv).
  // GPU memory held by a live resident worker (counts as available whatever its precision).
  double residentWorkerGB(unsigned& pid) {
    pid = 0;
    const std::string info = readFile(workerSocket() + ".json");
    if (info.empty() || !pidAlive(std::atoi(jsonField(info, "pid").c_str()))) return 0.0;
    pid = unsigned(std::atoi(jsonField(info, "pid").c_str()));
    return std::atof(jsonField(info, "vram_gb").c_str());
  }

  // Decision 61: Auto takes the longest chunk size whose estimated need fits the available
  // memory, else the smallest. Without NVML it takes Full. The overlap goes with the size.
  // With a Frame Hold (74) the model frames of a chunk are what counts.
  int chooseChunkSize(int mode, bool fp8, int w, int h, int frames, bool resident, double head, int hold) {
    if (mode > 0) return std::min(mode, kChunkSizes - 1);  // cvt1.29: 1 Half, 2 Quarter (size index)
    const VramInfo v = queryVram();
    if (!v.ok) return 0;
    unsigned pid = 0;
    const double available = v.freeGB + residentWorkerGB(pid) - head;
    for (int s = 0; s < kChunkSizes; ++s) {
      const VramNeed n =
          estimateVramNeed(fp8, w, h, heldModelFrames(std::min(heldChunkLen(s, hold), frames), hold), resident, memoryFit(fp8 ? 1 : 0));
      LOGI("chunk size auto: %d frames/chunk needs %.1f GB, available %.1f GB%s", kSizeLen[s], n.gb, available,
           n.overMpsLimit ? "; over the Apple Silicon size limit" : "");
      if (n.overMpsLimit) continue;  // cvt1.28: MPS can't run it at all
      if (n.gb <= available) return s;
    }
    return kChunkSizes - 1;
  }

  // modelFrames: of the largest chunk, holds included (74/75).
  bool preflight(int modelFrames, int w, int h, std::string& vramJson) {
    int prec = 0;
    double head = 0.0, simNeed = 0.0;
    bool unload = true;
    precision_->getValue(prec);
    headroom_->getValue(head);
    unload_->getValue(unload);
    simNeed_->getValue(simNeed);
    const VramNeed est = estimateVramNeed(prec == 1, w, h, modelFrames, !unload, memoryFit(prec));
    const double need = simNeed > 0.0 ? simNeed : est.gb;
    const VramInfo v = queryVram();
    unsigned workerPid = 0;
    const double reusable = residentWorkerGB(workerPid);
    const double available = v.freeGB + reusable - head;
    LOGI("preflight: %s %dx%d largest chunk %d model frames -> model %dx%dx%d, need %.1f GB%s; free %.1f + resident "
         "%.1f - reserve %.1f = %.1f GB available (nvml %s)",
         effectivePrecision(prec).c_str(), w, h, modelFrames, est.modelW, est.modelH, est.modelFrames, need,
         simNeed > 0.0 ? " (simulated)" : "", v.freeGB, reusable, head, available, v.ok ? "ok" : "unavailable");
    bool warned = false;
    // cvt1.28: too big for Apple Silicon at this size, whatever the memory (MPS tensor limit).
    if (est.overMpsLimit && !useDummy()) {
      LOGW("preflight: %dx%dx%d is over the Apple Silicon size limit (%.0f Mpx-frames)", est.modelW, est.modelH,
           est.modelFrames, kMpsMaxMpxFrames);
      error("Chunk too large for Apple Silicon: " + std::to_string(est.modelW) + "x" + std::to_string(est.modelH) +
            " with " + std::to_string(est.modelFrames) + " frames per chunk is over what macOS's GPU framework "
            "can handle (it is not a memory problem). Set Chunk Size to Auto or Half (73), or Quarter (33) "
            "above HD.");
      return false;
    }
    // CVT Resolve port: the dummy uses no GPU memory, so it is only checked when Sim VRAM GB asks.
    bool skip = false;
    skipMemCheck_->getValue(skip);
    if (v.ok && need > available && (!useDummy() || simNeed > 0.0) && skip) {
      // cvt1.27: Skip Memory Check: the user runs it anyway (to find out whether a smaller Mac
      // copes, e.g. a 64 GB M1 Max: the model alone is 42 GB). The job's peak memory goes to
      // logs/vram_stats.csv as usual, flagged as warned.
      LOGW("preflight: needs %.1f GB, %.1f GB available; Skip Memory Check is on, processing anyway",
           need, available);
      warned = true;
    } else if (v.ok && need > available && (!useDummy() || simNeed > 0.0)) {
      // Who holds the rest: Flame (this process), our resident worker, everyone else.
      double flame = 0.0, others = 0.0;
      std::string list;
      int listed = 0;
      for (const auto& p : queryGpuProcesses()) {
        const double gb = std::max(0.0, p.usedGB);
        if (p.pid == unsigned(getpid())) {
          flame += gb;
        } else if (p.pid != workerPid) {
          others += gb;
          if (gb >= 0.1 && listed < 4) {
            list += std::string(listed++ ? ", " : "") + p.name + " " + fmtGB(gb);
          }
        }
      }
      LOGW("preflight: GPU %.1f GB total, %.1f free; host holds %.1f GB, LTX worker %.1f GB, other apps %.1f GB (%s)",
           v.totalGB, v.freeGB, flame, reusable, others, list.c_str());
      std::string msg = "This clip may not fit in GPU memory.\n\nEstimated need: about " + fmtGB(need) +
                        " GB (" + effectivePrecision(prec) + ", " + std::to_string(est.modelW) + "x" +
                        std::to_string(est.modelH) + ", " + std::to_string(est.modelFrames) +
                        " frames per chunk).\nAvailable: " + fmtGB(available) + " GB = " + fmtGB(v.freeGB) +
                        " GB free";
      if (reusable > 0.0) msg += " + " + fmtGB(reusable) + " GB held by the LTX worker";
      msg += " - " + fmtGB(head) + " GB reserved for " + hostShort() + ".\nIn use on the " + fmtGB(v.totalGB) +
             " GB GPU: " + hostShort() + " " + fmtGB(flame) + " GB, other apps " + fmtGB(others) + " GB" +
             (list.empty() ? "" : " (" + list + ")") + ".\n\nThe estimate is conservative, so it may " +
             (kMps ? "still work; if not, macOS swaps memory to disk and the job (and the Mac) slows down "
                     "badly. A smaller Chunk Size needs less."  // Metal over-commit swaps (devices.py)
                   : "still work; if not, the job stops with an out-of-memory error and " + hostShort() + " is not affected.") +
             (!kMps && prec == 0 ? " fp8 needs about 17 GB less." : "") + "\n\nProcess anyway?";
      const OFX::Message::MessageReplyEnum reply = sendMessage(OFX::Message::eMessageQuestion, "", msg);
      LOGI("preflight: warning shown, reply %d (0 OK, 1 Yes, 2 No, 3 Failed)", int(reply));
      if (reply == OFX::Message::eMessageReplyNo) return false;
      if (reply != OFX::Message::eMessageReplyYes) {
        // CVT Resolve port: Resolve can't ask Yes/No (reply Failed) and refuses persistent
        // messages, which threw and ended Process. On a Mac an over-committed run swaps the
        // whole machine instead of failing cleanly, so without an answer it stops here.
        error("Too large for this Mac's memory: needs about " + fmtGB(need) + " GB, " + fmtGB(available) +
              " GB available. Scale the clip down before this node (UHD to HD), give it an alpha "
              "ROI, or choose a smaller Chunk Size. Or tick Skip Memory Check (Speed and Quality) "
              "to try anyway.");
        return false;
      }
      warned = true;
    }
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "{\"estimate_gb\":%.2f,\"free_gb\":%.2f,\"total_gb\":%.2f,\"resident_gb\":%.2f,"
                  "\"reserve_gb\":%.2f,\"model_w\":%d,\"model_h\":%d,\"model_frames\":%d,\"nvml\":%s,"
                  "\"warned\":%s,",
                  need, v.freeGB, v.totalGB, reusable, head, est.modelW, est.modelH, est.modelFrames,
                  v.ok ? "true" : "false", warned ? "true" : "false");
    vramJson = std::string(buf) + "\"gpu\":" + jsonStr(v.name) + ",\"driver\":" + jsonStr(v.driver) + "}";
    return true;
  }

  // Fetches every frame of the clip during this action and hands them to a detached job.
  void process(double time) {
    if (gateClosed()) return error("Complete the Licence and Models setup first.");
    const UserConfig cfg = loadUserConfig();
    std::string modelDir;
    modelDir_->getValue(modelDir);
    modelDir = trimSlashes(modelDir);  // ".../models/" must not look like another folder (restarts the worker)
    const std::string ltxPython = cfg.count("ltx_python") ? cfg.at("ltx_python") : "";
    if (!useDummy()) {
      struct stat st {};
      if (ltxPython.empty() || stat(ltxPython.c_str(), &st) != 0)
        return error("The LTX runtime is not installed (no ltx_python in " + userConfigPath() + ").");
      if (stat((modelDir + "/diffusion_models").c_str(), &st) != 0)
        return error("No models found in the Model Folder: " + modelDir);
    }
    int first = 0, last = -1;
    if (!compRange(first, last)) return error("The clip and the comp's frame range do not overlap.");
    // cvt1.35: Resolve doesn't pass on the render range (the yellow bars), which Fusion Studio
    // does, so there the node has its own range. Only here, not in compRange: Purge Cache still
    // takes the whole comp. A comp moved to Fusion Studio ignores any values left in it.
    if (hostIsResolve()) {
      int wantFirst = 0, wantLast = 0;
      firstFrame_->getValue(wantFirst);
      lastFrame_->getValue(wantLast);
      const int clipFirst = first, clipLast = last;
      if (!limitRange(first, last, wantFirst, wantLast))
        return error("First Frame and Last Frame don't fit this clip's frames (" + std::to_string(clipFirst) + "-" +
                     std::to_string(clipLast) + ").");
      if (wantFirst > 0 || wantLast > 0)
        LOGI("process: First/Last Frame %d/%d: frames %d-%d of %d-%d", wantFirst, wantLast, first, last, clipFirst, clipLast);
    }
    const int n = last - first + 1;
    // Fail fast: the last frame first, so a range the host can't deliver errors now, not after
    // reading everything before it.
    {
      std::unique_ptr<OFX::Image> probe;
      try {
        probe.reset(src_->fetchImage(double(last)));
      } catch (const std::exception& e) {
        LOGE("process: fetchImage(%d) threw: %s", last, e.what());
      }
      if (!probe)
        return error(hostShort() + " did not provide the last frame (" + std::to_string(last) + ") of the range " +
                     std::to_string(first) + "-" + std::to_string(last) + ".");
    }
    // cvt1.07: the canvas, the union of every frame's region of definition (asking for it renders
    // nothing). With a Crop, a Magic Mask or a garbage matte upstream the regions differ.
    Canvas canvas;
    {
      double x1 = 1e30, y1 = 1e30, x2 = -1e30, y2 = -1e30;
      int minW = 1 << 30, maxW = 0, minH = 1 << 30, maxH = 0;
      for (int f = 0; f < n; ++f) {
        const OfxRectD rod = src_->getRegionOfDefinition(double(first + f));
        if (!(rod.x2 > rod.x1 && rod.y2 > rod.y1)) continue;  // an empty frame
        x1 = std::min(x1, rod.x1), y1 = std::min(y1, rod.y1), x2 = std::max(x2, rod.x2), y2 = std::max(y2, rod.y2);
        const int rw = int(std::lround(rod.x2 - rod.x1)), rh = int(std::lround(rod.y2 - rod.y1));
        minW = std::min(minW, rw), maxW = std::max(maxW, rw), minH = std::min(minH, rh), maxH = std::max(maxH, rh);
      }
      if (x2 <= x1 || y2 <= y1) return error("The source is empty in every frame of the comp.");
      canvas.x = int(std::floor(x1)), canvas.y = int(std::floor(y1));
      canvas.w = int(std::ceil(x2)) - canvas.x, canvas.h = int(std::ceil(y2)) - canvas.y;
      LOGI("process: frame regions %d-%dx%d-%d px, canvas x %d y %d %dx%d", minW, maxW, minH, maxH, canvas.x,
           canvas.y, canvas.w, canvas.h);
    }
    const int fullW = canvas.w, fullH = canvas.h;
    LOGI("process: reading clip [%d,%d] (%d frames, %dx%d) on the host's UI thread", first, first + n - 1, n,
         fullW, fullH);
    // CVT Resolve port: the host's progress bar for the whole read (ended on every return).
    struct ProgressScope {
      OFX::ImageEffect& fx;
      explicit ProgressScope(OFX::ImageEffect& e, const std::string& msg) : fx(e) {
        try { fx.progressStart(msg, "ltxag_staging"); } catch (const std::exception&) {}
      }
      ~ProgressScope() {
        try { fx.progressEnd(); } catch (const std::exception&) {}
      }
    } progress(*this, "LTX Alpha Gen: reading " + std::to_string(n) + " frames");

    // Decision 63: the crop. Without an alpha matte the box is the whole frame.
    RoiState roi;
    std::string err;
    stagingBase_ = 0.0;
    if (!computeRoi(first, n, canvas, roi, err)) return error(err);
    if (roi.active) stagingBase_ = 0.5;  // the ROI scan was the first half of the bar
    const bool useRoi = roi.active;
    // Decision 72: no 6K. Checked on what the model sees (the crop), before anything is staged.
    if (std::max(roi.box.w, roi.box.h) >= kMaxLongSide)
      return error(std::string(useRoi ? "The region of interest" : "The frame") + " is " +
                   std::to_string(roi.box.w) + "x" + std::to_string(roi.box.h) +
                   ": too large for the LTX model (6K and above are not supported). Scale the clip "
                   "down before this node and the matte up after it, or give the source an alpha "
                   "that limits the region of interest.");
    int prec = 0, chunkMode = 0, mergeMode = 0;
    bool unload = true;
    double head = 0.0;
    precision_->getValue(prec);
    if (kMps && prec == 1)  // decision 34: no silent downgrade
      return error("fp8 is not available on Apple Silicon (MPS has no fp8 type): set Precision to bf16.");
    unload_->getValue(unload);
    headroom_->getValue(head);
    chunkSize_->getValue(chunkMode);
    chunkMode = std::min(std::max(chunkMode, 0), kChunkSizeChoices - 1);
    merge_->getValue(mergeMode);
    // Frame Hold (74/75). Auto plans its chunks after staging, from the motion measured while
    // staging; memory is checked before that against the worst case (4x), which bounds any plan.
    const int hold = frameHold();
    const int planHold = hold == 0 ? kAutoMaxHold : hold;
    const int sizeIdx = chooseChunkSize(chunkMode, prec == 1, roi.box.w, roi.box.h, n, !unload, head, planHold);
    const int overlap = hold == 0 ? kSizeOverlap[sizeIdx] : heldOverlap(sizeIdx, hold);
    std::vector<Chunk> chunks;
    int modelFrames = std::min(kSizeLen[sizeIdx], heldModelFrames(n, planHold));
    if (hold != 0) {
      chunks = planChunks(n, heldChunkLen(sizeIdx, hold), overlap);
      modelFrames = 0;
      for (auto& c : chunks) c.hold = hold, modelFrames = std::max(modelFrames, heldModelFrames(c.len, hold));
    }
    LOGI("process: chunk size %s -> %d model frames, overlap %d; frame hold %s; merge %s",
         kChunkSizeNames[chunkMode], kSizeLen[sizeIdx], overlap, kFrameHoldNames[std::min(kFrameHoldChoices - 1, std::max(0, frameHoldIndex()))],
         kMergeNames[mergeMode]);
    std::string vramJson;
    if (!preflight(modelFrames, roi.box.w, roi.box.h, vramJson)) return;
    const std::string sig = paramSignature(&roi);
    const std::string root = cacheRoot();
    const std::string jobId = std::to_string(getpid()) + "-" + std::to_string(gJobSeq++) + "-" +
                              std::to_string(std::time(nullptr));
    LogScope scope(nodeId_, jobId);
    const std::string input = stagingDir() + "/ltxag_job_" + jobId + "_in.bin";
    LOGI("process: t=%g clip [%d,%d] %d frames, %s chunk(s), params %s", time, first, first + n - 1, n,
         hold == 0 ? "auto" : std::to_string(chunks.size()).c_str(), sig.c_str());

    std::vector<std::string> keys(n);
    int w = 0, h = 0;
    const int cw = roi.box.w, ch = roi.box.h;  // what the model sees
    FrameWriter writer;
    std::vector<float> rgb, maskRow, onCanvas, thumb;
    std::vector<uint16_t> frame;
    std::unique_ptr<MotionAnalyzer> analyzer;  // Auto Frame Hold (75)
    if (hold == 0) analyzer.reset(new MotionAnalyzer(cw, ch));
    std::vector<double> motion(n, 0.0);
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < n; ++f) {
      if (!stagingProgress(stagingBase_ + (1.0 - stagingBase_) * f / n, f, n, "staging")) {
        writer.close();
        unlink(input.c_str());
        LOGI("process: cancelled while staging frame %d of %d", f, n);
        return error("Cancelled.");
      }
      std::unique_ptr<OFX::Image> img;
      try {
        img.reset(src_->fetchImage(double(first + f)));
      } catch (const std::exception& e) {
        LOGE("process: fetchImage(%d) threw: %s", first + f, e.what());
      }
      if (!img) {
        writer.close();
        unlink(input.c_str());
        return error(hostShort() + " did not provide source frame " + std::to_string(first + f) + ".");
      }
      if (img->getPixelDepth() != OFX::eBitDepthFloat) {
        writer.close();
        unlink(input.c_str());
        return error("Only 32-bit float images are supported.");
      }
      if (f == 0) {
        w = fullW, h = fullH;  // cvt1.07: the canvas, whatever this frame's own size
        LOGI("process: source %s depth %d on a %dx%d canvas", img->getPixelComponentsProperty().c_str(),
             int(img->getPixelDepth()), w, h);
        if (!writer.open(input, cw, ch, n, err)) {
          unlink(input.c_str());
          return error(err);
        }
        rgb.resize(size_t(cw) * 3);
        maskRow.resize(size_t(w));
        frame.resize(size_t(cw) * ch * 3);
      }
      placeOnCanvas(*img, canvas, onCanvas);  // cvt1.07: frames may differ in size (Crop, Magic Mask)
      const int sc = 4;
      // Black Out Outside ROI: the source times its own alpha matte, in the source's colour space
      // (black stays near black after the colour conversion).
      const bool blackOut = useRoi && roi.mask;
      for (int row = 0; row < ch; ++row) {
        const int srcRow = roi.box.y + row;
        const float* canvasRow = onCanvas.data() + size_t(srcRow) * w * 4;
        const float* p = canvasRow + size_t(roi.box.x) * 4;
        for (int x = 0; x < cw; ++x)
          for (int k = 0; k < 3; ++k) rgb[size_t(x) * 3 + k] = p[size_t(x) * sc + k];
        if (useRoi && (blackOut || analyzer)) roiRowAlpha(canvasRow, sc, w, maskRow.data());
        // Motion on the picture before Black Out, inside the ROI matte (75).
        if (analyzer) analyzer->addRow(row, rgb.data(), useRoi ? maskRow.data() + roi.box.x : nullptr);
        if (blackOut) {
          for (int x = 0; x < cw; ++x)
            for (int k = 0; k < 3; ++k) rgb[size_t(x) * 3 + k] *= maskRow[size_t(roi.box.x + x)];
        }
        floatToHalf(rgb.data(), frame.data() + size_t(row) * cw * 3, rgb.size());
      }
      if (analyzer) motion[f] = analyzer->endFrame();
      thumbnailOf(onCanvas.data(), w, h, thumb);  // cvt1.18: found by node, frame and thumbnail
      keys[f] = takeSlot(root, first + f, canvasSignature(sig, canvas), thumb);
      if (!writer.writeFrame(frame.data(), err)) {
        writer.close();
        unlink(input.c_str());
        return error(err);
      }
    }
    writer.close();
    const double sec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    LOGI("process: staged %d frames in %.2f s (%.1f ms/frame), %.1f MB in %s", n, sec,
         1000.0 * sec / n, double(n) * cw * ch * 6 / 1048576.0, input.c_str());
    if (hold == 0) {
      chunks = planAutoChunks(autoHolds(motion), sizeIdx);
      std::vector<double> sorted(motion.begin() + 1, motion.end());
      std::sort(sorted.begin(), sorted.end());
      std::string plan;
      const int total = planModelFrames(chunks);
      for (const auto& c : chunks)
        plan += (plan.empty() ? "" : " ") + std::to_string(c.start) + "+" + std::to_string(c.len) + "@" +
                std::to_string(c.hold) + "x";
      LOGI("process: auto hold: motion px/frame median %.1f, max %.1f; %d chunk(s) %s; %d model frames "
           "for %d frames",
           sorted.empty() ? 0.0 : sorted[sorted.size() / 2], sorted.empty() ? 0.0 : sorted.back(),
           int(chunks.size()), plan.c_str(), total, n);
      std::string per;
      for (int f = 1; f < n; ++f) per += (f > 1 ? " " : "") + std::to_string(int(std::lround(motion[f])));
      LOGD("process: auto hold: motion per frame %s", per.c_str());
    }

    double seconds = 10.0;
    int sim = 0;
    simSeconds_->getValue(seconds);
    simulate_->getValue(sim);
    std::ostringstream js;
    js << "{\"job_id\":" << jsonStr(jobId) << ",\"node_id\":" << jsonStr(nodeId_)
       << ",\"input\":" << jsonStr(input) << ",\"width\":" << cw << ",\"height\":" << ch
       << ",\"full_width\":" << w << ",\"full_height\":" << h;
    if (useRoi)
      js << ",\"roi\":[" << roi.box.x << "," << roi.box.y << "," << roi.box.w << "," << roi.box.h << "]";
    js << ",\"first_frame\":" << first;  // cvt1.29: for the seam lines in the log
    js << ",\"merge\":" << (mergeMode == 1 ? "\"flow\"" : "\"smooth\"")
       << ",\"frames\":" << n << ",\"chunks\":[";
    for (size_t c = 0; c < chunks.size(); ++c)
      js << (c ? "," : "") << "[" << chunks[c].start << "," << chunks[c].len << "]";
    js << "],\"holds\":[";
    for (size_t c = 0; c < chunks.size(); ++c) js << (c ? "," : "") << chunks[c].hold;
    js << "],\"frame_keys\":[";
    for (int f = 0; f < n; ++f) js << (f ? "," : "") << jsonStr(keys[f]);
    js << "],\"contrib\":[";
    for (int f = 0; f < n; ++f) {
      js << (f ? "," : "") << "[";
      // cvt1.31: the eased (smoothstep) seam curve for both modes. Flow Warp had the linear one,
      // which kept ~3x more of the other chunk near the seam ends (and a stronger warp there).
      const auto contrib = frameContributions(chunks, f, overlap, true);
      for (size_t c = 0; c < contrib.size(); ++c)
        js << (c ? "," : "") << "[" << contrib[c].chunk << "," << contrib[c].weight << "]";
      js << "]";
    }
    js << "],\"cache_root\":" << jsonStr(root)
       << ",\"worker\":"
       << jsonStr(resourcesDir() + (useDummy() ? "/worker/dummy_worker.py" : "/worker/ltx_worker.py"));
    // Input Colour: the mode and the colourspace the host delivered the frames in (worker:
    // colour.Converter). ocio_config stays "": the worker uses Flame's ACES 2.0 config, which has
    // the fixed view; the host's config is only logged.
    const std::string deliveredCs = deliveredColourspace();
    LOGI("process: Input Colour %s, Source delivered in '%s' (host OCIO config '%s')",
         kColourModeNames[colourMode()], deliveredCs.c_str(),
         getPropertySet().propGetString(kOfxImageEffectPropOCIOConfig, false).c_str());
    js << ",\"colour\":" << jsonStr(kColourModeNames[colourMode()]) << ",\"colour_tag\":" << jsonStr(deliveredCs);
    if (!useDummy()) {
      js << ",\"worker_python\":" << jsonStr(ltxPython) << ",\"worker_args\":[\"--model-dir\","
         << jsonStr(modelDir) << ",\"--embeddings\"," << jsonStr(root + "/prompt-empty.safetensors")
         << "],\"worker_socket\":" << jsonStr(workerSocket()) << ",\"model_dir\":" << jsonStr(modelDir)
         << ",\"unload\":" << (unload ? "true" : "false") << ",\"ocio_config\":\"\",\"seed\":1234";
    }
    js
       << ",\"seconds\":" << seconds << ",\"simulate\":" << jsonStr(kSimulations[sim])
       << ",\"precision\":" << jsonStr(effectivePrecision(prec)) << ",\"vram\":" << vramJson
       << ",\"flame_pid\":" << getpid()
       << ",\"title\":" << jsonStr("LTX Alpha Gen - " + std::to_string(n) + " frames") << "}";
    const std::string manifest = stagingDir() + "/ltxag_job_" + jobId + ".json";
    {
      std::ofstream mf(manifest);
      mf << js.str();
    }
    chmod(manifest.c_str(), 0600);  // cvt1.43: Resolve's umask is 000, and this file tells a job what to do
    const std::string level = logLevel() == LogLevel::Debug ? "debug" : "info";
    // Before the launch: the render Flame triggers right after this button press must already
    // see PROCESSING (test 2: it raced job.py's own markers and showed NOT PROCESSED).
    setIfChanged(roiState_, formatRoiState(roi));  // render keys on it from now on
    setIfChanged(seams_, seamsText(chunks, first));  // cvt1.20: where to look for seams
    setIfChanged(canvasState_, formatCanvas(canvas));  // and places the matte on this (cvt1.07)
    setIfChanged(roiInfo_, useRoi ? "Crop " + std::to_string(roi.box.w) + "x" + std::to_string(roi.box.h)
                                  : std::string("None (alpha opaque)"));
    markPending(root, keys, getpid());
    // job.py needs numpy: the system python3 has it on Rocky 9 but not on macOS, where the job
    // runs under the LTX runtime's own Python (decision 65; Setup is stdlib-only and keeps
    // pythonPath()).
#ifdef __APPLE__
    const std::string jobPython = ltxPython.empty() ? pythonPath() : ltxPython;
#else
    const std::string jobPython = pythonPath();
#endif
    const bool ok = launchDetached(
        {jobPython, "-I", "-B", resourcesDir() + "/worker/job.py", manifest},
        cleanEnvironment({"LTXAG_LOG_LEVEL=" + level}), err);
    if (!ok) {
      clearPending(root, keys);
      unlink(input.c_str());
      unlink(manifest.c_str());
      return error("Could not start processing: " + err);
    }
    {
      std::lock_guard<std::mutex> lock(jobMutex_);
      lastJobId_ = jobId;
      jobLaunched_ = std::chrono::steady_clock::now();
    }
    setIfChanged(status_, "Processing " + std::to_string(n) + " frames");
    LOGI("process: job launched (job python %s, worker %s)", jobPython.c_str(),
         useDummy() ? "dummy" : ltxPython.c_str());
    startWatcher(jobId, root, keys);
  }

  // --- Decision 42 (a): follow the job and nudge Flame to re-render. EXPERIMENT. ---
  void startWatcher(const std::string& jobId, const std::string& root, const std::vector<std::string>& keys) {
    stopWatcher();
    bool enabled = true;
    autoRefresh_->getValue(enabled);
    if (!enabled) {
      LOGI("auto refresh off: no watcher");
      return;
    }
    stopFlag_ = false;
    const std::string node = nodeId_;
    watcher_ = std::thread([this, jobId, root, node, keys] { watch(jobId, root, node, keys); });
  }

  void stopWatcher() {
    stopFlag_ = true;
    if (watcher_.joinable()) watcher_.join();
  }

  // Setup runs as its own process; nothing tells the node when it is done, so the status stayed
  // "Models: NO" and the params disabled until the next user edit (Mac test 2026-10-13). This
  // thread polls the user config until Setup reports ready, then refreshes the Setup UI; changes
  // from a non-action thread show at the next interaction in Flame (see "Flame host behaviour").
  void startSetupWatcher() {
    stopSetupWatcher();
    setupStop_ = false;
    const std::string node = nodeId_;
    setupWatcher_ = std::thread([this, node] {
      LogScope scope(node);
      LOGI("setup watcher started (this thread is not an OFX action thread)");
      const auto t0 = std::chrono::steady_clock::now();
      while (!setupStop_ && std::chrono::steady_clock::now() - t0 < std::chrono::hours(4)) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (!setupState().ready()) continue;
        try {
          refreshSetupUi();
          LOGI("setup watcher: setup complete; Setup UI refreshed from a non-action thread");
        } catch (const std::exception& e) {
          LOGE("setup watcher: refresh FAILED: %s [%s]", e.what(), typeid(e).name());
        }
        return;
      }
      LOGI("setup watcher finished without a ready setup");
    });
  }

  void stopSetupWatcher() {
    setupStop_ = true;
    if (setupWatcher_.joinable()) setupWatcher_.join();
  }

  void watch(const std::string& jobId, const std::string& root, const std::string& node,
             const std::vector<std::string>& keys) {
    LogScope scope(node, jobId);
    LOGI("watcher started (this thread is not an OFX action thread)");
    std::string lastSeen;
    int tick = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (!stopFlag_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      const std::string j = readFile(root + "/jobs/" + jobId + ".json");
      if (j.empty()) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(60)) {
          // job.py never ran, so the markers written before the launch (with Flame's pid) would
          // otherwise show PROCESSING for as long as Flame runs.
          clearPending(root, keys);
          LOGW("watcher: job wrote no status within 60 s; cleared its pending markers, giving up");
          return;
        }
        continue;
      }
      const std::string state = jsonField(j, "state");
      const std::string seen = state + "/" + jsonField(j, "chunks_done");
      if (seen == lastSeen) continue;
      lastSeen = seen;
      std::string text;
      if (state == "running")
        text = "Processing: " + jsonField(j, "chunks_done") + "/" + jsonField(j, "chunks") +
               " chunks done";
      else if (state == "done")
        text = "Done: " + jsonField(j, "frames_done") + " frames";
      else if (state == "cancelled")
        text = "Cancelled";
      else
        text = "Failed: " + jsonField(j, "message").substr(0, 20);
      try {
        LOGI("watcher: %s -> status + refresh tick %d from a non-action thread", seen.c_str(),
             tick + 1);
        setIfChanged(status_, text);
        ++tick;
        bumpRefreshTick();
        LOGI("watcher: host accepted the param changes");
      } catch (const std::exception& e) {
        LOGE("watcher: param change FAILED: %s [%s]", e.what(), typeid(e).name());
        return;
      }
      if (state != "running") break;
    }
    LOGI("watcher finished");
  }

  // CVT Resolve port: the frames Process reads. In Resolve and Fusion the Source clip's range is
  // far longer than the comp (the whole media; frames past the comp are not delivered: "did not
  // provide source frame 4858" after a 6-minute read), so it is limited to the effect's timeline
  // bounds. Each lookup on its own: a host may not support one (cvt1.02 asked for the output
  // clip's range, Resolve threw kOfxStatFailed, and Process ended silently). cvt1.10: shared with
  // Purge Cache, which read the whole media too. False if the ranges do not overlap.
  bool compRange(int& first, int& last) {
    const OfxRangeD r = src_->getFrameRange();
    first = int(std::lround(r.min));
    last = int(std::lround(r.max));
    double t1 = 0.0, t2 = 0.0;
    try {
      timeLineGetBounds(t1, t2);
    } catch (const std::exception& e) {
      LOGW("range: timeline bounds not available: %s", e.what());
    }
    OfxRangeD dr{0.0, -1.0};
    try {
      dr = dst_->getFrameRange();
    } catch (const std::exception& e) {
      LOGW("range: output clip range not available: %s", e.what());
    }
    LOGI("range: source [%g,%g], output [%g,%g], timeline [%g,%g]", r.min, r.max, dr.min, dr.max, t1, t2);
    if (!(t2 > t1) && dr.max >= dr.min) t1 = dr.min, t2 = dr.max;  // no timeline bounds: the output's range
    if (t2 > t1) {
      first = std::max(first, int(std::lround(t1)));
      last = std::min(last, int(std::lround(t2)));
    }
    return last >= first;
  }

  void purgeNode() {
    int first = 0, last = -1;
    const std::string sig = paramSignature();
    std::vector<std::string> keys;
    const std::string root = cacheRoot();
    if (compRange(first, last)) {  // cvt1.10: the comp's frames, not the whole media
      for (int t = first; t <= last; ++t) {
        std::unique_ptr<OFX::Image> img(src_->fetchImage(double(t)));
        if (!img) continue;
        keys.push_back(matteKey(hashImage(*img), sig));  // a matte made before cvt1.18
        // cvt1.18: this clip's slot (the one whose thumbnail matches); other clips' slots stay.
        if (img->getPixelDepth() != OFX::eBitDepthFloat) continue;
        bool changed = false;
        const Canvas cv = renderCanvas(img->getBounds());
        const std::string k = lookupMatte(root, t, canvasSignature(paramSignature(nullptr, img.get()), cv), *img,
                                          cv, changed);
        if (!k.empty()) keys.push_back(k);
      }
    }
    const size_t n = purgeKeys(root, keys);
    LOGI("purgeNode: frames [%d,%d], %zu of %zu had cached mattes, removed", first, last, n, keys.size());
    setIfChanged(roiState_, "");  // back to never processed: pass-through (decision 71)
    setIfChanged(seams_, "");  // cvt1.20
    setIfChanged(canvasState_, "");  // cvt1.10: and no saved canvas
    setIfChanged(status_, "Cache purged: " + std::to_string(n) + " frames");
    refreshCacheInfo();
  }

  // cvt1.24: Fusion's Reset (the Inspector's reset button) copies every visible param from a
  // fresh default node as a user edit, read-only labels included, and leaves the hidden ones
  // (node id, processed record) alone, so the old mattes kept showing. A user can't edit the
  // "Port by" credit and undo never changes it, so its change means Reset: start fresh, as Purge
  // Cache does (this clip's mattes removed, back to never processed), and forget the last job.
  void resetNode() {
    LOGI("reset: Fusion reset the node, purging its mattes and state");
    purgeNode();
    {
      std::lock_guard<std::mutex> lock(jobMutex_);
      lastJobId_.clear();
    }
    setIfChanged(status_, "Not processed");
    bumpRefreshTick();
  }

  void saveAsDefault() {
    int prec = 0;
    double head = 0.0;
    bool unl = true;
    precision_->getValue(prec);
    headroom_->getValue(head);
    unload_->getValue(unl);
    UserConfig cfg = loadUserConfig();
    cfg["precision"] = kPrecisions[prec];
    cfg["headroom_gb"] = fmtGB(head);
    cfg["unload"] = unl ? "1" : "0";
    const bool ok = saveUserConfig(cfg);
    LOGI("saveAsDefault: %s precision=%s headroom=%.1f unload=%d", ok ? "ok" : "FAILED",
         kPrecisions[prec], head, int(unl));
    sendMessage(OFX::Message::eMessageMessage, "",
                ok ? "Saved as your default for new LTX Alpha Gen nodes."
                   : "Could not write " + userConfigPath());
  }

  void download() {
    std::string token, dir, existing;
    token_->getValue(token);
    modelDir_->getValue(dir);
    existingDir_->getValue(existing);
    dir = trimSlashes(dir);
    existing = trimSlashes(existing);
    LOGI("download pressed: licence=%d token %s model dir=%s existing=%s",
         int(setupState().licenceAccepted), token.empty() ? "none" : "given", dir.c_str(), existing.c_str());
    if (!setupState().licenceAccepted)
      return error("Accept the LTX-2 Community License before downloading.");
    // cvt1.42: before the token is used and cleared, so it is still there for the next try.
    if (pythonPath() == "/usr/bin/python3" && !appleToolsInstalled())
      return error("Apple's Command Line Tools are missing. In Terminal, run xcode-select --install, "
                   "then press Download again.");
    // The token is optional: everything may already be in the Model Folder or the Existing
    // Models folder (decision 51). The helper says which files still need it.
    std::string err;
    const std::string helper = resourcesDir() + "/helper/setup_helper.py";
    std::vector<std::string> argv = {pythonPath(), "-I", "-B", helper, "--model-dir", dir,
                                     "--cache-root", cacheRoot()};
    if (!existing.empty()) {
      argv.insert(argv.end(), {"--existing", existing});
      // Decision 66: link to the existing files instead of copying them. cvt1.47: a checkbox, because
      // Fusion never showed the question this used to ask, so it always copied.
      bool link = false;
      linkExisting_->getValue(link);
      if (link) argv.push_back("--link");
    }
    const std::string level = logLevel() == LogLevel::Debug ? "debug" : "info";
    Env extras = {"LTXAG_LOG_LEVEL=" + level};
    if (!token.empty()) extras.push_back("HF_TOKEN=" + token);
    const bool ok = launchDetached(argv, cleanEnvironment(extras), err);
    token.assign(token.size(), '\0');
    token_->setValue("");  // decision 13: used once, then discarded
    // Decision 41: say what happened to the token instead of leaving a blank field.
    setIfChanged(tokenCaption_, ok && !token.empty() ? "Token used " + timeStr("%H:%M") + ", not stored"
                                                     : std::string(kTokenCaptionDefault));
    if (!ok) error("Download failed to start: " + err);
    LOGI("download: helper %s %s; token cleared", ok ? "launched" : "FAILED", err.c_str());
    if (ok) startSetupWatcher();
  }

  // Bundles logs, environment and job status files for a support request (decision 39).
  void collectLogs() {
    const std::string dir = logDir();
    {
      std::ofstream envf(dir + "/environment.txt");
      for (const auto& line : environmentSnapshot(hostInfo(), resourcesDir())) envf << line << "\n";
    }
    chmod((dir + "/environment.txt").c_str(), 0644);  // cvt1.43
    const std::string bundle =
        homeDir() + "/ltx-alphagen-support-" + timeStr("%Y%m%d-%H%M%S") + ".tar.gz";
    std::vector<std::string> argv = {"/usr/bin/tar", "-czf", bundle, "-C",
                                     dir.substr(0, dir.rfind('/')), "logs"};
    const std::string root = cacheRoot();
    struct stat st {};
    if (stat((root + "/jobs").c_str(), &st) == 0) {
      argv.insert(argv.end(), {"-C", root, "jobs"});
    }
    std::string err;
    WorkerProcess tar;
    std::vector<std::string> lines;
    const bool started = tar.start(argv, cleanEnvironment(), err);
    if (started)
      while (tar.readLines(200, lines)) {
      }
    const int status = started ? tar.wait() : -1;
    const bool ok = started && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    for (const auto& l : lines) LOGW("collectLogs: tar: %s", l.c_str());
    LOGI("collectLogs: %s -> %s %s", ok ? "ok" : "FAILED", bundle.c_str(), err.c_str());
    sendMessage(ok ? OFX::Message::eMessageMessage : OFX::Message::eMessageError, "",
                ok ? "Support bundle written to " + bundle +
                         ". Please review it before sending: it contains your logs, system "
                         "details and folder paths, but no images and no token."
                   : "Could not write the support bundle: " + err);
  }

  OFX::Clip* src_;
  OFX::Clip* dst_;
  OFX::ChoiceParam* chunkSize_;
  OFX::ChoiceParam* frameHold_;
  OFX::ChoiceParam* merge_;
  OFX::IntParam* roiMargin_;
  OFX::BooleanParam* roiMask_;
  OFX::BooleanParam* roiLimit_;
  OFX::StringParam* roiState_;
  OFX::StringParam* roiInfo_;
  OFX::ChoiceParam* colourMode_;
  OFX::ChoiceParam* legacyColour_;
  OFX::StringParam* status_;
  OFX::BooleanParam* showMarker_;
  OFX::ChoiceParam* precision_;
  OFX::BooleanParam* unload_;
  OFX::DoubleParam* headroom_;
  OFX::BooleanParam* accept_;
  OFX::StringParam* tokenCaption_;
  OFX::StringParam* token_;
  OFX::StringParam* modelDir_;
  OFX::StringParam* existingDir_;
  OFX::BooleanParam* linkExisting_;
  OFX::StringParam* setupStatus_;
  OFX::BooleanParam* debugLog_;
  OFX::StringParam* cacheDir_;
  OFX::StringParam* cacheInfo_;
  OFX::DoubleParam* cacheLimit_;  // cvt1.14
  OFX::DoubleParam* simSeconds_;
  OFX::ChoiceParam* simulate_;
  OFX::DoubleParam* simNeed_;
  OFX::StringParam* python_;
  OFX::BooleanParam* enforceGate_;
  OFX::BooleanParam* autoRefresh_;
  OFX::BooleanParam* useDummy_;
  OFX::BooleanParam* initialized_;
  OFX::StringParam* nodeIdParam_;
  OFX::IntParam* refreshTick_;
  OFX::ChoiceParam* output_;
  OFX::StringParam* canvasState_;
  bool setupHidden_ = false;  // cvt1.09: download controls currently hidden
  OFX::BooleanParam* showSetup_;  // cvt1.10
  OFX::BooleanParam* postMultiply_;  // cvt1.10
  OFX::DoubleParam* blackClip_;      // cvt1.26
  OFX::BooleanParam* skipMemCheck_;  // cvt1.27
  OFX::IntParam* firstFrame_;        // cvt1.35
  OFX::IntParam* lastFrame_;         // cvt1.35
  OFX::DoubleParam* whiteClip_;      // cvt1.26
  OFX::StringParam* seams_;  // cvt1.20
  double stagingBase_ = 0.0;  // CVT Resolve port: progress bar share already used by the ROI scan
  std::chrono::steady_clock::time_point stagingT0_;
  std::string nodeId_;
  std::mutex jobMutex_;
  std::string lastJobId_;
  std::chrono::steady_clock::time_point jobLaunched_;  // cvt1.33
  std::atomic<bool> finalRender_{false};               // cvt1.33: not a viewer render
  bool inChangedParam_ = false;
  bool loggedOutput_ = false;
  bool loggedDeliveredCs_ = false;
  std::string lastOutputCs_;
  std::string lastDeliveredCs_;  // render logs the delivered colourspace when it changes
  std::thread watcher_;
  std::thread setupWatcher_;
  std::thread browseThread_;
  std::atomic<bool> browsing_{false};
  std::mutex browseMutex_;
  std::shared_ptr<WorkerProcess> browseProc_;
  std::atomic<bool> setupStop_{false};
  bool gateWasClosed_ = false;  // render: refresh the Setup UI once Setup has completed
  std::atomic<bool> stopFlag_{false};
};

template <class D>
D* labelled(D* p, const char* label, const char* hint) {
  p->setLabels(label, label, label);
  if (hint && *hint) p->setHint(hint);
  return p;
}

class AlphaGenFactory : public OFX::PluginFactoryHelper<AlphaGenFactory> {
 public:
  AlphaGenFactory()
      : PluginFactoryHelper<AlphaGenFactory>(kPluginId, LTXAG_VERSION_MAJOR, LTXAG_VERSION_MINOR) {}

  // Input Colour Auto: after the Support library's GetClipPreferences, the OpenFX 1.5 colour
  // request on the out args (the 1.4 Support library has no such call; as Matte Refine ML).
  static OfxStatus entry(const char* action, const void* handle, OfxPropertySetHandle in,
                         OfxPropertySetHandle out) {
    if (std::strcmp(action, kOfxImageEffectActionGetOutputColourspace) == 0) {
      try {
        auto* fx = dynamic_cast<AlphaGenEffect*>(
            OFX::Private::retrieveImageEffectPointer((OfxImageEffectHandle)handle));
        const std::string cs = fx ? fx->outputColourspace() : "";
        if (cs.empty()) return kOfxStatReplyDefault;
        OFX::PropertySet(out).propSetString(kOfxImageClipPropColourspace, cs);
        return kOfxStatOK;
      } catch (const std::exception& e) {
        LOGE("colour: GetOutputColourspace FAILED: %s", e.what());
        return kOfxStatFailed;
      }
    }
    const OfxStatus st = FactoryMainEntryHelper<AlphaGenFactory>::mainEntry(action, handle, in, out);
    if (std::strcmp(action, kOfxImageEffectActionGetClipPreferences) != 0) return st;
    if (st != kOfxStatOK && st != kOfxStatReplyDefault) return st;
    try {
      auto* fx = dynamic_cast<AlphaGenEffect*>(
          OFX::Private::retrieveImageEffectPointer((OfxImageEffectHandle)handle));
      return fx ? fx->requestColour(out, st) : st;
    } catch (const std::exception& e) {
      LOGE("colour: clip preferences FAILED: %s", e.what());
      return st;
    }
  }
  OfxPluginEntryPoint* getMainEntry() override { return entry; }

  void describe(OFX::ImageEffectDescriptor& desc) override {
    logConfigure();
    rotateLogs();
    LOGI("==== plugin load (describe) ====");
    for (const auto& line : environmentSnapshot(hostInfo(), resourcesDir()))
      LOGI("env %s", line.c_str());
    const size_t staged = sweepStaleStaging();
    const SweepResult sw = sweepCache(cacheSettings(loadUserConfig()));
    LOGI("sweep: %zu stale staging file(s); cache aged %zu, capped %zu, kept %zu (%.1f MB), "
         "freed %.1f MB",
         staged, sw.aged, sw.capped, sw.kept, sw.bytesKept / 1048576.0, sw.bytesFreed / 1048576.0);
    desc.setLabels(kPluginLabel, kPluginLabel, kPluginLabel);
    desc.setPluginGrouping(kPluginGroup);
    desc.addSupportedContext(OFX::eContextFilter);
    desc.addSupportedBitDepth(OFX::eBitDepthFloat);
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(false);
    desc.setSupportsTiles(false);
    desc.setTemporalClipAccess(true);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setRenderThreadSafety(OFX::eRenderInstanceSafe);
    // Input Colour Auto: OpenFX 1.5 colour management (ofxColour.h), as Matte Refine ML declares
    // it; a host without it ignores these, and the source arrives unconverted (no tag).
    desc.getPropertySet().propSetString(kOfxImageEffectPropColourManagementStyle,
                                        kOfxImageEffectColourManagementOCIO, false);
    desc.getPropertySet().propSetString(kOfxImageEffectPropColourManagementAvailableConfigs,
                                        "ofx-native-v1.5_aces-v1.3_ocio-v2.3", false);
  }

  void describeInContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum ctx) override {
    try {
      describeParams(desc);
      LOGI("describeInContext ctx=%d done", int(ctx));
    } catch (const std::exception& e) {
      LOGE("describeInContext ctx=%d FAILED: %s [%s]", int(ctx), e.what(), typeid(e).name());
      throw;
    }
  }

  void describeParams(OFX::ImageEffectDescriptor& desc) {
    const Defaults d = computeDefaults();
    LOGI("describeInContext: defaults [%s]", d.source.c_str());

    OFX::ClipDescriptor* src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    src->addSupportedComponent(OFX::ePixelComponentRGBA);
    src->addSupportedComponent(OFX::ePixelComponentRGB);
    src->setTemporalClipAccess(true);
    src->setSupportsTiles(false);
    OFX::ClipDescriptor* dst = desc.defineClip(kOfxImageEffectOutputClipName);
    dst->addSupportedComponent(OFX::ePixelComponentAlpha);
    dst->addSupportedComponent(OFX::ePixelComponentRGBA);
    dst->setSupportsTiles(false);
    // Flame (test 2/3 screenshots): params flow into columns of <= 5 rows, 6 columns per
    // tab; each group starts a column; tabs are named after OFX pages in order. Earlier
    // builds listed every param as a page child and Flame ignored page membership. Here
    // pages list ONLY their groups (allowed by the OFX spec) — Flame still ignored that
    // (2026-10-10). Groups are defined in tab order; tabs come only from overflow, hence the
    // spacer columns below (decision 52).
    OFX::PageParamDescriptor* main = desc.definePageParam("Alpha Gen");
    OFX::PageParamDescriptor* setupPage = desc.definePageParam("Setup");
    OFX::PageParamDescriptor* proto = LTXAG_DEV_BUILD ? desc.definePageParam("Prototype") : nullptr;
    // CVT port (cvt1.05): `open` = unfolded in the Inspector; advanced groups start folded.
    auto group = [&](const char* name, const char* text, OFX::PageParamDescriptor* page, bool open = true) {
      auto* g = desc.defineGroupParam(name);
      g->setLabels(text, text, text);
      g->setOpen(open);
      if (page) page->addChild(*g);
      return g;
    };
    auto place = [](OFX::ParamDescriptor& p, OFX::GroupParamDescriptor* g) { p.setParent(*g); };
    auto label = [&](const char* name, const char* text, const char* def) {
      auto* p = labelled(desc.defineStringParam(name), text, "");
      // CVT Resolve port: Resolve draws a label param with its name, not its value (Status read
      // "Status"), so there it is a greyed-out text field.
      if (hostIsBlackmagic()) {
        p->setStringType(OFX::eStringTypeSingleLine);
        p->setEnabled(false);
      } else {
        p->setStringType(OFX::eStringTypeLabel);
      }
      p->setDefault(def);
      p->setIsPersistant(false);
      p->setEvaluateOnChange(false);
      p->setAnimates(false);  // cvt1.09: no keyframe diamond on a read-only field
      return p;
    };
    // cvt1.09: nothing on this node is keyframed (Process takes the whole clip), so no control
    // shows Fusion's keyframe diamond.
    auto still = [](OFX::ValueParamDescriptor* p) { p->setAnimates(false); };
    auto button = [&](const char* name, const char* text, const char* hint) {
      return labelled(desc.definePushButtonParam(name), text, hint);
    };
    auto perUser = [](OFX::ValueParamDescriptor* p) {
      p->setIsPersistant(false);
      p->setEvaluateOnChange(false);
      p->setAnimates(false);
    };

    // --- Alpha Gen (5 rows) ---
    auto* gMain = group("grpMain", "Alpha Gen", main);
    // cvt1.37: Process and Status first: the rest of this section acts after the matte exists.
    place(*button(kPProcess, "Process",
                  "Generate mattes for the whole clip in the background. Progress and Cancel "
                  "appear in a separate window."),
          gMain);
    place(*label(kPStatus, "Status", "Not processed"), gMain);
    auto* colour = labelled(desc.defineChoiceParam(kPColourMode), "Input Colour",
                            "Auto: Flame converts the source to ACEScg using the clip's colour "
                            "tag, then the model sees it through the ACES 2.0 SDR 100 nits (Rec.709) "
                            "view; Rec.709 video is used as it is. The image output then shows the "
                            "picture with the matte overlay in display space. Bypass: no conversion "
                            "(the source is already display-referred); the image output shows the "
                            "matte too.");
    for (const char* c : kColourModeNames) colour->appendOption(c);
    // Bypass, so setups saved before this param existed load as Bypass (the old default); new
    // nodes are set to Auto in createInstance.
    colour->setDefault(kColourBypass);
    desc.addClipPreferencesSlaveParam(*colour);  // a change re-runs GetClipPreferences
    still(colour);
    place(*colour, gMain);
    // CVT Resolve port: Resolve has no OpenFX colour management, so Auto has nothing to ask for;
    // the param stays defined (saved setups load) but hidden there.
    const bool resolve = hostIsBlackmagic();
    if (resolve) colour->setIsSecret(true);
    auto* output = labelled(desc.defineChoiceParam(kPOutput), "Output",
                            "What the node puts out. Source + Matte Alpha: the picture untouched with "
                            "the matte in the alpha, for a Merge or a mask in Fusion. Matte: the "
                            "matte in RGB and alpha. Overlay: a preview, the whole picture with the "
                            "matte in magenta and a solid alpha, to check it.");
    for (const char* c : kOutputNames) output->appendOption(c);
    output->setDefault(kOutSource);
    desc.addClipPreferencesSlaveParam(*output);
    still(output);
    place(*output, gMain);
    // cvt1.10: premultiply in the node, as Fusion's MatteControl "Post-Multiply Image" does, so
    // a Merge can take the output directly. Render-time only, no reprocessing.
    auto* postMul = labelled(desc.defineBooleanParam(kPPostMultiply), "Post-Multiply Image",
                             "With Output = Source + Matte Alpha: multiply the picture by the "
                             "matte (premultiplied, as Fusion's Merge expects). Off: the picture "
                             "untouched (straight). Matte and Overlay ignore it. No "
                             "reprocessing.");
    postMul->setDefault(true);
    desc.addClipPreferencesSlaveParam(*postMul);  // re-runs GetClipPreferences (premult state)
    still(postMul);
    place(*postMul, gMain);
    // cvt1.26: the model's decoder leaves faint noise in the matte (0.001-0.005 in the
    // background, 0.99-0.999 inside the subject); these clean it up at render time.
    auto* blackClip = labelled(desc.defineDoubleParam(kPBlackClip), "Alpha Black Clip",
                               "Matte values at or below this become 0, clearing the faint haze the "
                               "model leaves in the background. Values between the two clips are "
                               "stretched, so soft edges stay soft. 0: off. No reprocessing.");
    blackClip->setRange(0.0, 0.5);
    blackClip->setDisplayRange(0.0, 0.05);
    blackClip->setDefault(0.005);
    blackClip->setDigits(3);
    blackClip->setIncrement(0.001);
    still(blackClip);
    place(*blackClip, gMain);
    auto* whiteClip = labelled(desc.defineDoubleParam(kPWhiteClip), "Alpha White Clip",
                               "Matte values at or above this become 1, filling the faint speckle "
                               "inside the subject so it is fully solid. 1: off. No reprocessing.");
    whiteClip->setRange(0.5, 1.0);
    whiteClip->setDisplayRange(0.95, 1.0);
    whiteClip->setDefault(0.995);
    whiteClip->setDigits(3);
    whiteClip->setIncrement(0.001);
    still(whiteClip);
    place(*whiteClip, gMain);
    auto* marker = labelled(desc.defineBooleanParam(kPShowMarker), "Show Marker",
                            "Draw a marker (MAKING MAGIC, NOT CONNECTED) on frames without a "
                            "matte. Off: such frames are plain black. A missing matte on a "
                            "processed node always shows MATTE MISSING.");
    marker->setDefault(true);
    marker->setIsSecret(true);  // CVT port (cvt1.05): always on; it explains an empty frame
    still(marker);
    place(*marker, gMain);

    // --- Performance (5 rows) ---
    // CVT port (cvt1.05): hidden. One GPU, one precision (bf16), and the defaults (unload after
    // each job, reserve 5% of free memory) are right for a single Mac; the params stay defined so
    // saved comps load.
    auto* gPerf = group("grpPerf", "Performance", main, false);
    gPerf->setIsSecret(true);
    auto* prec = labelled(desc.defineChoiceParam(kPPrecision), "Precision",
                          "bf16: reference quality. fp8: about half the GPU memory.");
    for (const char* p : kPrecisions) prec->appendOption(p);
    prec->setDefault(d.precision);
    // CVT Resolve port (Apple Silicon only): MPS has no fp8, so bf16 (or Setup's fp16 copy, 68)
    // is the only choice; the param stays defined, hidden.
    if (kMps) prec->setIsSecret(true);
    still(prec);
    place(*prec, gPerf);
    const std::string host = hostShort();
    const std::string unloadHint = "Return all GPU memory to " + host +
                                   " when this node's processing ends. Turn off on all but the last "
                                   "Alpha Gen node to reuse the loaded model.";
    auto* unload = labelled(desc.defineBooleanParam(kPUnload), "Unload GPU", unloadHint.c_str());
    unload->setDefault(d.unload);
    unload->setIsSecret(true);
    still(unload);
    place(*unload, gPerf);
    const std::string headHint = "GPU memory (GB) kept free for " + host + " while processing.";
    auto* head = labelled(desc.defineDoubleParam(kPHeadroom), "VRAM Reserve", headHint.c_str());
    head->setRange(0.0, 256.0);
    head->setDisplayRange(0.0, 32.0);
    head->setDefault(d.headroomGB);
    head->setIsSecret(true);
    still(head);
    place(*head, gPerf);
    auto* saveDef = button(kPSaveDefault, "Save Default",
                           "Use this column's values for every new LTX Alpha Gen node.");
    saveDef->setIsSecret(true);
    place(*saveDef, gPerf);

    // --- Chunking (4 rows; decisions 61, 62) ---
    // CVT port (cvt1.05): with Frame Hold, the advanced quality/speed settings, folded.
    auto* gChunk = group("grpChunk", "Speed and Quality", main, false);
    // cvt1.35: Resolve only: there the yellow render range bars don't limit Process.
    auto* firstF = labelled(desc.defineIntParam(kPFirstFrame), "First Frame (0 = start)",
                            "Optional: process only part of the clip, to try a short section first. "
                            "With First and Last Frame both at 0, the whole clip is processed, and the "
                            "comp does not have to start at frame 0. Use the frame numbers of the page "
                            "you are on (the Fusion page usually starts at 1001, the Edit and Color "
                            "pages at 0). Frames left out show MATTE MISSING. Needs Process.");
    auto* lastF = labelled(desc.defineIntParam(kPLastFrame), "Last Frame (0 = end)",
                           "Optional: the last frame to process, in the frame numbers of the page you "
                           "are on. 0 goes to the end of the clip. Needs Process.");
    for (auto* p : {firstF, lastF}) {
      p->setRange(0, 10000000);
      p->setDisplayRange(0, 2000);
      p->setDefault(0);
      p->setEvaluateOnChange(false);
      if (!hostIsResolve()) p->setIsSecret(true);
      still(p);
      place(*p, gChunk);
    }
    auto* csize = labelled(desc.defineChoiceParam(kPChunkSize), "Chunk Size",
                           "Frames per model run. Auto: the longest that fits the free memory and the "
                           "Mac's size limit: up to 145 frames for small pictures (a Guided ROI "
                           "crop, 720p), 73 at HD. Smaller chunks need less memory but have more seams, "
                           "and the model may pick a different main object in each chunk.");
    for (const char* c : kChunkSizeNames) csize->appendOption(c);
    csize->setDefault(0);
    csize->setEvaluateOnChange(false);
    still(csize);
    place(*csize, gChunk);
    // cvt1.29: the old Chunk Size (with Full), so comps saved with it still load.
    auto* oldSize = labelled(desc.defineChoiceParam(kPLegacyChunkSize), "Chunk Size (old)", "");
    for (const char* c : kLegacyChunkSizeNames) oldSize->appendOption(c);
    oldSize->setDefault(0);
    oldSize->setIsSecret(true);
    still(oldSize);
    place(*oldSize, gChunk);
    auto* merge = labelled(desc.defineChoiceParam(kPMerge), "Merge",
                           "How neighbouring chunks are joined where they overlap. Flow Warp "
                           "(default): where the two mattes disagree (a moving leg), lines their "
                           "edges up with optical flow while dissolving, so the edge slides instead "
                           "of showing twice; a few seconds per job. Dissolve: a plain eased "
                           "cross-dissolve. Needs Process.");
    for (const char* c : kMergeNames) merge->appendOption(c);
    // cvt1.31: Flow Warp is the default (it removed the ghosted leg mid-seam on a fast-running dog).
    merge->setDefault(1);
    merge->setEvaluateOnChange(false);
    still(merge);
    place(*merge, gChunk);
    auto* hold = labelled(desc.defineChoiceParam(kPFrameHold), "Frame Hold",
                          "Removes ghosting on fast-moving objects (a running leg, a whip pan): the "
                          "model packs 8 frames into one step, so fast motion within 8 frames can "
                          "blur or double the matte edges. 2x and 4x repeat each frame so a step "
                          "holds fewer real frames: sharper edges, but processing takes about 2x or "
                          "4x as long. Try 2x first. Needs Process.");
    for (const char* c : kFrameHoldNames) hold->appendOption(c);
    hold->setDefault(0);
    still(hold);
    place(*hold, gChunk);
    // cvt1.26: the old Frame Hold (Off, Auto, 2x, 4x, 8x), so comps saved with it still load.
    auto* oldHold = labelled(desc.defineChoiceParam(kPLegacyFrameHold), "Frame Hold (old)", "");
    for (const char* c : kLegacyFrameHoldNames) oldHold->appendOption(c);
    oldHold->setDefault(0);
    oldHold->setIsSecret(true);
    still(oldHold);
    place(*oldHold, gChunk);
    // cvt1.27: process even when the memory estimate says it won't fit.
    auto* skipMem = labelled(desc.defineBooleanParam(kPSkipMemCheck), "Skip Memory Check",
                             "Process even when the memory estimate says the clip won't fit. The "
                             "estimate is cautious, so it may work; if it doesn't, macOS swaps to "
                             "disk and the job (and the Mac) slows down badly, or the job fails. "
                             "Nothing is damaged. For Macs near the limit, such as a 64 GB M1 Max.");
    skipMem->setDefault(false);
    skipMem->setEvaluateOnChange(false);
    still(skipMem);
    place(*skipMem, gChunk);
    // cvt1.20: where the last Process joined chunks (comp frames), to check them; saved with the
    // comp. "none (one chunk)" for a shot done in one piece.
    auto* seams = label(kPSeams, "Seams", "");
    seams->setIsPersistant(true);
    seams->setHint("The frames where the last Process joined two chunks (comp frame numbers). Check "
                   "moving edges there with Output = Matte. Empty until Process.");
    place(*seams, gChunk);

    // --- ROI (4 rows; decision 63): the matte is the picture's own alpha ---
    // CVT port (cvt1.08): named for Fusion users ("input alpha" = a Magic Mask, roto or garbage
    // matte on the picture), the render-time Multiply first (no reprocessing, the one used
    // most), then Black Out (needs Process), then the margin. Param ids are unchanged.
    auto* gRoi = group("grpRoi", "Guided ROI", main, false)  /* cvt1.33: was Input Alpha Hint */;
    place(*label(kPRoiInfo, "Model Crop", "Alpha = ROI"), gRoi);
    auto* rlimit = labelled(desc.defineBooleanParam(kPRoiLimit), "Multiply by Input Alpha",
                            "Multiply the matte by the alpha coming into the node (a Magic Mask, roto "
                            "or garbage matte), so only your subject is kept. Instant, no "
                            "reprocessing. Grow the input alpha a little (ErodeDilate) so LTX's own "
                            "edge decides the outline. No effect when the input alpha is solid.");
    rlimit->setDefault(true);  // cvt1.08: on for new nodes; nothing changes without an input alpha
    still(rlimit);
    place(*rlimit, gRoi);
    auto* rmask = labelled(desc.defineBooleanParam(kPRoiMask), "Black Out Outside Alpha",
                           "Black out the picture outside the input alpha before the model sees it, "
                           "so it cannot pick anything else. Needs Process again. The edges then "
                           "follow your mask more closely.");
    rmask->setDefault(false);
    still(rmask);
    place(*rmask, gRoi);
    auto* rmargin = labelled(desc.defineIntParam(kPRoiMargin), "Crop Margin (px)",
                             "Pixels of context added around the input alpha's area over the whole "
                             "clip (at least 32). The crop the model sees is also at least 512 "
                             "pixels per side. Raise it if the matte is cut off in a straight line at "
                             "the crop edge. Needs Process again.");
    rmargin->setRange(kRoiMinMargin, 2048);
    rmargin->setDisplayRange(kRoiMinMargin, 256);
    rmargin->setDefault(64);
    still(rmargin);
    place(*rmargin, gRoi);

    // --- Cache (5 rows) ---
    auto* gCache = group("grpCache", "Cache", main, false);
    auto* cdir = labelled(desc.defineStringParam(kPCacheDir), "Cache Folder",
                          "Where generated mattes are cached (about 4 MB per HD frame).");
    cdir->setStringType(OFX::eStringTypeSingleLine);  // cvt1.48: a Choose button replaces Fusion's browse, which only picks files
    cdir->setDefault(homeDir() + "/.cache/ltx-alphagen/mattes");
    perUser(cdir);
    place(*cdir, gCache);
    place(*button(kPChooseCache, "Choose Cache Folder", "Pick the folder in Finder."), gCache);
    place(*label(kPCacheInfo, "Cache Size", ""), gCache);
    // cvt1.14: the size limit, per user (was a fixed 50 GB unless set in the config file).
    auto* climit = labelled(desc.defineDoubleParam(kPCacheLimit), "Cache Limit (GB)",
                            "Largest the matte cache may grow (about 4 MB per HD frame, so 50 GB is "
                            "about 12,000 HD frames). Over it, the least recently used mattes are "
                            "deleted: now when you lower it, and at each start. Mattes unused for "
                            "30 days are deleted too. For you, in every project.");
    climit->setRange(1.0, 100000.0);
    climit->setDisplayRange(5.0, 500.0);
    climit->setDefault(50.0);
    perUser(climit);
    place(*climit, gCache);
    // cvt1.25: Refresh moved here from under Process: the node redraws itself when a job ends,
    // so it is a rarely needed backup, not a step after Process.
    place(*button(kPRefresh, "Refresh",
                  "Rarely needed: the node redraws by itself when a job finishes. If the viewer "
                  "still shows MAKING MAGIC after a job has finished, this makes Resolve render "
                  "the node again to pick up the new mattes. Nothing is reprocessed."),
          gCache);
    place(*button(kPPurgeNode, "Purge Cache", "Delete the cached mattes of this node's clip."),
          gCache);
    place(*button(kPPurgeAll, "Purge All", "Delete every cached matte of every node and project."),
          gCache);

    // --- Setup: licence, models, support, version (CVT port, cvt1.05: one group instead of
    // Motion, Licence, Models and Support; Frame Hold moved to Speed and Quality). Open until
    // Setup is complete, folded after that.
    auto* gLic = group("grpSetup", "Setup", setupPage, !setupState().ready());
    auto* gModels = gLic;
    auto* gSupport = gLic;
    // cvt1.10: brings back the download controls hidden once Setup is complete (to download the
    // models again, use other model files, or read the licence). Shown only after Setup.
    auto* showSetup = labelled(desc.defineBooleanParam(kPShowSetup), "Show Setup Options",
                               "Show the licence, token and download controls again: to download "
                               "the models again, use model files from another folder, or read "
                               "the licence.");
    showSetup->setDefault(false);
    perUser(showSetup);
    if (!hostIsBlackmagic()) showSetup->setIsSecret(true);  // other hosts never hide them
    place(*showSetup, gLic);
    place(*button(kPViewLicence, "View Licence", "Show the LTX-2.x Community License terms."),
          gLic);
    auto* accept = labelled(desc.defineBooleanParam(kPAccept), "I Accept",
                            "I accept the LTX-2.x Community License. Free below USD 10M annual "
                            "revenue; above that a paid Lightricks licence is required.");
    accept->setDefault(false);
    perUser(accept);
    place(*accept, gLic);
    // cvt1.39: the two gated pages, right where the token is needed: each account accepts the
    // terms on both before a token can download.
    place(*button(kPModelPage, "LTX-2.5 Model Page",
                  "Open the LTX-2.5 page on Hugging Face in your browser. Log in and accept the "
                  "terms there before downloading."),
          gLic);
    place(*button(kPLoraPage, "Alpha Gen LoRA Page",
                  "Open the LTX Alpha Gen LoRA page on Hugging Face in your browser. Log in and "
                  "accept the terms there before downloading."),
          gLic);
    place(*button(kPTokenPage, "Token Page",
                  "Open your access tokens page on Hugging Face in your browser. Create a Read "
                  "token there and paste it below."),
          gLic);
    auto* tokCaption = label(kPTokenCaption, "Token", kTokenCaptionDefault);
    // cvt1.09: Flame showed no label beside a text field, hence this caption row; Fusion shows
    // "Hugging Face Token" itself, so there it is hidden.
    if (hostIsBlackmagic()) tokCaption->setIsSecret(true);
    place(*tokCaption, gLic);
    auto* token = labelled(desc.defineStringParam(kPToken), "Hugging Face Token",
                           "Your Hugging Face access token (hf_...). Used for this download only, "
                           "never stored.");
    token->setStringType(OFX::eStringTypeSingleLine);  // cvt1.09: Fusion drew a tall box
    token->setDefault("");
    perUser(token);
    place(*token, gLic);
    place(*button(kPDownload, "Download",
                  "Install the LTX runtime and the models into the Model Folder: reuses verified "
                  "files from Existing Models, downloads the rest with your token."),
          gLic);

    // --- Models (5 rows, per user) ---
    auto* dir = labelled(desc.defineStringParam(kPModelDir), "Model Folder",
                         "Where the models and the LTX runtime are installed (about 80 GB, or about "
                         "125 GB on M1 and M2 Macs, where Setup adds a faster copy of the main "
                         "model).");
    dir->setStringType(OFX::eStringTypeSingleLine);
    dir->setDefault(homeDir() + "/LTX-2.5");  // cvt1.46: one folder for every LTX-2.5 plugin
    perUser(dir);
    place(*dir, gModels);
    place(*button(kPChooseModel, "Choose Model Folder", "Pick the folder in Finder."), gModels);
    auto* ex = labelled(desc.defineStringParam(kPExistingDir), "Existing Models",
                        "Optional: a folder that may already contain the model files (e.g. a ComfyUI "
                        "models folder). Matching files are verified and copied instead of downloaded.");
    ex->setStringType(OFX::eStringTypeSingleLine);
    ex->setDefault("");
    perUser(ex);
    place(*ex, gModels);
    place(*button(kPChooseExisting, "Choose Existing Models", "Pick the folder in Finder."), gModels);
    auto* linkExisting = labelled(desc.defineBooleanParam(kPLinkExisting), "Link Existing Models",
                                  "Link to the files in Existing Models instead of copying them: saves up to 73 GB, but that "
                                  "folder must stay where it is and stay available. Off: they are copied into the Model Folder.");
    linkExisting->setDefault(false);
    perUser(linkExisting);
    place(*linkExisting, gModels);
    place(*label(kPSetupStatus, "Setup", ""), gModels);

    // --- Support (2 rows, per user) ---
    auto* dbg = labelled(desc.defineBooleanParam(kPDebugLog), "Debug Logging",
                         "Verbose logging for troubleshooting (per user).");
    dbg->setDefault(false);
    perUser(dbg);
    place(*dbg, gSupport);
    place(*button(kPCollectLogs, "Collect Logs",
                  "Write a support bundle (logs and system details) to your home folder."),
          gSupport);
    place(*label("version", "Version", LTXAG_VERSION), gSupport);  // cvt1.13: "1.13", the one version
    // CVT port (cvt1.06): credits. The fields are narrow, so each hint carries the full text.
    auto* credit = label(kPCreditPort, "Port by", "Chadwick, Creative Video Tips");
    credit->setHint("Resolve and Fusion Studio port by Chadwick, Creative Video Tips "
                    "(creativevideotips.com).");
    place(*credit, gSupport);
    place(*button(kPWebsite, "creativevideotips.com", "Open creativevideotips.com in your browser."),
          gSupport);
    auto* lic = label("creditLicence", "Licence", "Open source (MPL 2.0)");
    lic->setHint("Open source under the Mozilla Public License 2.0; the source and every change are "
                 "listed in CVT_PORT.md. The LTX model has its own licence (View Licence).");
    place(*lic, gSupport);
    auto* based = label("creditBasedOn", "Based on", "Jan Klier, Logik.tv (Flame)");
    based->setHint("Based on LTX Alpha Gen OFX for Flame by Jan Klier, maintained by Logik.tv "
                   "(github.com/flamelogik/Flame_LTXAlphaGen_OFX).");
    place(*based, gSupport);

    // --- Prototype (dev builds; hidden otherwise) ---
    OFX::GroupParamDescriptor* gProto = proto ? group("grpProto", "Prototype", proto) : nullptr;
    auto* secs = labelled(desc.defineDoubleParam(kPSimSeconds), "Sim Seconds",
                          "Simulated processing time per chunk.");
    secs->setRange(0.0, 3600.0);
    secs->setDisplayRange(0.0, 120.0);
    secs->setDefault(20.0);
    auto* sim = labelled(desc.defineChoiceParam(kPSimulate), "Simulate",
                         "Make the next Process run fail on purpose.");
    for (const char* s : kSimulations) sim->appendOption(s);
    sim->setDefault(0);
    auto* needp = labelled(desc.defineDoubleParam(kPSimNeed), "Sim VRAM GB",
                           "Pretend the run needs this much GPU memory (0 = skip the check).");
    needp->setRange(0.0, 1024.0);
    needp->setDisplayRange(0.0, 128.0);
    needp->setDefault(0.0);
    auto* py = labelled(desc.defineStringParam(kPPython), "Worker Python",
                        "Python used for the worker and helpers.");
    py->setStringType(OFX::eStringTypeFilePath);
    py->setDefault("/usr/bin/python3");
    auto* gate = labelled(desc.defineBooleanParam(kPEnforceGate), "Enforce Gate",
                          "Apply the licence/model gate as a release build would.");
    gate->setDefault(false);
    auto* refresh = labelled(desc.defineBooleanParam(kPAutoRefresh), "Auto Refresh",
                             "EXPERIMENT: refresh the viewer when processing finishes (sets a "
                             "param from a background thread).");
    refresh->setDefault(true);
    auto* dummy = labelled(desc.defineBooleanParam(kPUseDummy), "Use Dummy",
                           "Use the dummy worker (luminance, no GPU) instead of the real model. "
                           "Sim Seconds and Simulate only apply to the dummy.");
    dummy->setDefault(false);
    for (OFX::ValueParamDescriptor* p :
         std::vector<OFX::ValueParamDescriptor*>{secs, sim, needp, py, gate, refresh, dummy}) {
      p->setEvaluateOnChange(false);
      if (gProto)
        place(*p, gProto);
      else
        p->setIsSecret(true);
    }

    // --- Hidden ---
    auto* legacy = desc.defineChoiceParam(kPColour);  // the old Input Colour (saved setups load)
    for (const char* c : kLegacyColourSpaces) legacy->appendOption(c);
    legacy->setDefault(0);
    legacy->setIsSecret(true);
    legacy->setEvaluateOnChange(false);
    auto* roist = desc.defineStringParam(kPRoiState);
    roist->setDefault("");
    roist->setIsSecret(true);
    roist->setEvaluateOnChange(false);
    auto* canv = desc.defineStringParam(kPCanvas);  // CVT port (cvt1.07), saved with the comp
    canv->setDefault("");
    canv->setIsSecret(true);
    canv->setEvaluateOnChange(false);
    auto* init = desc.defineBooleanParam(kPInitialized);
    init->setDefault(false);
    init->setIsSecret(true);
    init->setEvaluateOnChange(false);
    auto* nid = desc.defineStringParam(kPNodeId);
    nid->setDefault("");
    nid->setIsSecret(true);
    nid->setEvaluateOnChange(false);
    auto* tick = desc.defineIntParam(kPRefreshTick);
    tick->setDefault(0);
    tick->setIsSecret(true);
    tick->setIsPersistant(false);
    tick->setCanUndo(false);
    tick->setEvaluateOnChange(true);  // the point: a change makes Flame re-render
  }

  OFX::ImageEffect* createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) override {
    return new AlphaGenEffect(handle);
  }
};

}  // namespace

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray& ids) {
  static AlphaGenFactory factory;
  ids.push_back(&factory);
}
}  // namespace Plugin
}  // namespace OFX
