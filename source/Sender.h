#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/**
    One OMT sender on its own thread.

    omt_send encodes VMX on the calling thread, which is milliseconds at 1080p
    and more at 4K, so the render thread only copies pixels into a buffer and
    hands it over. If the encoder is still busy with the previous frame when
    the next one arrives, the waiting frame is replaced (and counted as
    dropped): a sender that queues falls further behind every frame.

    The name is what receivers see after the host name, "HOST (Name)". Two
    senders in one process with the same name would be indistinguishable to
    a receiver, so the second becomes "Name 2", and so on; status() reports
    the name actually in use. A sender on "Name 2" moves back to "Name" once
    the first has gone.
*/
namespace somt
{

struct SenderStatus
{
	enum class State
	{
		Idle,     // no frame yet
		Loading,  // waiting on the runtime
		NoRuntime,// the runtime would not load
		Failed,   // omt_send_create refused
		Sending
	};
	State state = State::Idle;
	std::string name;   // as claimed in this process
	std::string address;// as receivers see it, from libomt
	std::string error;
	int width = 0;
	int height = 0;
	int rateN = 0;
	int rateD = 1;
	int connections = 0;
	uint64_t sent = 0;
	uint64_t dropped = 0;
	double encodeMs = 0.0;// mean of the last second
};

struct OutgoingFrame
{
	std::vector< uint8_t > pixels;// BGRA, top row first, tightly packed
	int width = 0;
	int height = 0;
	int rateN = 60;
	int rateD = 1;
	bool alpha = false;
	int64_t timestamp = 0;// 100 ns units
};

class Sender
{
public:
	Sender() = default;
	~Sender();

	Sender( const Sender& ) = delete;
	Sender& operator=( const Sender& ) = delete;

	/// Name (empty means "Resolume"), OMTQuality value, and whether the
	/// source should exist at all (off = it leaves the network). Takes
	/// effect on the worker; a change recreates or removes the sender.
	void configure( const std::string& name, int quality, bool active = true );

	/// The buffer to fill for the next frame, sized for width x height BGRA.
	/// Render thread only; valid until submit().
	OutgoingFrame& frameToFill( int width, int height );
	void submit();

	SenderStatus status() const;

private:
	void run();

	mutable std::mutex mutex;
	std::condition_variable wake;
	std::thread thread;
	bool stop = false;

	std::string wantedName = "Resolume";
	int wantedQuality = 0;
	bool wantedActive = true;
	uint64_t configSerial = 1;

	OutgoingFrame filling;
	OutgoingFrame pending;
	bool pendingFresh = false;

	SenderStatus current;
};

/// Process-wide registry of sender names, so two OMT Outs never collide.
std::string claimSenderName( const std::string& wanted );
bool senderNameFree( const std::string& wanted );
void releaseSenderName( const std::string& claimed );

} // namespace somt
