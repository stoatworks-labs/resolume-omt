# Resolume OMT

Two FFGL plugins that put [Open Media Transport](https://github.com/openmediatransport)
(OMT) into Resolume Arena and Avenue:

- **SW OMT In** — a source. Any OMT source on the network becomes a clip.
- **SW OMT Out** — an effect. Whatever it sits on (a clip, a layer, a group, the whole
  composition) goes out as an OMT source and passes through unchanged.

OMT is the open, MIT-licensed video-over-IP protocol from the vMix team. Resolume has NDI
and Syphon built in but not OMT, and its Advanced Output device list is compiled into the
app, so no plugin can add OMT there. To send Advanced Output over OMT, give a screen a
[Frame Ferret](https://github.com/stoatworks-labs/frame-ferret) virtual display as its
device (Arena lists it as an ordinary display, "Display 2" and so on) and route that to a
Frame Ferret OMT sender, or put OMT Out on the composition.

## Using them

Copy both bundles into `~/Documents/Resolume Arena/Extra Effects` (or Avenue's) and
restart Resolume.

### SW OMT In

| Parameter | What it does |
|---|---|
| Source | The OMT sources Resolume can see, found by DNS-SD. Picking one fills in Address. A saved source that has gone offline stays listed as "(offline)". |
| Address | The source to receive, `HOST (Name)`, or a `host:port` that discovery can't see. This is what the composition saves. |
| Fit | Fit, Fill, Stretch, or Native 1:1, when the source isn't the clip's size. |
| No Signal | Hold last frame, Transparent, or Black, after a second without frames. |
| Keep Alpha | Use the source's alpha, if it sends one. Off makes it opaque. |
| Status | What it's receiving, read-only. |

### SW OMT Out

| Parameter | What it does |
|---|---|
| Name | The source name receivers see after the host name, `HOST (Name)`. Two OMT Outs with the same name become "Name" and "Name 2"; when the first goes, the second takes the name back, so reopening a composition keeps its OMT name. |
| Send | Off takes the source off the network. |
| Quality | Receiver decides, Low, Medium or High (VMX encoder quality). |
| Send Alpha | Send the alpha channel, un-premultiplied. |
| Frame Rate | Auto (Resolume's render rate, snapped to the nearest broadcast rate), or a fixed rate. A fixed rate paces sending: Arena renders at the display's rate, often 100 or 120 Hz, and a 60 fps source should carry 60 frames a second, not relabel 100. |
| Status | The name receivers see, size, rate, and receiver count. |

OMT Out reads the frame back asynchronously, so it costs Resolume's render thread a copy
(0.75 ms at 1080p, 1.4 ms at 4K on an M-series Mac) and adds one frame of latency. The VMX
encode (3–4 ms at 1080p) runs on its own thread.

## The OMT runtime

The plugins load `libomt.dylib` and `libvmx.dylib` at run time; each bundle carries a copy
in `Contents/Frameworks`. Two plugins in one Resolume share whichever copy loaded first.
`RESOLUME_OMT_LIBRARY` points them at another build. libomt is a Native AOT .NET library,
about 18 MB, which is why the bundles are large.

Logs go to `~/Library/Logs/resolume-omt/`. `RESOLUME_OMT_VERBOSE=1` adds libomt's own log.

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOMT_RUNTIME_DIR=$HOME/.local/lib/omt
cmake --build build
cmake --install build   # into ~/Documents/Resolume Arena/Extra Effects
```

`OMT_RUNTIME_DIR` must hold a universal `libomt.dylib` and `libvmx.dylib` (from the
[libomt releases](https://github.com/openmediatransport/libomt/releases)). The bundles are
universal (arm64 and x86_64), signed ad hoc unless `SOMT_SIGN_IDENTITY` names a
certificate.

### Testing without Resolume

`omttest` hosts both plugins in a headless OpenGL context, the way Resolume does, and
checks them against each other and a test card over real OMT:

```bash
./build/omttest            # all checks
./build/omttest --list     # their names
./build/omttest --serve "Test Card" 600        # a live 1080p60 test card
./build/omttest --probe "MAC (Arena Test)" 10  # receive one and check it
```

The probe reads the test card's frame counter, so it reports dropped, repeated and
out-of-order frames as well as colour and orientation.

## Status

macOS only so far, tested in Resolume Arena 7.29. Not yet: Windows, tally, audio.

## Licence

MIT. libomt and libvmx are MIT, © Open Media Transport Contributors; the FFGL SDK is
BSD-3-Clause.
