#pragma once

#include <string>

#include <libomt.h>

/**
    The Open Media Transport runtime, loaded at run time.

    libomt is a Native AOT build of the .NET libomtnet: one self-contained
    native library (about 17 MB, universal on macOS) plus libvmx, the VMX
    codec, which it opens by leaf name and finds beside itself. Each bundle
    carries both in Contents/Frameworks.

    Why run time and not a link: Arena constructs every plugin at its startup
    scan, so a linked libomt would start a .NET runtime inside Resolume for
    everyone who has the plugin installed, used or not. Loading on first use
    keeps the scan free, and a missing or broken runtime becomes a status
    line instead of a plugin that fails to load.

    One runtime per process where possible: if another bundle (the other
    half of this pair, or anything else) has already loaded a libomt, that
    image is reused rather than a second copy started. Two copies do work
    (measured: frames crossed between two runtimes in one process), so a race
    between two first loads is harmless, just wasteful.

    The library is never unloaded. A Native AOT runtime cannot be torn down.
*/
namespace somt
{

struct OmtApi
{
	char** ( *discovery_getaddresses )( int* ) = nullptr;

	omt_receive_t* ( *receive_create )( const char*, OMTFrameType, OMTPreferredVideoFormat, OMTReceiveFlags ) = nullptr;
	void ( *receive_destroy )( omt_receive_t* ) = nullptr;
	OMTMediaFrame* ( *receive )( omt_receive_t*, OMTFrameType, int ) = nullptr;
	void ( *receive_getsenderinformation )( omt_receive_t*, OMTSenderInfo* ) = nullptr;// optional
	void ( *receive_getvideostatistics )( omt_receive_t*, OMTStatistics* ) = nullptr;  // optional

	omt_send_t* ( *send_create )( const char*, OMTQuality ) = nullptr;
	void ( *send_destroy )( omt_send_t* ) = nullptr;
	int ( *send )( omt_send_t*, OMTMediaFrame* ) = nullptr;
	int ( *send_connections )( omt_send_t* ) = nullptr;
	int ( *send_getaddress )( omt_send_t*, char*, int ) = nullptr;
	void ( *send_setsenderinformation )( omt_send_t*, OMTSenderInfo* ) = nullptr;// optional
	void ( *send_getvideostatistics )( omt_send_t*, OMTStatistics* ) = nullptr;  // optional
};

/// Load libomt if it is not loaded yet, and return its entry points, or null
/// with `error` set. Thread-safe. It can take a few hundred milliseconds the
/// first time (dyld plus the .NET runtime's start), so call it from a worker
/// thread, never from ProcessOpenGL.
const OmtApi* omtApi( std::string* error = nullptr );

/// The entry points if a previous omtApi() call succeeded, else null. Never
/// blocks on a load, so the render thread may call it.
const OmtApi* omtApiIfLoaded();

/// Where the runtime came from (for the log and the harness), empty until
/// loaded.
std::string omtRuntimePath();

/// Whether this image found an already-loaded libomt rather than opening its
/// own copy.
bool omtRuntimeWasShared();

} // namespace somt
