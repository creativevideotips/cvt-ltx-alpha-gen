# LTX Alpha Gen for DaVinci Resolve

**Local, stable AI roto for DaVinci Resolve. Free, on your Mac.** An OFX plugin
for DaVinci Resolve and Fusion Studio that makes an alpha matte for a clip with
Lightricks' LTX-2.5 Alpha Gen model.

From [Creative Video Tips](https://creativevideotips.com), ported by Chadwick
from [LTX Alpha Gen OFX for Flame](https://github.com/flamelogik/Flame_LTXAlphaGen_OFX)
by Jan Klier, maintained by [Logik.tv](https://logik.tv). This is an independent
port, not made or endorsed by Lightricks or Blackmagic Design.

## Download

### [Download the installer: LTXAlphaGen-1.45.pkg](https://github.com/creativevideotips/cvt-ltx-alpha-gen/releases/download/v1.45/LTXAlphaGen-1.45.pkg)

### [Download the guide (PDF)](https://github.com/creativevideotips/cvt-ltx-alpha-gen/releases/download/v1.45/LTX_Alpha_Gen_Guide.pdf)

These links download the files straight away. You won't find the installer in the list of code files above: GitHub keeps it on the [Releases page](https://github.com/creativevideotips/cvt-ltx-alpha-gen/releases/latest), under **Assets**. The installer is only the plugin, and you also need to [download the models](#setup-download-the-models).

![The first, middle and last matte of a shot of a circular saw and an arm](fusion_studio_first_mattes.png)


## Is it for you?

- **Try Magic Mask first.** Resolve's Magic Mask is far faster, so use it first.
  When its matte isn't good enough, use LTX Alpha Gen for the more precise work.
- **Mac with Apple Silicon only.** It runs the model on the Mac's GPU. There is
  no Windows, Linux or NVIDIA version.
- **macOS 15 (Sequoia) or later.** The plugin is built for macOS 15, and the installer
  will not install on an older macOS.
- **Tested with Resolve Studio and Fusion Studio.** It has not yet been tested in the
  free version of Resolve. Magic Mask is Studio-only, but any roto or garbage matte
  works for [Guided ROI](#choosing-the-subject-guided-roi).
- **128 GB of RAM recommended.** 64 GB can work for small pictures (a Guided ROI
  crop or a low resolution) and short clips, in Fusion Studio with nothing else
  open, but expect it to be slow on older chips. The model alone holds about
  41 GB of the GPU's memory, however small the clip.
- **It takes time.** Expect several minutes for a short HD clip on a fast Mac
  ([measured speeds](#size-and-speed)). The GPU stays busy, so playback is slow
  while a job runs.
- **It needs disk space.** About 80 GB for the models, or about 125 GB on some
  Macs ([which Macs](#disk-space)).
- **It needs Apple's Command Line Tools,** a free download from Apple
  ([Setup](#setup-download-the-models) covers it). You don't need to install Python:
  Setup brings its own.

## Install

1. [Download the installer](#download) from the top of this page.
2. Quit Resolve and Fusion Studio, then double-click the pkg. It is signed and
   notarized, asks for your admin password, and installs into
   `/Library/OFX/Plugins`.
3. Start Resolve. The node is in the Fusion page under Effects > OpenFX >
   Creative Video Tips > LTX Alpha Gen.

## Setup: download the models

The installer is only the plugin. The models are a separate download from
Hugging Face, and Lightricks asks each person to accept their terms, so you need
a free Hugging Face account.

First, install Apple's Command Line Tools if you don't have them. In Terminal, run
this and follow the window it opens. If it says they are already installed, carry on.

```bash
xcode-select --install
```

If you skip this, Download stops and Status tells you to run it.

The rest happens in the node's **Setup** section.

1. Press **View Licence**, then tick **I Accept**.
2. Press **LTX-2.5 Model Page** and **Alpha Gen LoRA Page**. Each one opens its
   page in your browser. Log in or sign up, and accept the terms in the box near
   the top. Access is granted at once. Do both pages: if you miss one, Download
   stops with "Hugging Face refused the download (HTTP 403)".
   - https://huggingface.co/Lightricks/LTX-2.5
   - https://huggingface.co/Lightricks/LTX-2.5-22b-IC-LoRA-Alpha-Gen
3. Press **Token Page**, or go to https://huggingface.co/settings/tokens. Choose
   **Create new token**, type **Read**. The name is only a label for you. Copy the
   token (it starts with `hf_`).
4. Paste it into **Hugging Face Token** and press Return.
5. Check the **Model Folder** (by default `~/ltx-alphagen-models`). If you already
   have some of the model files, for example in a ComfyUI folder, set **Existing
   Models** to that folder: matching files are checked and copied instead of
   downloaded.
6. Press **Download**. A window shows the progress. It installs its own Python
   and everything the model needs into the Model Folder, separate from anything
   else on your Mac, then downloads the models and checks every file. Keep every file in the folder,
   including the `.verified` ones.

**About the token:** it is used for this download only and is never stored. The
field clears as soon as Download starts. Keep it private, because with it someone
can download anything your account can. Once the models are on every Mac, you can
revoke it on the same page. If you record your screen, blur it or use a throwaway
token.

Once Setup is complete, these controls hide themselves. Tick **Show Setup Options**
to bring them back.

### Disk space

Every Mac downloads the same files, about 75 GB. At the end of Setup the plugin
times the model's core maths in two number formats. If fp16 is at least 1.2x faster
than bf16 on your Mac, it also makes an fp16 copy of the 42 GB main model and uses
that, so older chips run faster.

| Mac | Extra copy? | Folder size | Free space to allow |
|---|---|---|---|
| M1 family | yes, about 2x faster (measured on an M1 Max) | about 117 GB | about 125 GB |
| M2 family | yes (folder size measured on an M2 Max) | about 117 GB | about 125 GB |
| M3, M4 families | not measured yet | about 75 or 117 GB | about 125 GB to be safe |
| M5 family | no, same speed (measured on an M5 Max) | about 75 GB | about 80 GB |

If the disk fills up during the copy, Setup skips it and the plugin still runs,
just more slowly.

## Using it

Work on the Fusion page or in Fusion Studio, in HD:

```
MediaIn1 (or Loader) → Resize 1920×1080 → LTX Alpha Gen → Bitmap (Channel: Alpha) → mask
```

1. Press **Process**. The host pauses for a couple of seconds while it hands over
   the frames, then the job runs on its own with a progress window and Cancel.
   Status follows it, and the viewer updates when it is done.
2. Use the matte. A **Bitmap** node (Channel: Alpha) turns it into a mask, and masks
   scale to whatever they are applied to, so it works on the full-resolution picture:
   - **Grade the subject:** the Bitmap into a ColorCorrector's effect mask on the
     original MediaIn.
   - **Cut it out:** a MatteControl with the original as Background and the Bitmap
     in Garbage Matte, with Invert on, then a Merge over a new background.

### Feed it Rec.709

The model expects normal Rec.709 video, and the plugin passes on the pixels as they
arrive. Log footage still gives a good matte, but in a side-by-side test on one shot,
the matte from a Rec.709 picture was better. So convert before the node, and use its
matte on your untouched footage: source > Color Space Transform (log to Rec.709) >
LTX Alpha Gen, then take the matte to the original branch.

## Choosing the subject: Guided ROI

There is no text prompt: the model picks the main subject of what it sees. To choose
the subject, give the picture coming in an alpha, such as a loose Magic Mask, roto or
garbage matte. It does two things:

- **Where the model looks.** The model only sees a crop around the area the alpha
  covers over the whole clip, plus a margin. That is much faster, and better for small
  subjects. A mask that sweeps across half the frame makes a big crop, and the model
  may keep more than you want inside it.
- **What you keep.** **Multiply by Input Alpha** (on by default) trims the matte to
  your mask, instantly.

The setup that works best keeps the real picture inside the crop, so the model makes
the edges itself:

```
Loader1 ───────────────────────────────────┐ Background
   └→ MagicMask1 → ErodeDilate → Foreground ┴→ MatteControl1 → LTX Alpha Gen
```

1. Magic Mask the subject. An **ErodeDilate** grown a few pixels puts the mask's edge
   just outside the subject, so LTX's edge decides the outline.
2. **MatteControl:** Background = the original picture, Foreground = the mask.
   Matte: Combine **Combine Alpha**, Combine Op **Copy**, **Post-Multiply Image off**.
3. Press Process. **Model Crop** shows the crop size, for example "Crop 1280x1024".

A small subject in a UHD shot can be processed at full resolution this way, without
the Resize, as long as the crop is no bigger than about an HD frame in area.

## The controls

**Alpha Gen**

| Control | What it does |
|---|---|
| Process | Makes mattes for every frame of the comp, or the render range in Fusion Studio. |
| Status | What the node is doing, or why it stopped. |
| Output | **Source + Matte Alpha** (default): the picture with the matte in its alpha. **Matte**: the matte in RGB and alpha. **Overlay**: the picture with the matte in magenta, to check it. |
| Post-Multiply Image | On (default): the picture is multiplied by the matte, ready for a Merge. Off: the picture untouched. |
| Alpha Black Clip | Matte values at or below it become 0 (default 0.005). This clears the faint haze the model leaves in the background. |
| Alpha White Clip | Matte values at or above it become 1 (default 0.995), so the inside of the subject is solid. Soft edges stay soft. |

None of these except Process needs reprocessing.

**Speed and Quality** (the defaults are fine)

| Control | What it does |
|---|---|
| First Frame, Last Frame | Resolve only: process just these frames, to try a short section first. 0 means from the start or to the end. Resolve's yellow render range bars don't limit Process; in Fusion Studio they do, so it doesn't have these. |
| Chunk Size | **Auto** (default; leave it there), **Half (73)** or **Quarter (33)** frames per model run. Auto picks the longest that fits your memory: up to 145 frames for small pictures, 73 at HD. Shorter chunks need less memory, but join at more seams. |
| Merge | How chunks join where they overlap. **Flow Warp** (default) lines up moving edges with optical flow while dissolving, so they slide instead of doubling. **Dissolve** is a plain cross-dissolve; try it if Flow Warp ever smears an edge. |
| Seams | The frames where the last Process joined two chunks. Check moving edges there with Output on Matte. |
| Frame Hold | For fast motion (a running leg, a whip pan). **2x** or **4x** repeat each frame so the model sees less motion per step, for sharper edges and a more solid core, but processing takes about 2x or 4x as long. Leave it **Off** for most shots and try 2x on a final shot with fast motion. 4x is rarely worth it. |
| Skip Memory Check | Process even when the memory estimate says the clip won't fit. The estimate is cautious, so it may work. If not, macOS swaps to disk and the job slows down badly or fails, but nothing is damaged. |

**Guided ROI**

| Control | What it does |
|---|---|
| Model Crop | The crop the model saw at the last Process. |
| Multiply by Input Alpha | Trims the matte to the input alpha. Instant. On by default. |
| Black Out Outside Alpha | Blacks out the picture outside the alpha before the model sees it, so it can't pick anything else. The edges then follow your mask. Needs Process. |
| Crop Margin | Extra pixels of context around the alpha's area (default 64). Raise it if the matte is cut off in a straight line at the crop edge. Needs Process. |

**Cache**

Mattes are kept on disk, about 4 MB per HD frame, and renders reuse them. A real change
before the node (a grade, the Resize, a trim, another clip) needs a new Process. **Purge
Cache** deletes this node's mattes and **Purge All** deletes everyone's. **Cache Limit**
(default 50 GB, about 12,000 HD frames) is the most the cache may grow to: the least
recently used mattes go first, and mattes unused for 30 days are deleted too.

**Refresh** is rarely needed: if the viewer still shows MAKING MAGIC after a job has
finished, it picks up the new mattes without reprocessing.

## Check your renders: frames without a matte

A frame that has no matte shows **MATTE MISSING** in the viewer. It shows **PICTURE
CHANGED** if the picture coming in has changed since Process.

**That text can end up in your render:**

- **Fusion page and Fusion Studio:** a render stops at the first frame with no matte
  that it has to draw. But frames you have already viewed in this session can be saved
  with the text in them, and a Deliver made up only of such frames reports Complete.
- **Edit and Color pages:** a Deliver always carries the text on those frames and
  reports Complete.

So process the whole range you are rendering, and check the result.

## Edit and Color pages

The plugin also works as an effect on these pages.

- **Adding it:** on the Edit page, drag it onto the clip, or select the clip and
  double-click the effect. On the Color page, drag it onto a node.
- **Frames:** these pages count edit frames from 0, while the Fusion page counts source
  frames, usually from 1001.
- **After a job:** if the viewer still says MAKING MAGIC or shows an old picture, move
  the playhead one frame.
- **While a job runs on the Color page:** the viewer shows the picture rather than
  MAKING MAGIC. The progress window and Status show that it is running.
- **Remote grades:** a node on the Color page goes into the clip's grade. With a remote
  grade, that changes the clip on every timeline sharing it. To try the plugin, switch
  the clip to a local version first.
- **To see the matte on the Color page:** set Output to Overlay or Matte.

## Slow-motion clips

The Fusion page works on source frames, not edit frames. A clip shot at 120 fps in a
24 fps timeline has about 5 source frames per edit frame: a 21-frame clip was 101 frames
to process. Check the frame count in Status, and use First Frame and Last Frame to try
a section first.

Mattes from an earlier First/Last Frame range stay in the cache, but two ranges
processed separately aren't blended where they meet. For a finished shot, process the
whole shot at once.

## Size and speed

Two limits decide what a Mac can process in one chunk:

- **Memory.** About 41 GB however small the clip, plus about 0.1 GB per megapixel per
  frame: about 45 GB for 33 HD frames, 53 GB for 73.
- **Chunk size.** macOS's GPU framework refuses the model's largest arrays above about
  170 megapixel-frames per chunk, however much memory is free. That makes 73 frames the
  practical maximum at HD.

So work in HD with a Resize, and use the matte as a mask on the full-resolution picture,
or use Guided ROI. UHD only fits for very short clips, about 17 frames.

| Mac | Job | Time |
|---|---|---|
| M5 Max, 128 GB | 176 HD frames in Resolve, 3 chunks | about 31 min |
| M5 Max, 128 GB | 176 frames with a 512×576 Guided ROI crop in Fusion Studio, 2 chunks | about 4.3 min |
| M5 Max, 128 GB | 63 HD frames with Frame Hold Off, 2x, 4x | about 9, 17, 35 min |
| M1 Max, 64 GB | 33 HD frames in Fusion Studio | about 32 min |

On a 64 GB Mac, use Fusion Studio. It leaves about 45-49 GB for the model, against about
39 GB in Resolve, where the memory check stops HD jobs (Skip Memory Check runs them
anyway). Keep Fusion Studio's own memory setting (Preferences > Memory) at the default
80% or lower.

## Known limits

- **Repeat runs can differ slightly.** The first chunk of a job repeats exactly, but
  later chunks can differ a little between runs, mostly at the edges. The cause isn't
  known.
- **Seams.** Long clips are made in chunks, and the model may pick a slightly different
  subject in each. Check the frames listed under Seams.

## If something goes wrong

**Collect Logs** in Setup writes a support bundle (logs and system details) to your home
folder. The log itself is `~/.cache/ltx-alphagen/logs/ltx-alphagen.log`.

## Uninstall

Delete these. The last three hold the models, mattes and settings, and only exist once
you have used the plugin.

- `/Library/OFX/Plugins/LTXAlphaGen.ofx.bundle`
- `~/ltx-alphagen-models` (or your Model Folder)
- `~/.cache/ltx-alphagen` (and your Cache Folder, if you changed it)
- `~/.config/ltx-alphagen`

## Build from source

You need the Xcode Command Line Tools and cmake (`brew install cmake`). Quit Resolve first.

```bash
./build.sh --systest
```

```bash
./install.sh
```

The source is in `src/`, and every change from the Flame version is listed in
`src/CVT_PORT.md`.

## Licences and credits

- **The plugin** is open source under the Mozilla Public License 2.0 (`src/LICENSE`),
  the licence of the Flame plugin it is based on. See `src/NOTICE`.
- **The original plugin:** LTX Alpha Gen OFX for Flame, copyright 2026 Jan Klier, donated
  to and maintained by Logik.tv.
- **The models are not included.** Setup downloads them from Lightricks under the LTX-2.x
  Community License, which is free below USD 10M annual revenue. Above that, a paid
  Lightricks licence is required.
- **The port:** Chadwick, [Creative Video Tips](https://creativevideotips.com).

DaVinci Resolve and Fusion Studio are trademarks of Blackmagic Design.
