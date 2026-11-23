#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
    The OMT sources on the network, polled on one thread per process image.

    omt_discovery_getaddresses returns whatever DNS-SD (or a discovery
    server) has heard so far, without blocking, so a 1 s poll is cheap. The
    first call also loads the runtime, which is the slow part and the reason
    this lives on its own thread: the render thread only reads a snapshot.

    The thread starts on the first subscribe() (the first rendered frame of
    an OMT In, never the startup scan) and stops when the last subscriber
    leaves. The object is deliberately leaked so a thread still running at
    process exit never touches a destroyed static.
*/
namespace somt
{

class Discovery
{
public:
	static Discovery& instance();

	void subscribe();
	void unsubscribe();

	/// Sorted source names, and a counter that changes whenever they do.
	std::vector< std::string > sources( uint64_t* generation = nullptr ) const;
	uint64_t generation() const;

	/// Empty while loading or once loaded; the reason otherwise.
	std::string runtimeError() const;
	bool runtimeReady() const;

private:
	Discovery() = default;
	struct State;
	State& s() const;
};

/// A source name as an option parameter's value: FNV-1a folded to 23 bits
/// and offset past the menu's fixed entries, so it is exact in a float and a
/// saved composition finds the same source by name after a restart, whatever
/// order discovery lists it in.
constexpr float kKeyBase = 16.0f;
float sourceKey( const std::string& name );

} // namespace somt
