// omttest: the offline harness for SW OMT In and SW OMT Out.
//
// It drives the real plugin classes (OmtIn, OmtOut), not copies, in a
// headless GL context, and checks them against libomt's own sender and
// receiver: an independent implementation of both ends of the wire. Every
// check prints PASS or FAIL and a measured number; the exit status is the
// number of failures.
//
//   omttest                run everything
//   omttest --only NAME    run one check (see the list printed by --list)
//   omttest --list
//
// It sends real OMT sources on this Mac while it runs, named
// "omttest-<pid>-...", which other machines on the network can see.

#include <OpenGL/OpenGL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <mach-o/dyld.h>
#include <unistd.h>

#include "Discovery.h"
#include "OmtIn.h"
#include "OmtOut.h"
#include "OmtRuntime.h"

namespace
{
using clock_ = std::chrono::steady_clock;

int failures = 0;
void report( bool ok, const std::string& name, const std::string& detail )
{
	std::printf( "%s  %-22s %s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str() );
	std::fflush( stdout );
	if( !ok )
		++failures;
}

std::string fmt( const char* f, double v )
{
	char b[ 64 ];
	std::snprintf( b, sizeof( b ), f, v );
	return b;
}

void sleepMs( int ms )
{
	std::this_thread::sleep_for( std::chrono::milliseconds( ms ) );
}

bool libomtLoaded()
{
	for( uint32_t i = 0; i < _dyld_image_count(); ++i )
	{
		const char* n = _dyld_get_image_name( i );
		if( n && std::strstr( n, "/libomt.dylib" ) )
			return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// A headless GL 4.1 core context: what Resolume gives a plugin on macOS.
// ---------------------------------------------------------------------------
bool makeContext()
{
	CGLPixelFormatAttribute attributes[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_3_2_Core ),
		kCGLPFAColorSize,     static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize,     static_cast< CGLPixelFormatAttribute >( 8 ),
		kCGLPFAAccelerated,   kCGLPFAAllowOfflineRenderers,
		static_cast< CGLPixelFormatAttribute >( 0 ) };
	CGLPixelFormatObj format = nullptr;
	GLint count = 0;
	if( CGLChoosePixelFormat( attributes, &format, &count ) != kCGLNoError || !format )
		return false;
	CGLContextObj context = nullptr;
	CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	return context && CGLSetCurrentContext( context ) == kCGLNoError;
}

struct Target
{
	GLuint fbo = 0, texture = 0;
	int w = 0, h = 0;
};

Target makeTarget( int w, int h )
{
	Target t;
	t.w = w;
	t.h = h;
	glGenTextures( 1, &t.texture );
	glBindTexture( GL_TEXTURE_2D, t.texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glGenFramebuffers( 1, &t.fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, t.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.texture, 0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return t;
}

void freeTarget( Target& t )
{
	glDeleteFramebuffers( 1, &t.fbo );
	glDeleteTextures( 1, &t.texture );
	t = Target {};
}

void bindTarget( const Target& t )
{
	glBindFramebuffer( GL_FRAMEBUFFER, t.fbo );
	glViewport( 0, 0, t.w, t.h );
}

/// RGBA, top row first.
std::vector< uint8_t > readTarget( const Target& t )
{
	std::vector< uint8_t > bottomFirst( static_cast< size_t >( t.w ) * t.h * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, t.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 4 );
	glReadPixels( 0, 0, t.w, t.h, GL_RGBA, GL_UNSIGNED_BYTE, bottomFirst.data() );
	std::vector< uint8_t > top( bottomFirst.size() );
	const size_t row = static_cast< size_t >( t.w ) * 4;
	for( int y = 0; y < t.h; ++y )
		std::memcpy( &top[ y * row ], &bottomFirst[ ( t.h - 1 - y ) * row ], row );
	return top;
}

/// Upload RGBA (top row first) as a host texture (bottom row first).
void uploadTexture( GLuint texture, const std::vector< uint8_t >& rgbaTop, int w, int h )
{
	std::vector< uint8_t > bottom( rgbaTop.size() );
	const size_t row = static_cast< size_t >( w ) * 4;
	for( int y = 0; y < h; ++y )
		std::memcpy( &bottom[ y * row ], &rgbaTop[ ( h - 1 - y ) * row ], row );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, bottom.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glBindTexture( GL_TEXTURE_2D, 0 );
}

// ---------------------------------------------------------------------------
// The test card: 75% bars on top (so orientation is checkable), a grey ramp,
// and the frame number as 16 big black/white blocks along the bottom, which
// survive VMX and tell latency in frames. Optional alpha per bar.
// ---------------------------------------------------------------------------
const uint8_t kBars[ 8 ][ 3 ] = { { 191, 191, 191 }, { 191, 191, 0 }, { 0, 191, 191 }, { 0, 191, 0 },
	                              { 191, 0, 191 },   { 191, 0, 0 },   { 0, 0, 191 },   { 0, 0, 0 } };
uint8_t barAlpha( int bar )
{
	return static_cast< uint8_t >( 255 - bar * 28 );// 255 .. 59
}

/// RGBA top row first, straight alpha.
std::vector< uint8_t > card( int w, int h, uint32_t frame, bool withAlpha )
{
	std::vector< uint8_t > p( static_cast< size_t >( w ) * h * 4 );
	for( int y = 0; y < h; ++y )
		for( int x = 0; x < w; ++x )
		{
			uint8_t* q = &p[ ( static_cast< size_t >( y ) * w + x ) * 4 ];
			uint8_t r, g, b, a = 255;
			if( y < h / 2 )
			{
				const int bar = std::min( 7, x * 8 / w );
				r = kBars[ bar ][ 0 ];
				g = kBars[ bar ][ 1 ];
				b = kBars[ bar ][ 2 ];
				if( withAlpha )
					a = barAlpha( bar );
			}
			else if( y < 3 * h / 4 )
			{
				r = g = b = static_cast< uint8_t >( x * 255 / ( w - 1 ) );
			}
			else
			{
				const int bit = std::min( 15, x * 16 / w );
				r = g = b = ( frame >> bit ) & 1u ? 235 : 16;
			}
			q[ 0 ] = r;
			q[ 1 ] = g;
			q[ 2 ] = b;
			q[ 3 ] = a;
		}
	return p;
}

/// The host's textures are premultiplied.
std::vector< uint8_t > premultiply( std::vector< uint8_t > p )
{
	for( size_t i = 0; i < p.size(); i += 4 )
		for( int c = 0; c < 3; ++c )
			p[ i + c ] = static_cast< uint8_t >( std::lround( p[ i + c ] * p[ i + 3 ] / 255.0 ) );
	return p;
}

/// Mean of a 9x9 patch of channel c, from RGBA or BGRA (top first).
double patch( const uint8_t* p, int stride, int cx, int cy, int c )
{
	double sum = 0;
	for( int y = cy - 4; y <= cy + 4; ++y )
		for( int x = cx - 4; x <= cx + 4; ++x )
			sum += p[ static_cast< size_t >( y ) * stride + x * 4 + c ];
	return sum / 81.0;
}

/// The frame number from a card, or -1 if a block is ambiguous.
long decodeCounter( const uint8_t* p, int stride, int w, int h, int greenChannel )
{
	uint32_t v = 0;
	for( int bit = 0; bit < 16; ++bit )
	{
		const int cx = ( 2 * bit + 1 ) * w / 32;
		const double g = patch( p, stride, cx, 7 * h / 8, greenChannel );
		if( g > 170 )
			v |= 1u << bit;
		else if( g > 80 )
			return -1;
	}
	return static_cast< long >( v );
}

struct BarError
{
	double worst = 0;// largest channel error at a bar centre, levels
	bool upright = true;
};

/// Compare the bars of a received/rendered card with the source.
/// layout: 0 = RGBA, 1 = BGRA. Colour premultiplied by alpha when asked.
BarError checkBars( const uint8_t* p, int stride, int w, int h, int layout, bool alphaExpected, bool premultiplied )
{
	BarError e;
	const int R = layout == 0 ? 0 : 2, G = 1, B = layout == 0 ? 2 : 0, A = 3;
	for( int bar = 0; bar < 8; ++bar )
	{
		const int cx = ( 2 * bar + 1 ) * w / 16;
		const int cy = h / 4;
		const double a = alphaExpected ? barAlpha( bar ) : 255.0;
		const double scale = premultiplied ? a / 255.0 : 1.0;
		const double want[ 3 ] = { kBars[ bar ][ 0 ] * scale, kBars[ bar ][ 1 ] * scale, kBars[ bar ][ 2 ] * scale };
		const double got[ 3 ] = { patch( p, stride, cx, cy, R ), patch( p, stride, cx, cy, G ), patch( p, stride, cx, cy, B ) };
		for( int c = 0; c < 3; ++c )
			e.worst = std::max( e.worst, std::fabs( got[ c ] - want[ c ] ) );
		e.worst = std::max( e.worst, std::fabs( patch( p, stride, cx, cy, A ) - a ) );
	}
	// Upright: white bar top left, and the grey ramp below the bars dark on
	// the left and bright on the right. Flipped, the ramp's row would land in
	// the bars (white on the left). Independent of the counter's value.
	const double topLuma = patch( p, stride, w / 16, h / 4, G );
	const double rampLeft = patch( p, stride, w / 32, 5 * h / 8, G );
	const double rampRight = patch( p, stride, w - w / 32, 5 * h / 8, G );
	e.upright = topLuma > 150 && rampLeft < 60 && rampRight > 200;
	return e;
}

// ---------------------------------------------------------------------------
// Mini host: feeds a plugin exactly what ProcessOpenGL gets from Resolume.
// ---------------------------------------------------------------------------
struct Host
{
	Target out;
	GLuint input = 0;
	int inW = 0, inH = 0;

	void processEffect( CFFGLPlugin& plugin )
	{
		FFGLTextureStruct tex {};
		tex.Width = tex.HardwareWidth = static_cast< FFUInt32 >( inW );
		tex.Height = tex.HardwareHeight = static_cast< FFUInt32 >( inH );
		tex.Handle = input;
		FFGLTextureStruct* list[ 1 ] = { &tex };
		ProcessOpenGLStruct p {};
		p.numInputTextures = 1;
		p.inputTextures = list;
		p.HostFBO = out.fbo;
		bindTarget( out );
		plugin.ProcessOpenGL( &p );
	}
	void processSource( CFFGLPlugin& plugin )
	{
		ProcessOpenGLStruct p {};
		p.HostFBO = out.fbo;
		bindTarget( out );
		plugin.ProcessOpenGL( &p );
	}
};

template< class Plugin >
void initPlugin( Plugin& plugin, int w, int h )
{
	FFGLViewportStruct vp { 0, 0, static_cast< GLuint >( w ), static_cast< GLuint >( h ) };
	plugin.InitGL( &vp );
}

std::string prefix()
{
	return "omttest-" + std::to_string( getpid() );
}

const somt::OmtApi* api()
{
	std::string error;
	const somt::OmtApi* a = somt::omtApi( &error );
	if( !a )
	{
		std::printf( "OMT runtime: %s\n", error.c_str() );
		std::exit( 2 );
	}
	return a;
}

/// Wait until the OMT Out is sending and has an address.
std::string waitForAddress( OmtOut& out, Host& host, int timeoutMs )
{
	const auto until = clock_::now() + std::chrono::milliseconds( timeoutMs );
	while( clock_::now() < until )
	{
		host.processEffect( out );
		const somt::SenderStatus st = out.senderStatus();
		if( !st.address.empty() && st.state == somt::SenderStatus::State::Sending )
			return st.address;
		sleepMs( 16 );
	}
	return {};
}

// ---------------------------------------------------------------------------
// The checks.
// ---------------------------------------------------------------------------

/// Arena constructs every plugin at its startup scan, then destroys it. That
/// must start nothing: no runtime, no thread, no network.
void checkScan()
{
	const bool before = libomtLoaded();
	for( int i = 0; i < 8; ++i )
	{
		OmtIn in;
		OmtOut out;
		in.SetTextParameter( OmtIn::P_ADDRESS, "SOMEWHERE (Anything)" );
		out.SetTextParameter( OmtOut::P_NAME, "Scan" );
	}
	const bool after = libomtLoaded();
	report( !before && !after, "scan.lazy",
	        std::string( "construct+set+destroy x8 without a frame: libomt " ) + ( after ? "LOADED" : "not loaded" ) );
}

void checkNames()
{
	OmtIn in;
	OmtOut out;
	bool ok = true;
	std::string detail;
	auto check = [ & ]( CFFGLPlugin& plugin, unsigned int count, const char* who ) {
		std::set< std::string > seen;
		for( unsigned int i = 0; i < count; ++i )
		{
			const char* raw = plugin.GetParamName( i );
			std::string name = raw ? std::string( raw, strnlen( raw, 16 ) ) : "";
			std::string key;
			for( char c : name )
				if( c != ' ' )
					key += static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
			if( name.empty() || !seen.insert( key ).second )
			{
				ok = false;
				detail += std::string( who ) + " clash/empty at " + std::to_string( i ) + " ";
			}
		}
		detail += std::string( who ) + " " + std::to_string( seen.size() ) + " unique; ";
	};
	check( in, OmtIn::P_COUNT, "In" );
	check( out, OmtOut::P_COUNT, "Out" );
	report( ok, "names.unique", detail );
}

/// OMT Out -> libomt's receiver: geometry, colour, alpha, frame order.
void checkOut( int w, int h, bool alpha, const char* label )
{
	Host host;
	host.out = makeTarget( w, h );
	host.inW = w;
	host.inH = h;
	glGenTextures( 1, &host.input );

	OmtOut out;
	initPlugin( out, w, h );
	out.SetTextParameter( OmtOut::P_NAME, ( prefix() + "-" + label ).c_str() );
	out.SetFloatParameter( OmtOut::P_ALPHA, alpha ? 1.0f : 0.0f );
	uploadTexture( host.input, premultiply( card( w, h, 0, alpha ) ), w, h );

	const std::string address = waitForAddress( out, host, 5000 );
	if( address.empty() )
	{
		report( false, std::string( "out." ) + label, "never started sending" );
		return;
	}

	const somt::OmtApi* a = api();
	omt_receive_t* rx = a->receive_create( address.c_str(), OMTFrameType_Video, OMTPreferredVideoFormat_BGRA,
	                                       OMTReceiveFlags_None );
	BarError worst;
	worst.worst = 0;
	bool alphaFlag = false, sizeOk = true, orderOk = true;
	int got = 0;
	long lastCounter = -1;
	const auto until = clock_::now() + std::chrono::seconds( 6 );
	for( uint32_t f = 0; clock_::now() < until && got < 60; ++f )
	{
		uploadTexture( host.input, premultiply( card( w, h, f * 2, alpha ) ), w, h );
		host.processEffect( out );
		// The pass-through must be the input, untouched.
		if( f == 3 )
		{
			const std::vector< uint8_t > through = readTarget( host.out );
			const std::vector< uint8_t > want = premultiply( card( w, h, f * 2, alpha ) );
			int diff = 0;
			for( size_t i = 0; i < want.size(); ++i )
				diff = std::max( diff, std::abs( static_cast< int >( through[ i ] ) - static_cast< int >( want[ i ] ) ) );
			report( diff == 0, std::string( "out." ) + label + ".pass", "pass-through max diff " + std::to_string( diff ) + " levels" );
		}
		OMTMediaFrame* m = a->receive( rx, OMTFrameType_Video, 30 );
		if( !m )
			continue;
		++got;
		sizeOk = sizeOk && m->Width == w && m->Height == h;
		if( !sizeOk )
			break;
		alphaFlag = ( m->Flags & OMTVideoFlags_Alpha ) != 0;
		const uint8_t* px = static_cast< const uint8_t* >( m->Data );
		if( got > 5 )
		{
			const BarError e = checkBars( px, m->Stride, w, h, 1, alpha, false );
			worst.worst = std::max( worst.worst, e.worst );
			worst.upright = worst.upright && e.upright;
		}
		const long counter = decodeCounter( px, m->Stride, w, h, 1 );
		if( std::getenv( "OMTTEST_TRACE" ) )
			std::printf( "  rx f=%u counter=%ld ts=%lld\n", f, counter, static_cast< long long >( m->Timestamp ) );
		if( counter >= 0 )
		{
			// Never backwards. Repeats are allowed: the warm-up card (frame 0)
			// is still in the pipe for the first few frames.
			if( lastCounter >= 0 && counter < lastCounter )
				orderOk = false;
			lastCounter = counter;
		}
		sleepMs( 10 );
	}
	a->receive_destroy( rx );
	const somt::SenderStatus st = out.senderStatus();
	report( got >= 30 && sizeOk, std::string( "out." ) + label + ".frames",
	        std::to_string( got ) + " frames received at " + std::to_string( w ) + "x" + std::to_string( h ) + ", encode "
	            + fmt( "%.2f ms", st.encodeMs ) + ", " + std::to_string( st.dropped ) + " dropped by the sender" );
	report( worst.upright, std::string( "out." ) + label + ".upright", worst.upright ? "bars on top, counter at the bottom" : "UPSIDE DOWN" );
	report( worst.worst <= 10.0, std::string( "out." ) + label + ".colour", "worst bar-centre error " + fmt( "%.1f levels", worst.worst ) + " (limit 10)" );
	report( orderOk && lastCounter > 40, std::string( "out." ) + label + ".order", "frame counter never went backwards, reached " + std::to_string( lastCounter ) );
	report( alphaFlag == alpha, std::string( "out." ) + label + ".alphaflag", std::string( "Alpha flag " ) + ( alphaFlag ? "set" : "clear" ) );

	out.DeInitGL();
	glDeleteTextures( 1, &host.input );
	freeTarget( host.out );
}

/// libomt's sender -> OMT In: orientation, colour, alpha, fit.
void checkIn( int srcW, int srcH, int outW, int outH, bool alpha, int fit, const char* label )
{
	const somt::OmtApi* a = api();
	const std::string name = prefix() + "-src-" + label;
	omt_send_t* tx = a->send_create( name.c_str(), OMTQuality_Default );
	char buffer[ 512 ] = {};
	a->send_getaddress( tx, buffer, sizeof( buffer ) );
	const std::string address = buffer;

	Host host;
	host.out = makeTarget( outW, outH );
	OmtIn in;
	initPlugin( in, outW, outH );
	in.SetFloatParameter( OmtIn::P_FIT, static_cast< float >( fit ) );
	in.SetTextParameter( OmtIn::P_ADDRESS, address.c_str() );

	auto sendCard = [ & ]( uint32_t f ) {
		std::vector< uint8_t > rgba = card( srcW, srcH, f, alpha );
		for( size_t i = 0; i < rgba.size(); i += 4 )
			std::swap( rgba[ i ], rgba[ i + 2 ] );// to BGRA, straight alpha
		OMTMediaFrame frame {};
		frame.Type = OMTFrameType_Video;
		frame.Codec = OMTCodec_BGRA;
		frame.Width = srcW;
		frame.Height = srcH;
		frame.Stride = srcW * 4;
		frame.Flags = alpha ? OMTVideoFlags_Alpha : OMTVideoFlags_None;
		frame.FrameRateN = 60;
		frame.FrameRateD = 1;
		frame.AspectRatio = static_cast< float >( srcW ) / srcH;
		frame.ColorSpace = srcH >= 720 ? OMTColorSpace_BT709 : OMTColorSpace_BT601;
		frame.Timestamp = -1;
		frame.Data = rgba.data();
		frame.DataLength = static_cast< int >( rgba.size() );
		a->send( tx, &frame );
	};

	const auto until = clock_::now() + std::chrono::seconds( 6 );
	uint32_t f = 0;
	while( clock_::now() < until && in.framesUploaded() < 20 )
	{
		sendCard( f++ * 2 );
		host.processSource( in );
		sleepMs( 16 );
	}
	const std::vector< uint8_t > shown = readTarget( host.out );
	const std::string status = in.statusText();
	report( in.framesUploaded() >= 20, std::string( "in." ) + label + ".frames",
	        std::to_string( in.framesUploaded() ) + " frames uploaded; status \"" + status + "\"" );

	// Where the picture should land in the output.
	int x0 = 0, y0 = 0, x1 = outW, y1 = outH;
	const double fa = static_cast< double >( srcW ) / srcH, oa = static_cast< double >( outW ) / outH;
	if( fit == OmtIn::FIT_FIT )
	{
		if( fa < oa )
		{
			const int pw = static_cast< int >( std::lround( outH * fa ) );
			x0 = ( outW - pw ) / 2;
			x1 = x0 + pw;
		}
		else
		{
			const int ph = static_cast< int >( std::lround( outW / fa ) );
			y0 = ( outH - ph ) / 2;
			y1 = y0 + ph;
		}
	}
	const int pw = x1 - x0, ph = y1 - y0;
	std::vector< uint8_t > inner( static_cast< size_t >( pw ) * ph * 4 );
	for( int y = 0; y < ph; ++y )
		std::memcpy( &inner[ static_cast< size_t >( y ) * pw * 4 ], &shown[ ( static_cast< size_t >( y0 + y ) * outW + x0 ) * 4 ],
		             static_cast< size_t >( pw ) * 4 );
	const BarError e = checkBars( inner.data(), pw * 4, pw, ph, 0, alpha, true );
	report( e.upright, std::string( "in." ) + label + ".upright", e.upright ? "bars on top, counter at the bottom" : "UPSIDE DOWN" );
	report( e.worst <= 10.0, std::string( "in." ) + label + ".colour", "worst bar-centre error " + fmt( "%.1f levels", e.worst ) + " (premultiplied; limit 10)" );
	if( fit == OmtIn::FIT_FIT && ( x0 > 0 || y0 > 0 ) )
	{
		// The bars must be clear, and the picture's edge where Fit puts it.
		const double barA = patch( shown.data(), outW * 4, std::max( 4, x0 / 2 ), std::max( 4, y0 / 2 + 4 ), 3 );
		const double inA = patch( shown.data(), outW * 4, x0 + 8, y0 + ph / 4, 3 );
		report( barA == 0.0 && inA > 250.0, std::string( "in." ) + label + ".bars",
		        "outside alpha " + fmt( "%.0f", barA ) + ", inside " + fmt( "%.0f", inA ) + "; picture " + std::to_string( pw ) + "x"
		            + std::to_string( ph ) + " at " + std::to_string( x0 ) + "," + std::to_string( y0 ) );
	}

	// No signal: stop sending. Let what is already on the wire arrive, then
	// Hold must keep that frame, unchanged, through the loss.
	auto renderFor = [ & ]( int ms ) {
		const auto end = clock_::now() + std::chrono::milliseconds( ms );
		while( clock_::now() < end )
		{
			host.processSource( in );
			sleepMs( 16 );
		}
	};
	renderFor( 300 );
	const std::vector< uint8_t > last = readTarget( host.out );
	renderFor( 1200 );
	const std::vector< uint8_t > held = readTarget( host.out );
	const bool same = held == last;
	report( same && in.statusText().rfind( "No signal", 0 ) == 0, std::string( "in." ) + label + ".hold",
	        std::string( "1.2 s into a loss: " ) + ( same ? "last frame held bit for bit" : "frame CHANGED" ) + ", status \"" + in.statusText() + "\"" );

	// Black instead of Hold: an opaque black frame during the loss.
	in.SetFloatParameter( OmtIn::P_NO_SIGNAL, static_cast< float >( OmtIn::NS_BLACK ) );
	renderFor( 50 );
	const std::vector< uint8_t > blacked = readTarget( host.out );
	bool allBlack = true;
	for( size_t i = 0; i < blacked.size() && allBlack; i += 4 )
		allBlack = blacked[ i ] == 0 && blacked[ i + 1 ] == 0 && blacked[ i + 2 ] == 0 && blacked[ i + 3 ] == 255;
	report( allBlack, std::string( "in." ) + label + ".black", allBlack ? "No Signal = Black: every pixel opaque black" : "not all opaque black" );

	in.DeInitGL();
	freeTarget( host.out );
	a->send_destroy( tx );
}

/// OMT Out -> OMT In in one process: discovery, the menu, latency.
void checkLoop()
{
	const int w = 1920, h = 1080;
	Host outHost, inHost;
	outHost.out = makeTarget( w, h );
	outHost.inW = w;
	outHost.inH = h;
	glGenTextures( 1, &outHost.input );
	inHost.out = makeTarget( w, h );

	OmtOut out;
	initPlugin( out, w, h );
	const std::string name = prefix() + "-loop";
	out.SetTextParameter( OmtOut::P_NAME, name.c_str() );
	uploadTexture( outHost.input, premultiply( card( w, h, 0, false ) ), w, h );
	const std::string address = waitForAddress( out, outHost, 5000 );

	OmtIn in;
	initPlugin( in, w, h );
	// Find it the way an operator would: in the menu.
	const auto t0 = clock_::now();
	float key = -1.0f;
	while( clock_::now() - t0 < std::chrono::seconds( 10 ) && key < 0 )
	{
		outHost.processEffect( out );
		inHost.processSource( in );
		const auto& names = in.menuNames();
		for( size_t i = 0; i < names.size(); ++i )
			if( names[ i ] == address )
				key = in.menuValues()[ i ];
		sleepMs( 16 );
	}
	const double found = std::chrono::duration< double >( clock_::now() - t0 ).count();
	report( key >= somt::kKeyBase, "loop.discovery",
	        key >= 0 ? "\"" + address + "\" in the Source menu after " + fmt( "%.2f s", found ) : "never appeared in the menu" );
	if( key < 0 )
		return;

	in.SetFloatParameter( OmtIn::P_SOURCE, key );
	char* typed = in.GetTextParameter( OmtIn::P_ADDRESS );
	report( typed && address == typed, "loop.menu-sets-address", std::string( "Address now \"" ) + ( typed ? typed : "" ) + "\"" );

	// Latency, in host frames at 60 Hz: what number is shown when frame N is drawn.
	std::vector< long > lags;
	uint32_t f = 0;
	const auto until = clock_::now() + std::chrono::seconds( 8 );
	while( clock_::now() < until && lags.size() < 120 )
	{
		const auto frameStart = clock_::now();
		uploadTexture( outHost.input, premultiply( card( w, h, f, false ) ), w, h );
		outHost.processEffect( out );
		inHost.processSource( in );
		if( in.framesUploaded() > 10 )
		{
			const std::vector< uint8_t > shown = readTarget( inHost.out );
			const long counter = decodeCounter( shown.data(), w * 4, w, h, 1 );
			if( counter >= 0 )
				lags.push_back( static_cast< long >( f ) - counter );
		}
		++f;
		std::this_thread::sleep_until( frameStart + std::chrono::microseconds( 16667 ) );
	}
	std::sort( lags.begin(), lags.end() );
	const long median = lags.empty() ? -1 : lags[ lags.size() / 2 ];
	report( !lags.empty() && median >= 0 && median <= 4, "loop.latency",
	        lags.empty() ? "no readable frames"
	                     : "median " + std::to_string( median ) + " frames at 60 Hz (" + std::to_string( lags.size() ) + " samples, min "
	                           + std::to_string( lags.front() ) + ", max " + std::to_string( lags.back() ) + ")" );

	// Send off: the source must leave the network.
	out.SetFloatParameter( OmtOut::P_SEND, 0.0f );
	const auto t1 = clock_::now();
	bool gone = false;
	while( clock_::now() - t1 < std::chrono::seconds( 10 ) && !gone )
	{
		outHost.processEffect( out );
		const auto list = somt::Discovery::instance().sources();
		gone = std::find( list.begin(), list.end(), address ) == list.end();
		sleepMs( 50 );
	}
	report( gone, "loop.send-off", gone ? "source left discovery " + fmt( "%.2f s", std::chrono::duration< double >( clock_::now() - t1 ).count() ) + " after Send off"
	                                    : "still listed 10 s after Send off" );

	out.DeInitGL();
	in.DeInitGL();
	glDeleteTextures( 1, &outHost.input );
	freeTarget( outHost.out );
	freeTarget( inHost.out );
}

/// Two OMT Outs asking for the same name must not collide.
void checkUniqueNames()
{
	const int w = 320, h = 180;
	Host host;
	host.out = makeTarget( w, h );
	host.inW = w;
	host.inH = h;
	glGenTextures( 1, &host.input );
	uploadTexture( host.input, premultiply( card( w, h, 0, false ) ), w, h );
	OmtOut a, b;
	initPlugin( a, w, h );
	initPlugin( b, w, h );
	const std::string name = prefix() + "-same";
	a.SetTextParameter( OmtOut::P_NAME, name.c_str() );
	b.SetTextParameter( OmtOut::P_NAME, name.c_str() );
	const std::string addrA = waitForAddress( a, host, 5000 );
	const std::string addrB = waitForAddress( b, host, 5000 );
	report( !addrA.empty() && !addrB.empty() && addrA != addrB, "out.unique-names", "\"" + addrA + "\" and \"" + addrB + "\"" );
	a.DeInitGL();
	b.DeInitGL();
	glDeleteTextures( 1, &host.input );
	freeTarget( host.out );
}


/// A fixed Frame Rate paces sending: a host rendering at 100 Hz with Frame
/// Rate 60 must put 60 frames a second on the wire, 1/60 s apart in time.
void checkPace()
{
	const int w = 640, h = 360;
	Host host;
	host.out = makeTarget( w, h );
	host.inW = w;
	host.inH = h;
	glGenTextures( 1, &host.input );
	uploadTexture( host.input, premultiply( card( w, h, 0, false ) ), w, h );
	OmtOut out;
	initPlugin( out, w, h );
	out.SetTextParameter( OmtOut::P_NAME, ( prefix() + "-pace" ).c_str() );
	out.SetFloatParameter( OmtOut::P_RATE, 8.0f );// "60"
	const std::string address = waitForAddress( out, host, 5000 );
	const somt::OmtApi* a = api();
	omt_receive_t* rx = a->receive_create( address.c_str(), OMTFrameType_Video, OMTPreferredVideoFormat_BGRA, OMTReceiveFlags_None );
	sleepMs( 300 );
	std::vector< int64_t > stamps;
	int labelN = 0, labelD = 0;
	const auto start = clock_::now();
	auto nextRender = start;
	while( clock_::now() - start < std::chrono::seconds( 4 ) )
	{
		if( clock_::now() >= nextRender )
		{
			host.processEffect( out );// a 100 Hz host
			nextRender += std::chrono::microseconds( 10000 );
		}
		if( OMTMediaFrame* m = a->receive( rx, OMTFrameType_Video, 1 ) )
		{
			if( clock_::now() - start > std::chrono::seconds( 1 ) )
				stamps.push_back( m->Timestamp );
			labelN = m->FrameRateN;
			labelD = m->FrameRateD;
		}
	}
	a->receive_destroy( rx );
	const double fps = stamps.size() > 1 ? ( stamps.size() - 1 ) * 1e7 / static_cast< double >( stamps.back() - stamps.front() ) : 0;
	int64_t worstStep = 0;
	for( size_t i = 1; i < stamps.size(); ++i )
		worstStep = std::max( worstStep, std::llabs( ( stamps[ i ] - stamps[ i - 1 ] ) - 166667 ) );
	const double wire = stamps.size() / 3.0;
	report( std::fabs( wire - 60.0 ) < 3.0 && worstStep <= 1 && labelN == 60 && labelD == 1, "out.pace60",
	        "100 Hz host, Frame Rate 60: " + fmt( "%.1f frames/s on the wire", wire ) + ", timestamps " + fmt( "%.3f fps", fps )
	            + ", worst step error " + std::to_string( worstStep ) + " x100 ns, labelled " + std::to_string( labelN ) + "/"
	            + std::to_string( labelD ) );
	out.DeInitGL();
	glDeleteTextures( 1, &host.input );
	freeTarget( host.out );
}

/// Render-thread cost: what each plugin takes out of Resolume's frame.
void checkBench( int w, int h )
{
	Host host;
	host.out = makeTarget( w, h );
	host.inW = w;
	host.inH = h;
	glGenTextures( 1, &host.input );
	uploadTexture( host.input, premultiply( card( w, h, 0, false ) ), w, h );
	OmtOut out;
	initPlugin( out, w, h );
	out.SetTextParameter( OmtOut::P_NAME, ( prefix() + "-bench" ).c_str() );
	const std::string address = waitForAddress( out, host, 5000 );

	Host inHost;
	inHost.out = makeTarget( w, h );
	OmtIn in;
	initPlugin( in, w, h );
	in.SetTextParameter( OmtIn::P_ADDRESS, address.c_str() );

	std::vector< double > outMs, inMs;
	const auto until = clock_::now() + std::chrono::seconds( 6 );
	while( clock_::now() < until && outMs.size() < 240 )
	{
		const auto frameStart = clock_::now();
		auto t = clock_::now();
		host.processEffect( out );
		glFinish();
		outMs.push_back( std::chrono::duration< double, std::milli >( clock_::now() - t ).count() );
		const uint64_t before = in.framesUploaded();
		t = clock_::now();
		inHost.processSource( in );
		glFinish();
		if( in.framesUploaded() > before )
			inMs.push_back( std::chrono::duration< double, std::milli >( clock_::now() - t ).count() );
		std::this_thread::sleep_until( frameStart + std::chrono::microseconds( 16667 ) );
	}
	auto median = []( std::vector< double > v ) {
		if( v.empty() )
			return -1.0;
		std::sort( v.begin(), v.end() );
		return v[ v.size() / 2 ];
	};
	auto p95 = []( std::vector< double > v ) {
		if( v.empty() )
			return -1.0;
		std::sort( v.begin(), v.end() );
		return v[ v.size() * 95 / 100 ];
	};
	const somt::SenderStatus st = out.senderStatus();
	const std::string size = std::to_string( w ) + "x" + std::to_string( h );
	report( median( outMs ) >= 0 && p95( outMs ) < 8.0, "bench.out." + size,
	        "render thread median " + fmt( "%.2f ms", median( outMs ) ) + ", p95 " + fmt( "%.2f ms", p95( outMs ) ) + "; encoder "
	            + fmt( "%.2f ms/frame", st.encodeMs ) + ", sent " + std::to_string( st.sent ) + ", dropped " + std::to_string( st.dropped )
	            + " (glFinish included)" );
	report( median( inMs ) >= 0 && p95( inMs ) < 8.0, "bench.in." + size,
	        "render thread with an upload: median " + fmt( "%.2f ms", median( inMs ) ) + ", p95 " + fmt( "%.2f ms", p95( inMs ) ) + ", "
	            + std::to_string( inMs.size() ) + " uploads" );
	out.DeInitGL();
	in.DeInitGL();
	glDeleteTextures( 1, &host.input );
	freeTarget( host.out );
	freeTarget( inHost.out );
}

/// Many instances, made and dropped while running: no hang, no crash.
void checkChurn()
{
	const auto t0 = clock_::now();
	const int w = 320, h = 180;
	for( int round = 0; round < 6; ++round )
	{
		Host host;
		host.out = makeTarget( w, h );
		host.inW = w;
		host.inH = h;
		glGenTextures( 1, &host.input );
		uploadTexture( host.input, premultiply( card( w, h, 0, false ) ), w, h );
		std::vector< std::unique_ptr< OmtOut > > outs;
		std::vector< std::unique_ptr< OmtIn > > ins;
		for( int i = 0; i < 3; ++i )
		{
			outs.emplace_back( new OmtOut() );
			initPlugin( *outs.back(), w, h );
			outs.back()->SetTextParameter( OmtOut::P_NAME, ( prefix() + "-churn" ).c_str() );
			ins.emplace_back( new OmtIn() );
			initPlugin( *ins.back(), w, h );
			ins.back()->SetTextParameter( OmtIn::P_ADDRESS, "NOWHERE (nothing)" );
		}
		for( int f = 0; f < 10; ++f )
		{
			for( auto& o : outs )
				host.processEffect( *o );
			for( auto& i : ins )
				host.processSource( *i );
			sleepMs( 5 );
		}
		for( auto& o : outs )
			o->DeInitGL();
		for( auto& i : ins )
			i->DeInitGL();
		glDeleteTextures( 1, &host.input );
		freeTarget( host.out );
	}
	const double s = std::chrono::duration< double >( clock_::now() - t0 ).count();
	report( s < 20.0, "churn", "6 rounds of 3 Outs + 3 Ins made, run and destroyed in " + fmt( "%.2f s", s ) );
}


/// --serve NAME SECONDS: a live 1920x1080 60 fps test card as an OMT source,
/// for feeding a real host. The counter advances by one per frame.
int serve( const std::string& name, double seconds, bool alpha )
{
	const somt::OmtApi* a = api();
	omt_send_t* tx = a->send_create( name.c_str(), OMTQuality_Default );
	char buffer[ 512 ] = {};
	a->send_getaddress( tx, buffer, sizeof( buffer ) );
	std::printf( "serving \"%s\" for %.0f s\n", buffer, seconds );
	std::fflush( stdout );
	const int w = 1920, h = 1080;
	const auto start = clock_::now();
	uint32_t f = 0;
	std::vector< uint8_t > base = card( w, h, 0, alpha );
	for( size_t i = 0; i < base.size(); i += 4 )
		std::swap( base[ i ], base[ i + 2 ] );
	while( std::chrono::duration< double >( clock_::now() - start ).count() < seconds )
	{
		// Only the counter rows change; redraw just those.
		for( int y = 3 * h / 4; y < h; ++y )
			for( int x = 0; x < w; ++x )
			{
				const int bit = std::min( 15, x * 16 / w );
				const uint8_t v = ( f >> bit ) & 1u ? 235 : 16;
				uint8_t* q = &base[ ( static_cast< size_t >( y ) * w + x ) * 4 ];
				q[ 0 ] = q[ 1 ] = q[ 2 ] = v;
			}
		OMTMediaFrame frame {};
		frame.Type = OMTFrameType_Video;
		frame.Codec = OMTCodec_BGRA;
		frame.Width = w;
		frame.Height = h;
		frame.Stride = w * 4;
		frame.Flags = alpha ? OMTVideoFlags_Alpha : OMTVideoFlags_None;
		frame.FrameRateN = 60;
		frame.FrameRateD = 1;
		frame.AspectRatio = 16.0f / 9.0f;
		frame.ColorSpace = OMTColorSpace_BT709;
		frame.Timestamp = -1;// libomt paces to 60 fps
		frame.Data = base.data();
		frame.DataLength = static_cast< int >( base.size() );
		a->send( tx, &frame );
		++f;
	}
	a->send_destroy( tx );
	return 0;
}

/// --probe ADDRESS SECONDS: receive a source and judge it as a test card.
int probe( const std::string& address, double seconds )
{
	const somt::OmtApi* a = api();
	std::string target = address;
	// A substring is enough, as in oxbow: find it in discovery first.
	if( target.rfind( "omt://", 0 ) != 0 )
	{
		const auto until = clock_::now() + std::chrono::seconds( 5 );
		std::string match;
		while( clock_::now() < until && match.empty() )
		{
			int count = 0;
			char** list = a->discovery_getaddresses( &count );
			for( int i = 0; i < count; ++i )
				if( list[ i ] && std::strstr( list[ i ], address.c_str() ) )
					match = list[ i ];
			if( match.empty() )
				sleepMs( 200 );
		}
		if( match.empty() )
		{
			std::printf( "no OMT source matching \"%s\"\n", address.c_str() );
			return 1;
		}
		target = match;
	}
	omt_receive_t* rx = a->receive_create( target.c_str(), OMTFrameType_Video, OMTPreferredVideoFormat_BGRA, OMTReceiveFlags_None );
	const auto start = clock_::now();
	int frames = 0, readable = 0, backwards = 0;
	long first = -1, last = -1;
	BarError worst;
	int w = 0, h = 0, rn = 0, rd = 1;
	while( std::chrono::duration< double >( clock_::now() - start ).count() < seconds )
	{
		OMTMediaFrame* m = a->receive( rx, OMTFrameType_Video, 100 );
		if( !m )
			continue;
		++frames;
		w = m->Width;
		h = m->Height;
		rn = m->FrameRateN;
		rd = m->FrameRateD;
		const uint8_t* px = static_cast< const uint8_t* >( m->Data );
		const long c = decodeCounter( px, m->Stride, w, h, 1 );
		if( c >= 0 )
		{
			++readable;
			if( first < 0 )
				first = c;
			if( last >= 0 && c < last )
				++backwards;
			last = c;
			const BarError e = checkBars( px, m->Stride, w, h, 1, false, false );
			worst.worst = std::max( worst.worst, e.worst );
			worst.upright = worst.upright && e.upright;
		}
	}
	a->receive_destroy( rx );
	const double fps = frames / seconds;
	std::printf( "probe \"%s\": %d frames in %.0f s (%.1f fps), %dx%d, sender says %d/%d\n", target.c_str(), frames, seconds, fps, w, h, rn, rd );
	if( readable )
		std::printf( "  test card: %d frames readable, counter %ld -> %ld (%ld steps), %d backwards, %s, worst bar error %.1f levels\n", readable,
		             first, last, last - first, backwards, worst.upright ? "upright" : "UPSIDE DOWN", worst.worst );
	else
		std::printf( "  no test card in the picture\n" );
	return frames > 0 ? 0 : 1;
}

} // namespace

int main( int argc, char** argv )
{
	std::string only;
	const std::vector< std::string > all = { "scan", "names", "out", "in", "loop", "unique", "pace", "bench", "churn" };
	for( int i = 1; i < argc; ++i )
	{
		const std::string a = argv[ i ];
		if( a == "--only" && i + 1 < argc )
			only = argv[ ++i ];
		else if( a == "--serve" && i + 2 < argc )
		{
			const std::string n = argv[ i + 1 ];
			const double secs = std::atof( argv[ i + 2 ] );
			const bool withAlpha = i + 3 < argc && std::string( argv[ i + 3 ] ) == "alpha";
			return serve( n, secs, withAlpha );
		}
		else if( a == "--probe" && i + 2 < argc )
			return probe( argv[ i + 1 ], std::atof( argv[ i + 2 ] ) );
		else if( a == "--list" )
		{
			for( const auto& n : all )
				std::printf( "%s\n", n.c_str() );
			return 0;
		}
	}
	auto want = [ & ]( const char* n ) { return only.empty() || only == n; };

	if( std::getenv( "OMT_STORAGE_PATH" ) == nullptr )
	{
		// Keep libomt's logs and settings out of ~/.OMT while testing.
		static std::string store = std::string( std::getenv( "TMPDIR" ) ? std::getenv( "TMPDIR" ) : "/tmp" ) + "/omttest-store";
		setenv( "OMT_STORAGE_PATH", store.c_str(), 0 );
	}

	if( !makeContext() )
	{
		std::printf( "no GL context\n" );
		return 2;
	}
	std::printf( "GL %s on %s\n", glGetString( GL_VERSION ), glGetString( GL_RENDERER ) );

	if( want( "scan" ) )
		checkScan();
	if( want( "names" ) )
		checkNames();
	if( want( "out" ) )
	{
		checkOut( 1920, 1080, false, "1080" );
		checkOut( 1280, 720, true, "720a" );
	}
	if( want( "in" ) )
	{
		checkIn( 1920, 1080, 1920, 1080, false, OmtIn::FIT_FIT, "1080" );
		checkIn( 1280, 720, 1280, 720, true, OmtIn::FIT_STRETCH, "720a" );
		checkIn( 640, 480, 1280, 720, false, OmtIn::FIT_FIT, "4x3fit" );
	}
	if( want( "loop" ) )
		checkLoop();
	if( want( "unique" ) )
		checkUniqueNames();
	if( want( "pace" ) )
		checkPace();
	if( want( "bench" ) )
	{
		checkBench( 1920, 1080 );
		checkBench( 3840, 2160 );
	}
	if( want( "churn" ) )
		checkChurn();

	std::printf( "runtime: %s\n%d failure(s)\n", somt::omtRuntimePath().c_str(), failures );
	return failures;
}
