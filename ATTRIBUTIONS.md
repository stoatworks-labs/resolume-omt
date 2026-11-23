# Attributions

Resolume OMT is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### libomt and libvmx, the OMT runtime — Open Media Transport Contributors

<https://github.com/openmediatransport/libomtnet>  
Licence: MIT  
Copyright: Open Media Transport Contributors

Each plugin bundle carries libomt.dylib (1.0.0.16) and libvmx.dylib in Contents/Frameworks, from the Open Media Transport project's binaries release, and loads them at run time; third_party/omt/libomt.h is libomt's header, with its LICENSE.txt beside it. Both are MIT, so this notice travels in every download.

### The .NET runtime, compiled into libomt — .NET Foundation and Contributors

<https://github.com/dotnet/runtime>  
Licence: MIT  
Copyright: .NET Foundation and Contributors

libomt is a Native AOT .NET library (libomtnet compiled ahead of time), so the .NET runtime and its core library are compiled into the libomt.dylib each bundle ships.

### The log — Stoatworks haze

<https://github.com/stoatworks-labs/haze>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Diag.{h,cpp} is haze's log (millpond's before it), adapted: its own folder and environment variable, and directories made with mkdir rather than a shell.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

### Open Media Transport (libomt)

<https://github.com/openmediatransport/libomtnet>  
Licence: MIT  
Copyright: Open Media Transport Contributors

Header vendored alongside the NDI headers; the library is loaded at run time.

The open alternative to NDI, and MIT end to end — so unlike NDI it can be supported without a proprietary licence in the path.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
