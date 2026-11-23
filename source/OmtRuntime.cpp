#include "OmtRuntime.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "Diag.h"

#if defined( _WIN32 )
	#include <windows.h>
#else
	#include <dlfcn.h>
	#if defined( __APPLE__ )
		#include <mach-o/dyld.h>
	#endif
#endif

namespace somt
{
namespace
{

#if defined( __APPLE__ )
constexpr const char* kLibraryLeaf = "libomt.dylib";
#elif defined( _WIN32 )
constexpr const char* kLibraryLeaf = "libomt.dll";
#else
constexpr const char* kLibraryLeaf = "libomt.so";
#endif

bool endsWith( const std::string& s, const std::string& suffix )
{
	return s.size() >= suffix.size() && s.compare( s.size() - suffix.size(), suffix.size(), suffix ) == 0;
}

std::string parentDirectory( const std::string& path )
{
	const size_t slash = path.find_last_of( "/\\" );
	return slash == std::string::npos ? std::string( "." ) : path.substr( 0, slash );
}

/// The directory holding the binary this code is linked into: a bundle's
/// Contents/MacOS, a plugin DLL's folder, or the harness's build directory.
std::string ownImageDirectory()
{
#if defined( _WIN32 )
	HMODULE module = nullptr;
	if( !GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                         reinterpret_cast< LPCSTR >( &ownImageDirectory ), &module ) )
		return {};
	char path[ MAX_PATH ] = {};
	const DWORD n = GetModuleFileNameA( module, path, MAX_PATH );
	return n ? parentDirectory( std::string( path, n ) ) : std::string();
#else
	Dl_info info {};
	if( !dladdr( reinterpret_cast< void* >( &ownImageDirectory ), &info ) || !info.dli_fname )
		return {};
	return parentDirectory( info.dli_fname );
#endif
}

class Runtime
{
public:
	static Runtime& instance()
	{
		static Runtime runtime;
		return runtime;
	}

	const OmtApi* acquire( std::string* error )
	{
		std::lock_guard< std::mutex > lock( mutex );
		if( !attempted )
		{
			attempted = true;
			load();
			if( ok )
				ready.store( true, std::memory_order_release );
		}
		if( !ok )
		{
			if( error )
				*error = failure;
			return nullptr;
		}
		return &api;
	}

	const OmtApi* ifLoaded() const
	{
		return ready.load( std::memory_order_acquire ) ? &api : nullptr;
	}

	std::string path()
	{
		std::lock_guard< std::mutex > lock( mutex );
		return loadedFrom;
	}

	bool wasShared()
	{
		std::lock_guard< std::mutex > lock( mutex );
		return shared;
	}

private:
	void* findAlreadyLoaded()
	{
#if defined( __APPLE__ )
		const uint32_t count = _dyld_image_count();
		for( uint32_t i = 0; i < count; ++i )
		{
			const char* name = _dyld_get_image_name( i );
			if( !name || !endsWith( name, std::string( "/" ) + kLibraryLeaf ) )
				continue;
			// RTLD_NOLOAD: take a reference on the image that is there, never
			// open a new one by accident.
			if( void* handle = dlopen( name, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD ) )
			{
				loadedFrom = name;
				return handle;
			}
		}
		return nullptr;
#elif defined( _WIN32 )
		HMODULE module = GetModuleHandleA( kLibraryLeaf );
		if( !module )
			return nullptr;
		char path[ MAX_PATH ] = {};
		const DWORD n = GetModuleFileNameA( module, path, MAX_PATH );
		loadedFrom = n ? std::string( path, n ) : std::string( kLibraryLeaf );
		// Take our own reference so it outlives whoever loaded it.
		HMODULE ours = nullptr;
		GetModuleHandleExA( 0, kLibraryLeaf, &ours );
		return ours ? ours : module;
#else
		return nullptr;
#endif
	}

	std::vector< std::string > candidates() const
	{
		std::vector< std::string > list;
		if( const char* forced = std::getenv( "RESOLUME_OMT_LIBRARY" ) )
			if( *forced )
				list.push_back( forced );

		const std::string own = ownImageDirectory();
		if( !own.empty() )
		{
#if defined( __APPLE__ )
			list.push_back( own + "/../Frameworks/" + kLibraryLeaf );// inside a .bundle
#endif
			list.push_back( own + "/" + kLibraryLeaf );// beside the binary (DLL, harness)
		}
#if defined( __APPLE__ )
		if( const char* home = std::getenv( "HOME" ) )
			list.push_back( std::string( home ) + "/.local/lib/omt/" + kLibraryLeaf );
		list.push_back( std::string( "/usr/local/lib/" ) + kLibraryLeaf );
#endif
		return list;
	}

	void* openFile( const std::string& path, std::string& why )
	{
#if defined( _WIN32 )
		// ALTERED_SEARCH_PATH so libvmx.dll and libomtnet.dll beside it are
		// found from its own folder rather than Resolume's.
		HMODULE module = LoadLibraryExA( path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH );
		if( !module )
			why = "LoadLibrary error " + std::to_string( GetLastError() );
		return module;
#else
		void* handle = dlopen( path.c_str(), RTLD_NOW | RTLD_LOCAL );
		if( !handle )
		{
			const char* message = dlerror();
			why = message ? message : "dlopen failed";
		}
		return handle;
#endif
	}

	void* symbol( const char* name )
	{
#if defined( _WIN32 )
		return reinterpret_cast< void* >( GetProcAddress( static_cast< HMODULE >( handle ), name ) );
#else
		return dlsym( handle, name );
#endif
	}

	void load()
	{
		handle = findAlreadyLoaded();
		shared = handle != nullptr;

		std::string tried;
		if( !handle )
		{
			for( const std::string& path : candidates() )
			{
				std::string why;
				handle = openFile( path, why );
				if( handle )
				{
					loadedFrom = path;
					break;
				}
				// A missing file is the normal case for every candidate but one;
				// only a file that exists and will not load is worth reporting.
				if( why.find( "no such file" ) == std::string::npos && why.find( "error 126" ) == std::string::npos )
					tried += "\n  " + path + ": " + why;
			}
		}
		if( !handle )
		{
			failure = std::string( "OMT runtime not found: " ) + kLibraryLeaf
			          + " should be in the plugin bundle's Contents/Frameworks folder" + tried;
			diag::error( failure );
			return;
		}

		struct Entry
		{
			const char* name;
			void** slot;
			bool required;
		};
		const Entry entries[] = {
			{ "omt_discovery_getaddresses", reinterpret_cast< void** >( &api.discovery_getaddresses ), true },
			{ "omt_receive_create", reinterpret_cast< void** >( &api.receive_create ), true },
			{ "omt_receive_destroy", reinterpret_cast< void** >( &api.receive_destroy ), true },
			{ "omt_receive", reinterpret_cast< void** >( &api.receive ), true },
			{ "omt_receive_getsenderinformation", reinterpret_cast< void** >( &api.receive_getsenderinformation ), false },
			{ "omt_receive_getvideostatistics", reinterpret_cast< void** >( &api.receive_getvideostatistics ), false },
			{ "omt_send_create", reinterpret_cast< void** >( &api.send_create ), true },
			{ "omt_send_destroy", reinterpret_cast< void** >( &api.send_destroy ), true },
			{ "omt_send", reinterpret_cast< void** >( &api.send ), true },
			{ "omt_send_connections", reinterpret_cast< void** >( &api.send_connections ), true },
			{ "omt_send_getaddress", reinterpret_cast< void** >( &api.send_getaddress ), true },
			{ "omt_send_setsenderinformation", reinterpret_cast< void** >( &api.send_setsenderinformation ), false },
			{ "omt_send_getvideostatistics", reinterpret_cast< void** >( &api.send_getvideostatistics ), false },
		};
		for( const Entry& entry : entries )
		{
			*entry.slot = symbol( entry.name );
			if( !*entry.slot && entry.required )
			{
				failure = std::string( "OMT runtime at " ) + loadedFrom + " is missing " + entry.name;
				diag::error( failure );
				return;
			}
		}

		// libomt fails quietly (a send that cannot encode just does nothing),
		// so its own log is the only window in. Its discovery chatter is heavy,
		// so it is opt-in.
		if( std::getenv( "RESOLUME_OMT_VERBOSE" ) )
		{
			if( void* setCallback = symbol( "omt_setloggingcallback" ) )
				reinterpret_cast< void ( * )( OMTLoggingCallback ) >( setCallback )(
					[]( const char* line ) { diag::info( std::string( "[libomt] " ) + ( line ? line : "" ) ); } );
		}

		ok = true;
		diag::info( std::string( "OMT runtime " ) + ( shared ? "shared from " : "loaded from " ) + loadedFrom );
	}

	std::mutex mutex;
	std::atomic< bool > ready { false };
	bool attempted = false;
	bool ok = false;
	bool shared = false;
	void* handle = nullptr;
	OmtApi api;
	std::string loadedFrom;
	std::string failure;
};

} // namespace

const OmtApi* omtApi( std::string* error )
{
	return Runtime::instance().acquire( error );
}

const OmtApi* omtApiIfLoaded()
{
	return Runtime::instance().ifLoaded();
}

std::string omtRuntimePath()
{
	return Runtime::instance().path();
}

bool omtRuntimeWasShared()
{
	return Runtime::instance().wasShared();
}

} // namespace somt
