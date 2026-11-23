#include "Receiver.h"

#include <cstring>

#include "Diag.h"
#include "OmtRuntime.h"

namespace somt
{
namespace
{
// Requested as BGRA, so libomt converts from VMX for us; alpha-less senders
// arrive as BGRX, which the enum has no name for.
constexpr uint32_t kFourccBgrx = 0x58524742;

/// How long without a frame before Live becomes NoSignal.
constexpr double kNoSignalSeconds = 1.0;
} // namespace

Receiver::~Receiver()
{
	{
		std::lock_guard< std::mutex > lock( mutex );
		stop = true;
	}
	wake.notify_all();
	if( thread.joinable() )
		thread.join();
}

void Receiver::setTarget( const std::string& target )
{
	bool start = false;
	{
		std::lock_guard< std::mutex > lock( mutex );
		if( target == wanted )
			return;
		wanted = target;
		start = !thread.joinable() && !target.empty();
	}
	wake.notify_all();
	if( start )
		thread = std::thread( [ this ] { run(); } );
}

std::string Receiver::target() const
{
	std::lock_guard< std::mutex > lock( mutex );
	return wanted;
}

bool Receiver::takeFrame( VideoFrame& front )
{
	std::lock_guard< std::mutex > lock( mutex );
	if( !readyFresh )
		return false;
	std::swap( front, ready );
	readyFresh = false;
	return true;
}

ReceiverStatus Receiver::status() const
{
	std::lock_guard< std::mutex > lock( mutex );
	ReceiverStatus copy = current;
	if( copy.frames > 0 )
		copy.secondsSinceFrame = std::chrono::duration< double >( std::chrono::steady_clock::now() - lastFrame ).count();
	if( copy.state == ReceiverStatus::State::Live && copy.secondsSinceFrame > kNoSignalSeconds )
		copy.state = ReceiverStatus::State::NoSignal;
	return copy;
}

void Receiver::run()
{
	{
		std::lock_guard< std::mutex > lock( mutex );
		current.state = ReceiverStatus::State::Loading;
	}
	std::string error;
	const OmtApi* api = omtApi( &error );
	if( !api )
	{
		std::unique_lock< std::mutex > lock( mutex );
		current.state = ReceiverStatus::State::NoRuntime;
		current.error = error;
		wake.wait( lock, [ this ] { return stop; } );
		return;
	}

	omt_receive_t* receiver = nullptr;
	std::string connected;
	VideoFrame spare;
	bool askedForInfo = false;

	for( ;; )
	{
		std::string target;
		{
			std::unique_lock< std::mutex > lock( mutex );
			if( stop )
				break;
			if( wanted.empty() && !receiver )
			{
				current = ReceiverStatus {};
				wake.wait( lock, [ this ] { return stop || !wanted.empty(); } );
				continue;
			}
			target = wanted;
		}

		if( target != connected )
		{
			if( receiver )
			{
				api->receive_destroy( receiver );
				receiver = nullptr;
				diag::info( "receiver closed: " + connected );
			}
			connected = target;
			askedForInfo = false;
			{
				std::lock_guard< std::mutex > lock( mutex );
				current = ReceiverStatus {};
				current.target = target;
				current.state = target.empty() ? ReceiverStatus::State::Idle : ReceiverStatus::State::Connecting;
				readyFresh = false;
			}
			if( target.empty() )
				continue;
			receiver = api->receive_create( target.c_str(), OMTFrameType_Video, OMTPreferredVideoFormat_BGRA,
			                                OMTReceiveFlags_None );
			if( !receiver )
			{
				diag::error( "omt_receive_create failed for " + target );
				std::unique_lock< std::mutex > lock( mutex );
				current.error = "could not create a receiver";
				// Retry in a second, or sooner if the target changes.
				wake.wait_for( lock, std::chrono::seconds( 1 ), [ this, &target ] { return stop || wanted != target; } );
				connected.clear();
				continue;
			}
			diag::info( "receiver opened: " + target );
		}

		// A short timeout so a target change or stop is seen promptly.
		OMTMediaFrame* media = api->receive( receiver, OMTFrameType_Video, 40 );
		if( !media || media->Type != OMTFrameType_Video || !media->Data || media->Width <= 0 || media->Height <= 0 )
			continue;
		const uint32_t codec = static_cast< uint32_t >( media->Codec );
		if( codec != static_cast< uint32_t >( OMTCodec_BGRA ) && codec != kFourccBgrx )
			continue;// asked for BGRA; anything else is not ours to draw

		const size_t bytes = static_cast< size_t >( media->Stride ) * static_cast< size_t >( media->Height );
		if( media->Stride < media->Width * 4 || static_cast< size_t >( media->DataLength ) < bytes )
			continue;
		spare.pixels.resize( bytes );
		std::memcpy( spare.pixels.data(), media->Data, bytes );
		spare.width = media->Width;
		spare.height = media->Height;
		spare.stride = media->Stride;
		spare.codec = codec;
		spare.flags = static_cast< int >( media->Flags );
		spare.rateN = media->FrameRateN;
		spare.rateD = media->FrameRateD > 0 ? media->FrameRateD : 1;
		spare.aspect = media->AspectRatio;
		spare.timestamp = media->Timestamp;

		std::string product;
		if( !askedForInfo && api->receive_getsenderinformation )
		{
			OMTSenderInfo info {};
			api->receive_getsenderinformation( receiver, &info );
			info.ProductName[ sizeof( info.ProductName ) - 1 ] = 0;
			product = info.ProductName;
			askedForInfo = true;
		}

		std::lock_guard< std::mutex > lock( mutex );
		if( wanted != connected )
			continue;// retargeted while decoding; drop it
		spare.serial = ++serial;
		std::swap( spare, ready );
		readyFresh = true;
		if( current.frames == 0 )
			diag::info( "first frame from " + connected + ": " + std::to_string( media->Width ) + "x"
			            + std::to_string( media->Height ) );
		current.state = ReceiverStatus::State::Live;
		current.width = ready.width;
		current.height = ready.height;
		current.rateN = ready.rateN;
		current.rateD = ready.rateD;
		current.alpha = codec == static_cast< uint32_t >( OMTCodec_BGRA ) && ( ready.flags & OMTVideoFlags_Alpha );
		current.error.clear();
		if( !product.empty() )
			current.product = product;
		++current.frames;
		lastFrame = std::chrono::steady_clock::now();
	}

	if( receiver )
		api->receive_destroy( receiver );
}

} // namespace somt
