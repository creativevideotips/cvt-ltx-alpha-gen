# CVT Resolve port: what changed from upstream

Upstream: <https://github.com/flamelogik/Flame_LTXAlphaGen_OFX>, commit `e9ab353`
(2026-10-27), MPL 2.0. This folder is that source with the changes below; the
modified files stay under MPL 2.0 (`LICENSE`), the vendored OpenFX under BSD.
Code changes are marked `CVT Resolve port` in comments. The upstream design
notes (moved to `upstream-notes/`) are kept as reference; where they talk
about Flame, Linux or NVIDIA, that part does not apply here.

## Mac only

- Apple Silicon only. Removed: the Linux platform layer
  (`platform_linux*.cpp`, NVML), `tools/nvml_probe.cpp`, the Linux symbol
  list, the remote, Linux, packaging and GitHub release scripts, the Flame
  installer (`packaging/`) and the Flame Python hook (`flame_hook/`, its test).
  CMake stops on anything but macOS. The CUDA branches inside the Python
  worker and Setup are left as they are: they never run on a Mac.
- Precision is hidden: MPS has no fp8, so bf16 (or Setup's fp16 copy) is the
  only choice.

## For Resolve

- Plugin id `com.creativevideotips.ltx.alphagen`, menu group `CVT`
  (OpenFX > CVT > LTX Alpha Gen), so it never clashes with the Flame build.
- New **Output** choice (`outputMode`; the alpha is always the matte):
  Source + Matte Alpha (default: the picture untouched), Matte (matte in RGB
  too), Overlay (the upstream magenta preview). The output clip asks for RGBA.
- New **Refresh** button: bumps the hidden refresh tick, so the host renders
  again and picks up finished mattes. The watcher's tick now always rises
  (it restarted at 1 per job, so a second job did not change it).
- Input Colour: hidden in Resolve (no OpenFX colour management there); new
  nodes start on Bypass. Elsewhere unchanged.
- Messages and hints name the host ("Resolve") instead of "Flame".
- System test: the node helper sets Output = Overlay for the upstream image
  checks; `process_chunks_merge_purge` also checks Source + Matte Alpha and
  Matte.

## Changelog

- cvt1.00: first port (above).
- cvt1.01: Process no longer reads the clip twice. The ROI scan runs only if
  one of 5 sampled frames has a varying alpha (a Fusion MediaIn never does).
  The read shows the host's progress bar with Cancel (where the host has one)
  and logs the frame count and ms per frame. In Resolve, Process had blocked
  the UI (beach ball) while it read every frame twice.
- cvt1.02: Process reads only the comp's frame range (the effect's timeline
  bounds). In Fusion the Source clip reported a range far past the comp (the
  whole media), so Process read for minutes and then failed with "did not
  provide source frame". It also fetches the last frame first, so a range the
  host can't deliver fails at once, and logs the three ranges.
- cvt1.03: the range lookups are guarded one by one (Resolve threw for the
  output clip's frame range, so cvt1.02's Process ended at once with no
  message). Errors now also show in the Status field, and a failed Process
  says so there.
- cvt1.04: in Resolve the memory warning can't ask Yes/No and persistent
  messages are refused (that threw and ended Process), so a clip that may not
  fit now stops with a message in Status (scale down, ROI, smaller Chunk
  Size). The dummy worker skips the memory check unless Sim VRAM GB is set.
  Read-only fields (Status, ROI, Cache Size, Setup, Token, Version) are
  greyed-out text fields in Resolve, which drew label params with their name
  instead of their value. Output and Refresh follow the setup gate.
- cvt1.05: Fusion Studio is treated like Resolve (same OFX host: Status and
  the other read-only fields show their values, Input Colour hidden). A
  simpler Inspector: Alpha Gen open (Output, Process, Status, Refresh);
  Speed and Quality (Chunk Size, Merge, Frame Hold), Region of Interest and
  Cache folded; Licence, Models, Support and Version in one Setup group,
  folded once Setup is done; Show Marker, Precision, Unload GPU, VRAM Reserve
  and Save Default hidden with their defaults. The memory estimate has an
  M5-and-newer fit from two M5 Max runs (HD 33 frames 43.1 GB, HD 73 frames
  82.2 GB): the M1 fit was 18 GB high at 33 frames and 1.5 GB low at 73.
  Older chips keep the M1 fit. build.sh makes the release build by default
  (--dev for the Prototype test section); the system test uses build-dev/.
- cvt1.06: credits in the Setup section: Port by Chadwick, Creative Video Tips, a
  creativevideotips.com button (opens the site), Licence: Open source
  (MPL 2.0), Based on: LTX Alpha Gen for Flame by Jan Klier (Logik.tv). NOTICE
  names the port.
- cvt1.07: frames that differ in size work. Fusion trims each image to its
  Domain of Definition, so after a Crop, a Magic Mask or a garbage matte the
  frames are smaller than the format and change per frame; Process stopped
  with "Source frames differ in size within the clip". Process now builds one
  canvas, the union of every frame's region of definition (asked without
  rendering), places each frame on it (transparent black around it), and
  saves it (hidden canvasState) so render puts the matte back in place. Nodes
  processed earlier keep working (no canvas saved: the frame's own bounds).
- cvt1.08: the ROI section is "Input Alpha Hint", in this order: Model Crop
  (was ROI), Multiply by Input Alpha (was Multiply Result by ROI; now on for
  new nodes, no effect without an input alpha), Black Out Outside Alpha (was
  Black Out Outside ROI), Crop Margin (was ROI Margin). Param ids unchanged.
- cvt1.09: Inspector cleanup in Resolve and Fusion: HF Token is a one-line
  field (Fusion drew a tall box), the "Hugging Face token:" caption row (a
  Flame workaround) is hidden, no control can be keyframed (no diamonds; the
  node always processes the whole clip), and once Setup is complete View
  Licence, I Accept, HF Token, Download and Existing Models are hidden (they
  return if the licence or models stop being ready).
- cvt1.10: a Show Setup Options checkbox at the top of Setup (Resolve and
  Fusion, once Setup is complete) brings back View Licence, I Accept, HF
  Token, Download and Existing Models, to download the models again, use
  other model files or read the licence. Not saved with the comp.
  Also: the system test no longer pops up error dialogs on the user's screen
  (it leaves ~/.ltxag-no-dialogs in its own test HOME, and oslayer.message
  skips dialogs there), and the dummy's simulated out-of-memory says "GPU",
  not "CUDA". The CUDA branches left in the worker and Setup never run on a Mac.
  Post-Multiply Image (under Output, on for new nodes): with Source + Matte
  Alpha the picture is multiplied by the matte, as Fusion's MatteControl
  does, so a Merge takes it directly; the output clip's premultiplication
  state is reported to the host. Render-time only.
- cvt1.11: Purge Cache read the clip's whole media range, like Process before cvt1.03
  (minutes of beach ball in Resolve); it now uses the same comp range as
  Process (compRange) and also clears the saved canvas.
- cvt1.12: one version number. The plugin version hosts store with comps is
  the port's own (1.12; upstream's 1.0.0 is the CMake fallback only), and the
  Version field shows "cvt1.12". The major version stays 1, so saved comps
  keep loading. Leading zeros are dropped for OFX (1.08 -> 1.8).
- cvt1.13: the Version field shows just the number ("1.13").
- cvt1.14: Cache Limit (GB) in the Cache section, per user (cache_cap_gb,
  default 50). Lowering it trims the cache at once (least recently used
  first); Cache Size reads "0.6 of 50 GB".
- cvt1.15: the progress window estimates the time left from the start. Each
  real job records this Mac's speed (seconds per step per megapixel-frame,
  and the time before the first step and after the last, in steps) in
  ~/.config/ltx-alphagen/timing.json, blended 60 % new; the next job uses it
  until its own step 2. The dummy worker's speed is not used or saved.
- cvt1.16: the progress window's Cancel button never takes keyboard focus,
  so Space (play in Resolve and Fusion) can't press Cancel after the window
  was clicked.
- cvt1.17: the Python side is Mac only too. The worker's GPU class is MPS
  only (no CUDA branches, no CUDA allocator setting) and has no fp8 path;
  the OS layer has no Linux windows (zenity) or /proc; Setup has no CUDA smoke
  test or gcc check (the uv install command is unchanged: its Linux-only
  extra installs nothing on a Mac). The OS-layer test that opens a real
  window is opt-in (LTXAG_GUI_TESTS=1), so test runs show no window. Checked
  with the real model: worker_smoke 768x512x17 PASS (mean error 0.0054).
- cvt1.18: mattes are found by node, frame and settings, and a 64x36
  thumbnail of the source frame (<key>.thumb beside each matte) decides
  whether the picture is still the one processed: mean difference <= 0.02 and
  no cell over 0.25 (decoder noise passes; a +10 % gain or a moved subject
  does not). Upstream's exact pixel hash broke on long-GOP MP4s: Resolve
  decoded some frames slightly differently while Process read the clip than
  on later redraws, so those frames showed MATTE MISSING. Up to 4 slots per
  frame, so copies of a node (which share its id) on other clips keep their
  own mattes. Mattes made before cvt1.18 are still found by the old pixel
  key. Purge Cache, Purge All and the cleanup remove thumbnails with their
  mattes. A changed picture says so in Status.
- cvt1.19: the Merge choice "Smooth" is called "Dissolve" (same behaviour;
  setups store the index), and its hint says what each does. Resolve's Speed
  Warp can't be used for Flow Warp: plugins can't call Resolve's tools, the
  merge runs in the background job, and Fusion Studio has no Speed Warp.
- cvt1.20: a read-only Seams field in Speed and Quality lists where the last
  Process joined chunks, in comp frames ("1019-1033, 1037-1051", or "none (one
  chunk)"), saved with the comp and cleared by Purge Cache, so a seam can be
  checked, and checked again after changing Merge or Chunk Size.
- cvt1.21: the marker on frames that are still processing is much stronger (text
  100%, stripes 35%, was 30% and 8%) and says MAKING MAGIC instead of PROCESSING.
- cvt1.22: the marker has no diagonal stripes (they looked like a bug) and its letters
  are bolder (each dot a third wider and taller; same size and spacing).
- cvt1.23: Output = Overlay has a solid alpha, so the whole picture shows behind the
  magenta matte (the alpha used to be the matte, which cut the background away), and
  the marker is drawn in white over the picture there.
- cvt1.24: Fusion's Reset button starts the node fresh: this clip's mattes are removed
  and the node goes back to Not processed, as Purge Cache does (Reset used to restore
  only the visible settings, so the old mattes kept showing).
- cvt1.25: Refresh moved from under Process to the Cache section, with a hint saying
  it is rarely needed (the node redraws by itself when a job finishes).
- cvt1.26: Frame Hold is Off, 2x or 4x (param frameHoldMode). Auto and 8x are
  gone: Auto's plan (chosen for the 4x worst case, held at 12 px of motion) cost
  50% more on a mostly still clip (5 chunks for 63 frames), and its time could not be
  predicted; 8x was barely better than 4x. The old frameHold param stays defined,
  hidden and ignored, so saved comps load (at Off). The Auto code (motion.h,
  planAutoChunks and their unit tests) stays, unused; the auto_hold system test
  is removed. New Alpha Black Clip (0.005) and Alpha White Clip (0.995) under
  Post-Multiply Image: render time, before Multiply by Input Alpha, on real
  mattes only (clipMatte in marker.h); they clear the decoder's faint noise
  (0.001-0.005 in the background, 0.99-0.999 inside), stretching the values
  between so soft edges stay soft.
- cvt1.27: Skip Memory Check (Speed and Quality, off by default): Process runs
  even when the memory estimate says the clip won't fit, for Macs near the
  limit. A 64 GB M1 Max had 38.6 GB available in Resolve, 39.3 in Fusion
  Studio, against the M1 fit's 46.3 GB for a 420x432 12-frame clip (the 42 GB
  model is the floor). The job is logged as warned in vram_stats.csv, so its
  real peak can refit the M1 estimate. The Too large message names the option.
- cvt1.28: memory estimate refit for Apple Silicon (vram_estimate.h): one fit,
  max(40.8, 37.0 + 0.10 GB * Mpx * frames) + 1 GB, for the M5 and newer (bf16)
  and an M1-class Mac on the fp16 copy, from peak reserved MPS memory of 38 M5
  Max jobs and 3 M1 Max (64 GB) jobs. The cvt1.05 M5 fit was 30-40 GB high on
  long chunks (HD 73: 83.7 estimated, 50.2 used), so Auto fell back to 33-frame
  chunks in Resolve; the M1 fit refused every job on a 64 GB M1 Max (46.3 GB
  for 448x448, 33.7 used). The M1 Ultra bf16 fit stays for M1-class Macs without
  the fp16 copy. New Apple Silicon size limit: about 170 Mpx-frames per chunk
  (MPSGraph refuses tensors over INT_MAX elements; HD 145 failed after 9 s in
  the VAE encode, HD 81 and UHD 17 ran). Auto skips such chunk sizes; a fixed
  one stops in Status, even with Skip Memory Check.
- cvt1.29: Chunk Size is Auto, Half (73) or Quarter (33) (param chunkSizeMode);
  Full (145) is no longer offered, as HD is over the Apple Silicon size limit at
  145 frames. Auto still uses 145 when the picture is small enough. The old
  chunkSize param stays hidden and ignored (saved comps load at Auto). The job
  log has a line per seam: frames (comp numbers; Process sends first_frame), how
  much the two chunks' mattes differ (largest pixel difference, and the worst
  frame's mean), and with Flow Warp how many frames morph.py warped and the
  largest edge movement in pixels (morph() takes an optional stats dict).
- cvt1.30: Flow Warp acts only where it can help: the result is the dissolve,
  moved towards the warped matte inside a feathered mask of where the two
  chunks' mattes differ by more than 0.05 (grown by 2% of the long edge), and
  by sin(pi t), full in the middle of the seam and none at its ends. On a
  fast-running dog (HD, 3 Quarter chunks) Flow Warp was better than Dissolve
  mid-seam (frame 1032) but worse near the end (1039): the coarse flow (512 px)
  bent and resampled fur both mattes agreed on. The seam log line adds the
  share of the frame the warp changed. Dissolve stays the default.
- cvt1.31: Flow Warp is the default Merge, and both modes use the eased
  (smoothstep) seam curve. On the dog shot Flow Warp removed the ghosted back
  leg mid-seam (frames 1023, 1024, 1031, 1032) but lost near a seam end (1039):
  its linear curve kept ~12% of the other chunk there (Dissolve ~4%) and a
  ~38% warp; eased, that is ~4% and ~13%. Chunk 1 came out identical between
  two runs and chunk 3 differed (the upstream determinism note), so 1039 and
  1048 compare model runs as much as merges. The system test now checks
  Dissolve, Flow Warp being what the earlier runs use.
- cvt1.32: mattes repeat between runs more closely. Found by processing the same
  HD frames as identical chunks (LTXAG_HASH_STAGES=1 logs a fingerprint of each
  chunk's reference latent, decoder input and matte). The fix that matters for
  plugin jobs: the reference encode's dtype was the faster of bf16 and fp32 by a
  timed probe, a coin flip on an M5 Max (1.5-2.2 ms each) in every new worker,
  and in bf16 MPS gave different reference latents for the same HD frames anyway.
  Now always fp32 (the M1 had it already). A safeguard: the DiffVAE decode chose
  its tiles from the free memory (12 layouts between 20 and 68 GB at HD); now a
  fixed budget per Mac, half the Metal working set (53.8 GB on the M5 Max), or the
  real figure, logged, when less is free. Corrected in cvt1.33: this first said
  the tile budget was a main cause, from a test with the transformer resident
  (~70 GB free, on the 68 GB step). Plugin jobs rebuild the transformer per chunk
  and see ~105 GB free, so it would rarely have changed their tiles; the fixed
  budget does give them smaller tiles than before. Speed, one run each, 33 HD
  frames, M5 Max: 226.7 s bf16 with the large budget (as cvt1.31), 236.0 s fp32,
  240.3 s fp32 with the fixed budget: 6 % in all, kept for repeatability. What
  was measured: chunk 1 bit-identical between runs; two 3-chunk runs bit-identical
  only with the transformer resident and fingerprints on. Still open: chunks after
  the first sometimes come out in one of two versions (mean difference 0.0018,
  ~0.3 % of HD pixels, along edges), with the transformer resident or rebuilt, and
  with or without an emptied MPS cache; cause unknown, possibly MPS itself. Frame
  Hold's hint leads with what it is for: removing ghosting on fast-moving objects.
- cvt1.33: Process is ignored while the node's job is starting or running (a
  double click ran two jobs; the second to reach the worker unloaded it, and the
  other failed with "the worker process ended unexpectedly"). A job waiting for
  a worker that then unloads (Process on a second node) now starts a new worker
  instead of failing. In Fusion and Resolve the viewer shows the MATTE MISSING,
  PICTURE CHANGED, FAILED or CANCELLED marker: a failed render's picture was thrown
  away (red node, frozen viewer), and persistent messages are refused. Final
  renders still fail, and Fusion stops at that frame ("Render did not complete",
  tested by Chadwick). beginSequenceRender's isInteractive tells them apart;
  render()'s interactive flag is 0 for both. The Input Alpha Hint section is now
  Guided ROI (only its label; param names are unchanged). The Installer has a Read
  Me screen with the build's date. First 145-frame chunks on a Mac: a 512x576
  crop, 42.3 GB estimated, 40.8 GB peak.
- cvt1.34: tried to stop a final render as it begins if a frame of its range has
  no matte. Tested by Chadwick in Fusion Studio: it doesn't work. The check found
  the frame (1015) and failed the action, and Fusion carried on: it rendered 1005
  to 1020, saving 1015-1020, which the viewer had already drawn, with the marker
  (so the cache explanation is confirmed), and stopped at 1021, the first frame it
  had to draw. The failed action does nothing, but the log line naming the first
  missing frame is useful. Viewer or final now goes by isInteractive alone.
- cvt1.35: First Frame and Last Frame in Speed and Quality, Resolve only (hidden and
  ignored in Fusion Studio). Resolve's Fusion page doesn't pass on the render range
  (bars at 1043-1053, plugin told 1001-1101), and a plugin has no other way to read
  it. Fusion Studio does pass it on. Process only: Purge Cache still takes the whole
  comp. 0 leaves that end of the range as it is.
- cvt1.36: First Frame and Last Frame labels say "(0 = start)" and "(0 = end)", and the
  tooltip says 0 on both is the whole clip, the comp need not start at 0, and the numbers
  are the page's own (Chadwick: 0 and 0 could read as "nothing" or as "frame zero"). The
  effects category is Creative Video Tips (was CVT). No more persistent-message calls in
  Fusion and Resolve, which logged a warning on every frame. The watcher's status change
  is in an edit block in Resolve, trying to make the Edit and Color pages redraw after a
  job; whether it helps is untested. Tested on cvt1.35: Edit page, frames from 0 (edit
  frames, a 5x clip's 11 frames = every 5th source frame), First/Last worked. Color page
  the same; a second draw, probably thumbnails, logs PICTURE CHANGED and fails harmlessly,
  graying the node thumbnail; a Deliver there completes.
- cvt1.37: the effect has the CVT logo, white, 128x128 (ofx/resources/<plugin id>.png, the
  OFX bundle's icon, as Neat Video has). Process and Status are first in the Alpha Gen
  section, as the rest of it acts after the matte exists. The cvt1.36 edit block is gone:
  tested on the Edit page, it did nothing (Status and viewer still wait for the next
  interaction). Tested on cvt1.36: on a fast walker (shot 310, 16 frames), Frame Hold 2x
  gave a much more solid torso than Off (Chadwick), though still not fully solid.
- cvt1.38: on Resolve's Edit and Color pages a frame with no matte draws the marker
  (MATTE MISSING, PICTURE CHANGED ...) instead of failing. Tested on cvt1.37: on the Edit
  page a failed frame showed the plain picture, and a Deliver reported Complete with plain
  footage and no warning. Those pages give the output clip a frame range, the Fusion page
  and Fusion Studio don't, which tells them apart. On the Fusion page a final render still
  stops. Tested: Guided ROI on the Fusion page (ellipse alpha, crop 1216x1080, 83 s against
  129 s for 16 frames); shot 20's seams show no jump larger than normal frame changes
  (largest 0.0119 in a seam, 0.0125 outside), with a faint gray edge in the first seam
  that Chadwick judged good enough.
- cvt1.39: Setup has LTX-2.5 Model Page and Alpha Gen LoRA Page buttons that open the two
  Hugging Face pages in the browser, placed above the token and hidden with the other
  download controls once Setup is complete. The token field is now labelled Hugging Face
  Token, not HF Token (Chadwick: nobody will know what HF is).
- cvt1.40: Setup also has a Token Page button that opens the Hugging Face access tokens page,
  just above the token field.
- cvt1.41: the disk space notes and the Model Folder tooltip now allow about 125 GB on M2 Macs
  as well as M1. Measured: the Model Folder on an M2 Max is 109 GiB (about 117 GB), the same as
  an M1 Max, against 70 GiB on an M5 Max, so Setup made the fp16 copy there; the old note
  expected no copy on M2 to M4. M3 and M4 are not measured.
- cvt1.42: Download checks for Apple's Command Line Tools first and, if they are missing, says in
  Status what to run and keeps the token. Before, the helper's own check could never report it,
  because the system python3 that runs the helper comes from those tools. It looks on disk, since
  running that python3 or xcrun can open Apple's install window. Tested only by the system test's
  simulated failure, not on a Mac without the tools. Installer Read Me: new tagline, the models are
  no longer called open-source, the tools and the tested hosts are listed.
- cvt1.43: security review before publishing. Release builds ignore the Worker Python setting, which
  is saved with a comp, so a comp from someone else can't choose the program that Process or Download
  starts. Setup and jobs run with umask 022, and the settings, thumbnails and job manifests the plugin writes inside Resolve get fixed modes: Resolve's umask
  is 000, so the Python runtime, the settings and the cache came out writable by anyone on the Mac.
  The Hugging Face token no longer follows a redirect to another host. The node's logo no longer
  carries file metadata (EXIF and XMP held the Mac's user name).
- cvt1.44: the installer package no longer contains compiled Python caches, which carried the path
  of the Mac that built it. No change to the plugin itself.
- cvt1.45: the installer refuses to install on a Mac older than macOS 15 (the plugin is built for macOS 15,
  and macOS won't load it on an older system, so before this it installed and then simply didn't appear),
  and the README, guide and installer Read Me now say macOS 15 or later. No change to the plugin itself.

- cvt1.46: the Model Folder now starts at `~/LTX-2.5`, one folder for every LTX-2.5 plugin, and
  a new user who set up LTX Inpaint Still first keeps that folder. Anyone already set up keeps theirs. Setup's speed test
  for the extra 42 GB copy now needs 1.3x, not 1.2x, and doesn't decide again once it has decided, because an M5 Max sits right
  at 1.2x and could get the copy on one run and not the next.
- cvt1.47: new Link Existing Models checkbox. Fusion never showed the question that used to ask, so Download always copied.
  The token's length is no longer logged.
- cvt1.48: the folder fields have Choose buttons that open the Mac's Finder folder picker, in place of Fusion's browse
  control, which only picks files.
- cvt1.49: a Model Folder that was set up and has since been deleted is no longer offered from the saved settings: the field goes
  back to `~/LTX-2.5`. A folder on a drive that isn't mounted stays, since the drive may come back.
