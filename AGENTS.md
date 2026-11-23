# AGENTS.md: Resolume OMT

Why the code is the way it is. `CLAUDE.md` is the command reference and the layout; the user's
view is `docs/USER-GUIDE.md`. Two FFGL plugins over one OBJECT core: **SW OMT In** (`OM01`, a
source) and **SW OMT Out** (`OM02`, an effect), Open Media Transport into and out of Resolume.

## What is verified, and what is not

Keep this honest. "Compiles" is not "works", and "Arena loaded it" is not "the picture is right".

- **Run in Resolume Arena 7.29 on macOS**, both plugins, end to end over real OMT, with
  `omttest --probe` reading a test card's frame counter back, so dropped, repeated and
  out-of-order frames show up as well as colour and orientation (CLAUDE.md has the recipe through
  Arena's REST API). Saving and reopening a composition were checked there too.
- **`omttest`: 43 checks, all passing** (v0.1.0, 2026-11-23, M4 Max). It hosts both plugins in a
  headless CGL context the way Resolume does and checks them against libomt itself: Out's
  pass-through, geometry, colour, order and alpha flag; In's frames, colour, Fit's bars, Hold and
  Black; the discovery loop, latency, unique and reclaimed names, pacing, the render-thread cost
  and churn. `names.about` pushes every declared default back through the setters as a host's
  instantiateGL does, which nothing else here exercises.
- **Not tried:** Avenue, any Arena other than 7.29, Intel hardware (the x86_64 slices are built
  and `lipo`-checked, never run), a receiver other than libomt's own and Arena.
- **Not written:** Windows (the CMake has a Windows branch; nothing has built it), tally, audio.

## Decisions

- **libomt is loaded at run time, never linked.** Arena constructs every installed plugin at its
  startup scan, so a linked libomt would start a .NET runtime inside Resolume for everyone who has
  the plugin installed. Loading on first use keeps the scan free, and a missing runtime becomes a
  status line instead of a plugin that fails to load. Nothing starts in a constructor: discovery,
  receivers and senders start on the first ProcessOpenGL (`scan.lazy` pins it).
- **One runtime per process.** An image already loaded by anyone (the other bundle, another
  plugin) is reused through an `RTLD_NOLOAD` scan. Two copies do work, but opening a composition
  with an In and an Out made both bundles load libomt in the same instant, each found nothing
  loaded, and Arena ran two .NET runtimes. Loading is serialised across images with a pid-keyed
  named semaphore (a named mutex on Windows). The library is never unloaded: a Native AOT runtime
  cannot be torn down.
- **Each bundle carries its own runtime** in `Contents/Frameworks` (libomt 1.0.0.16 and libvmx,
  from the Open Media Transport binaries release, ~18 MB). Either bundle works alone.
- **Address is OMT In's source of truth; the Source menu follows it.** The menu's values are
  `sourceKey()`: an FNV-1a hash of the name folded to 23 bits, exact in a float, so a saved
  composition finds a source by name whatever order discovery lists it in. Never reorder the fixed
  entries (None = 0, Typed address = 1) or change the hash: saved compositions store it.
- **Out reads back through three PBOs** and encodes on a worker thread: the render thread pays a
  copy, not a stall, and the picture is one frame late. A fixed Frame Rate is paced on an ideal
  timeline that never runs backwards; Auto sends every rendered frame.
- **A sender that settled for "Name 2" takes its own name back** as soon as it is free.
- **The About block is last** in both plugins (`P_ABOUT_FIRST`), from `stoatworks-backend`'s
  `sync-about.py`. `source/StoatworksAbout*.h` are generated: never edit them here.

## Traps already hit

- **Arena builds a reopened composition's effects before it destroys the old ones.** Without the
  name reclaim, a composition reopened in place sent as "Name 2" from then on, and every receiver
  pointed at "Name" lost it. `out.reclaim-name` pins it.
- **Two first loads at once start two .NET runtimes.** See the semaphore above.
- **The About text line must accept its own default.** instantiateGL pushes every declared default
  through the setters and deletes the instance on the first FF_FAIL, and CFFGLPlugin's stub
  returns FF_FAIL. Every hand-driven check would still pass. `names.about` catches it (proved by
  removing the guard: it fails on Out's index 6).
- **Resolume's textures are premultiplied; OMT's rows are top first.** Out un-premultiplies when
  Send Alpha is on and In premultiplies straight alpha; both directions flip rows in the shader.
- **Arena's Advanced Output device list is compiled in.** No plugin can add OMT to it. The README
  points at a Frame Ferret virtual display (experimental, on Frame Ferret's main, not yet in a
  release) or OMT Out on the composition.

## Releasing

Built and packaged on this Mac, not by CI: libomt is a Native AOT library from outside the repo
(`OMT_RUNTIME_DIR`, default `~/.local/lib/omt`), and nothing here builds it.

1. Bump `project(... VERSION x.y.z` in CMakeLists.txt, and run `sync-about.py --only resolume-omt
   --apply` from `stoatworks-backend` (the About block's version fallback). Grep for the old
   version before tagging.
2. `scripts/release.sh` from the tagged commit: a universal Release build, `omttest`, `lipo` on
   both binaries AND both dylibs in each bundle, `_plugMain`, the plist version, then
   `out/release/resolume-omt-macos-universal.zip` and `resolume-omt-<x.y.z>-macos-universal.dmg`,
   each with both bundles, ATTRIBUTIONS.md and libomt's licence. Both are ad hoc on purpose.
3. `gh release create v<x.y.z>` with both files. Check the screen is unlocked and
   `xcrun notarytool history --keychain-profile notary` answers first, then kick the autosign agent
   (`launchctl kickstart gui/$(id -u)/com.stoatworks.autosign`). It signs inside out with the
   Developer ID: `rl_mac_sign` signs every `*.dylib` under `Contents/` with hardened runtime and a
   timestamp before it seals the bundle, then notarises, staples and re-uploads. Never sign the
   bundle before the dylibs.
4. `check-notarised.py --repo resolume-omt`, `gen-downloads.py --repo resolume-omt` and
   `gen-catalog-data.py --repo resolume-omt` from `stoatworks-backend`, then the website.
