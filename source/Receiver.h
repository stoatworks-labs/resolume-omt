#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/**
    One OMT receiver on its own thread, handing the render thread the newest
    frame and nothing older.

    libomt decodes VMX and converts to BGRA on the calling thread; at 4K that
    is milliseconds, so it never runs on Resolume's render thread. The worker
    waits in omt_receive, copies each frame into a spare buffer and swaps it
    in as "ready". The render thread swaps "ready" for its own front buffer
    when there is a new one. Three buffers, so neither side ever waits for
    the other, and a slow render thread just sees fewer frames.

    The target is a source name as discovery lists it ("HOST (Name)") or an
    omt:// URL; libomt resolves either. Changing it tears down and recreates
    the receiver on the worker.
*/
namespace somt
{

struct VideoFrame
{
	std::vector< uint8_t > pixels;
	int width = 0;
	int height = 0;
	int stride = 0;
	uint32_t codec = 0;
	int flags = 0;
	int rateN = 0;
	int rateD = 1;
	float aspect = 0.0f;
	int64_t timestamp = 0;
	uint64_t serial = 0;
};

struct ReceiverStatus
{
	enum class State
	{
		Idle,      // no target
		Loading,   // waiting on the runtime
		NoRuntime, // the runtime would not load
		Connecting,// a target, no frame yet
		Live,
		NoSignal// had frames, none for a second
	};
	State state = State::Idle;
	std::string target;
	std::string error;
	std::string product;// from the sender's information, when it gives one
	int width = 0;
	int height = 0;
	int rateN = 0;
	int rateD = 1;
	bool alpha = false;
	uint64_t frames = 0;
	double secondsSinceFrame = 0.0;
};

class Receiver
{
public:
	Receiver() = default;
	~Receiver();

	Receiver( const Receiver& ) = delete;
	Receiver& operator=( const Receiver& ) = delete;

	/// Starts the worker on the first non-empty target.
	void setTarget( const std::string& target );
	std::string target() const;

	/// If a newer frame than the one in `front` is ready, swap it in and
	/// return true. Never waits.
	bool takeFrame( VideoFrame& front );

	ReceiverStatus status() const;

private:
	void run();

	mutable std::mutex mutex;
	std::condition_variable wake;
	std::thread thread;
	bool stop = false;

	std::string wanted;
	VideoFrame ready;
	bool readyFresh = false;
	uint64_t serial = 0;

	ReceiverStatus current;
	std::chrono::steady_clock::time_point lastFrame {};
};

} // namespace somt
