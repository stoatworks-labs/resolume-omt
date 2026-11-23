#include "Discovery.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "Diag.h"
#include "OmtRuntime.h"

namespace somt
{

struct Discovery::State
{
	mutable std::mutex mutex;
	std::condition_variable wake;
	std::thread thread;
	int subscribers = 0;
	bool stop = false;
	bool running = false;

	std::vector< std::string > names;
	uint64_t generation = 0;
	std::string runtimeError;
	bool runtimeReady = false;

	void run()
	{
		std::string error;
		const OmtApi* api = omtApi( &error );
		{
			std::lock_guard< std::mutex > lock( mutex );
			runtimeReady = api != nullptr;
			runtimeError = api ? std::string() : error;
			++generation;
		}
		if( !api )
		{
			std::unique_lock< std::mutex > lock( mutex );
			wake.wait( lock, [ this ] { return stop; } );
			return;
		}

		const auto started = std::chrono::steady_clock::now();
		for( ;; )
		{
			int count = 0;
			char** addresses = api->discovery_getaddresses( &count );
			std::vector< std::string > found;
			for( int i = 0; i < count; ++i )
				if( addresses && addresses[ i ] && *addresses[ i ] )
					found.emplace_back( addresses[ i ] );
			std::sort( found.begin(), found.end() );
			found.erase( std::unique( found.begin(), found.end() ), found.end() );

			std::unique_lock< std::mutex > lock( mutex );
			if( found != names )
			{
				names = std::move( found );
				++generation;
			}
			// Quick polls for the first seconds, while DNS-SD is still filling
			// the list in, then once a second.
			const bool warming = std::chrono::steady_clock::now() - started < std::chrono::seconds( 3 );
			wake.wait_for( lock, std::chrono::milliseconds( warming ? 250 : 1000 ), [ this ] { return stop; } );
			if( stop )
				return;
		}
	}
};

Discovery& Discovery::instance()
{
	static Discovery* leaked = new Discovery();
	return *leaked;
}

Discovery::State& Discovery::s() const
{
	// Constructed on first use and never freed, like the instance.
	static State* leaked = new State();
	return *leaked;
}

void Discovery::subscribe()
{
	State& st = s();
	std::thread stale;
	{
		std::lock_guard< std::mutex > lock( st.mutex );
		if( ++st.subscribers > 1 || st.running )
			return;
		if( st.thread.joinable() )
			stale = std::move( st.thread );
	}
	if( stale.joinable() )
		stale.join();
	std::lock_guard< std::mutex > lock( st.mutex );
	if( st.subscribers == 0 || st.running )
		return;// left again, or another subscriber started it, while we joined
	st.stop = false;
	st.running = true;
	st.thread = std::thread( [ &st ] { st.run(); } );
}

void Discovery::unsubscribe()
{
	State& st = s();
	std::thread finished;
	{
		std::lock_guard< std::mutex > lock( st.mutex );
		if( st.subscribers == 0 || --st.subscribers > 0 )
			return;
		st.stop = true;
		st.running = false;
		finished = std::move( st.thread );
	}
	st.wake.notify_all();
	if( finished.joinable() )
		finished.join();
}

std::vector< std::string > Discovery::sources( uint64_t* generation ) const
{
	State& st = s();
	std::lock_guard< std::mutex > lock( st.mutex );
	if( generation )
		*generation = st.generation;
	return st.names;
}

uint64_t Discovery::generation() const
{
	State& st = s();
	std::lock_guard< std::mutex > lock( st.mutex );
	return st.generation;
}

std::string Discovery::runtimeError() const
{
	State& st = s();
	std::lock_guard< std::mutex > lock( st.mutex );
	return st.runtimeError;
}

bool Discovery::runtimeReady() const
{
	State& st = s();
	std::lock_guard< std::mutex > lock( st.mutex );
	return st.runtimeReady;
}

float sourceKey( const std::string& name )
{
	uint32_t hash = 2166136261u;
	for( unsigned char c : name )
	{
		hash ^= c;
		hash *= 16777619u;
	}
	const uint32_t folded = ( hash >> 23 ) ^ ( hash & 0x7FFFFFu );
	return static_cast< float >( folded & 0x7FFFFFu ) + kKeyBase;
}

} // namespace somt
