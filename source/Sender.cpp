#include "Sender.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>

#include "Diag.h"
#include "OmtRuntime.h"

#ifndef RESOLUME_OMT_VERSION
	#define RESOLUME_OMT_VERSION "0.0.0"
#endif

namespace somt
{
namespace
{
std::mutex& namesMutex()
{
	static std::mutex* leaked = new std::mutex();
	return *leaked;
}
std::set< std::string >& names()
{
	static std::set< std::string >* leaked = new std::set< std::string >();
	return *leaked;
}

void copyField( char* field, size_t size, const std::string& value )
{
	std::memset( field, 0, size );
	std::memcpy( field, value.data(), std::min( value.size(), size - 1 ) );
}
} // namespace

std::string claimSenderName( const std::string& wanted )
{
	const std::string base = wanted.empty() ? std::string( "Resolume" ) : wanted;
	std::lock_guard< std::mutex > lock( namesMutex() );
	std::string candidate = base;
	for( int n = 2; names().count( candidate ); ++n )
		candidate = base + " " + std::to_string( n );
	names().insert( candidate );
	return candidate;
}

void releaseSenderName( const std::string& claimed )
{
	if( claimed.empty() )
		return;
	std::lock_guard< std::mutex > lock( namesMutex() );
	names().erase( claimed );
}

Sender::~Sender()
{
	{
		std::lock_guard< std::mutex > lock( mutex );
		stop = true;
	}
	wake.notify_all();
	if( thread.joinable() )
		thread.join();
}

void Sender::configure( const std::string& name, int quality, bool active )
{
	const std::string effective = name.empty() ? std::string( "Resolume" ) : name;
	{
		std::lock_guard< std::mutex > lock( mutex );
		if( effective == wantedName && quality == wantedQuality && active == wantedActive )
			return;
		wantedName = effective;
		wantedQuality = quality;
		wantedActive = active;
		++configSerial;
		if( !active )
			pendingFresh = false;
	}
	wake.notify_all();
}

OutgoingFrame& Sender::frameToFill( int width, int height )
{
	filling.width = width;
	filling.height = height;
	filling.pixels.resize( static_cast< size_t >( width ) * static_cast< size_t >( height ) * 4 );
	return filling;
}

void Sender::submit()
{
	bool start = false;
	{
		std::lock_guard< std::mutex > lock( mutex );
		if( pendingFresh )
			++current.dropped;// the encoder never got to the last one
		std::swap( filling, pending );
		pendingFresh = true;
		start = !thread.joinable();
	}
	if( start )
		thread = std::thread( [ this ] { run(); } );
	wake.notify_all();
}

SenderStatus Sender::status() const
{
	std::lock_guard< std::mutex > lock( mutex );
	return current;
}

void Sender::run()
{
	{
		std::lock_guard< std::mutex > lock( mutex );
		current.state = SenderStatus::State::Loading;
	}
	std::string error;
	const OmtApi* api = omtApi( &error );
	if( !api )
	{
		std::unique_lock< std::mutex > lock( mutex );
		current.state = SenderStatus::State::NoRuntime;
		current.error = error;
		wake.wait( lock, [ this ] { return stop; } );
		return;
	}

	omt_send_t* sender = nullptr;
	std::string claimed;
	uint64_t appliedSerial = 0;
	OutgoingFrame working;

	using clock = std::chrono::steady_clock;
	auto windowStart = clock::now();
	double windowEncodeMs = 0.0;
	int windowFrames = 0;

	for( ;; )
	{
		std::string name;
		int quality = 0;
		bool active = true;
		uint64_t serialNow = 0;
		bool haveFrame = false;
		{
			std::unique_lock< std::mutex > lock( mutex );
			wake.wait( lock, [ & ] { return stop || pendingFresh || configSerial != appliedSerial; } );
			if( stop )
				break;
			name = wantedName;
			quality = wantedQuality;
			active = wantedActive;
			serialNow = configSerial;
			if( pendingFresh )
			{
				std::swap( pending, working );
				pendingFresh = false;
				haveFrame = true;
			}
		}

		if( serialNow != appliedSerial )
		{
			if( sender )
			{
				api->send_destroy( sender );
				sender = nullptr;
				diag::info( "sender closed: " + claimed );
			}
			releaseSenderName( claimed );
			claimed.clear();
			appliedSerial = serialNow;
			if( !active )
			{
				std::lock_guard< std::mutex > lock( mutex );
				current.state = SenderStatus::State::Idle;
				current.name.clear();
				current.address.clear();
				current.connections = 0;
				continue;
			}
			claimed = claimSenderName( name );
			sender = api->send_create( claimed.c_str(), static_cast< OMTQuality >( quality ) );
			std::string address;
			if( sender )
			{
				if( api->send_setsenderinformation )
				{
					OMTSenderInfo info {};
					copyField( info.ProductName, sizeof( info.ProductName ), "Resolume (SW OMT Out)" );
					copyField( info.Manufacturer, sizeof( info.Manufacturer ), "Stoatworks" );
					copyField( info.Version, sizeof( info.Version ), RESOLUME_OMT_VERSION );
					api->send_setsenderinformation( sender, &info );
				}
				char buffer[ 512 ] = {};
				if( api->send_getaddress( sender, buffer, sizeof( buffer ) ) > 0 )
					address = buffer;
				diag::info( "sender opened: " + claimed + " as " + address );
			}
			else
			{
				diag::error( "omt_send_create failed for " + claimed );
			}
			std::lock_guard< std::mutex > lock( mutex );
			current.name = claimed;
			current.address = address;
			current.state = sender ? SenderStatus::State::Idle : SenderStatus::State::Failed;
			current.error = sender ? std::string() : "could not create the sender";
		}

		if( !haveFrame || !sender )
			continue;

		OMTMediaFrame frame {};
		frame.Type = OMTFrameType_Video;
		frame.Codec = OMTCodec_BGRA;
		frame.Width = working.width;
		frame.Height = working.height;
		frame.Stride = working.width * 4;
		frame.Flags = working.alpha ? OMTVideoFlags_Alpha : OMTVideoFlags_None;
		frame.FrameRateN = working.rateN;
		frame.FrameRateD = working.rateD;
		frame.AspectRatio = working.height > 0 ? static_cast< float >( working.width ) / static_cast< float >( working.height ) : 1.0f;
		frame.ColorSpace = working.height >= 720 ? OMTColorSpace_BT709 : OMTColorSpace_BT601;
		frame.Timestamp = working.timestamp;
		frame.Data = working.pixels.data();
		frame.DataLength = static_cast< int >( working.pixels.size() );

		const auto before = clock::now();
		api->send( sender, &frame );
		const double ms = std::chrono::duration< double, std::milli >( clock::now() - before ).count();
		windowEncodeMs += ms;
		++windowFrames;
		const int connections = api->send_connections( sender );

		std::lock_guard< std::mutex > lock( mutex );
		current.state = SenderStatus::State::Sending;
		current.width = working.width;
		current.height = working.height;
		current.rateN = working.rateN;
		current.rateD = working.rateD;
		current.connections = connections;
		++current.sent;
		if( clock::now() - windowStart >= std::chrono::seconds( 1 ) )
		{
			current.encodeMs = windowFrames ? windowEncodeMs / windowFrames : 0.0;
			windowStart = clock::now();
			windowEncodeMs = 0.0;
			windowFrames = 0;
		}
	}

	if( sender )
		api->send_destroy( sender );
	releaseSenderName( claimed );
}

} // namespace somt
