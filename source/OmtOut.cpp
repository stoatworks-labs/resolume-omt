#include "OmtOut.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Diag.h"
#include "GLState.h"
#include "OmtRuntime.h"

using namespace ffglex;
using somt::SenderStatus;

namespace
{
const char* const kVertex = R"(#version 410 core
uniform vec2 MaxUV;
uniform int FlipY;
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;
out vec2 uv;
void main()
{
	gl_Position = vPosition;
	vec2 t = vUV;
	if( FlipY == 1 )
		t.y = 1.0 - t.y;
	uv = t * MaxUV;
}
)";

// The pass-through and the capture share it. Capture flips so the buffer's
// first row is the picture's top row, and for an alpha send undoes the
// host's premultiplication: OMT, like NDI, means straight alpha unless the
// PreMultiplied flag says otherwise, and straight is what every receiver
// handles. Without alpha the colour stays premultiplied, which is the
// picture over black: what a receiver ignoring alpha should see.
const char* const kFragment = R"(#version 410 core
uniform sampler2D InputTexture;
uniform int Unpremultiply;
in vec2 uv;
out vec4 fragColor;
void main()
{
	vec4 c = texture( InputTexture, uv );
	if( Unpremultiply == 1 )
		c.rgb = c.a > 0.0 ? min( c.rgb / c.a, vec3( 1.0 ) ) : vec3( 0.0 );
	fragColor = c;
}
)";

struct Rate
{
	int n, d;
};
// The Frame Rate menu, after Auto.
const Rate kRates[] = { { 24000, 1001 }, { 24, 1 }, { 25, 1 }, { 30000, 1001 }, { 30, 1 },
	                    { 50, 1 },       { 60000, 1001 }, { 60, 1 } };
const char* const kRateNames[] = { "23.98", "24", "25", "29.97", "30", "50", "59.94", "60" };
constexpr int kRateCount = sizeof( kRates ) / sizeof( kRates[ 0 ] );

// What Auto may snap to: the menu plus the high rates displays run at.
const Rate kAutoRates[] = { { 24000, 1001 }, { 24, 1 }, { 25, 1 }, { 30000, 1001 }, { 30, 1 }, { 48, 1 },
	                        { 50, 1 }, { 60000, 1001 }, { 60, 1 }, { 72, 1 }, { 75, 1 }, { 90, 1 },
	                        { 100, 1 }, { 120000, 1001 }, { 120, 1 }, { 144, 1 }, { 165, 1 }, { 240, 1 } };

const OMTQuality kQualities[] = { OMTQuality_Default, OMTQuality_Low, OMTQuality_Medium, OMTQuality_High };

std::string formatRate( int n, int d )
{
	const double fps = static_cast< double >( n ) / static_cast< double >( std::max( 1, d ) );
	char buffer[ 32 ];
	if( std::fabs( fps - std::round( fps ) ) < 0.005 )
		std::snprintf( buffer, sizeof( buffer ), "%d fps", static_cast< int >( std::lround( fps ) ) );
	else
		std::snprintf( buffer, sizeof( buffer ), "%.2f fps", fps );
	return buffer;
}
} // namespace

OmtOut::OmtOut()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	SetParamInfo( P_NAME, "Name", FF_TYPE_TEXT, "Resolume" );
	SetParamInfo( P_SEND, "Send", FF_TYPE_BOOLEAN, true );

	SetOptionParamInfo( P_QUALITY, "Quality", 4, 0.0f );
	SetParamElementInfo( P_QUALITY, 0, "Receiver decides", 0.0f );
	SetParamElementInfo( P_QUALITY, 1, "Low", 1.0f );
	SetParamElementInfo( P_QUALITY, 2, "Medium", 2.0f );
	SetParamElementInfo( P_QUALITY, 3, "High", 3.0f );

	SetParamInfo( P_ALPHA, "Send Alpha", FF_TYPE_BOOLEAN, false );

	SetOptionParamInfo( P_RATE, "Frame Rate", kRateCount + 1, 0.0f );
	SetParamElementInfo( P_RATE, 0, "Auto", 0.0f );
	for( int i = 0; i < kRateCount; ++i )
		SetParamElementInfo( P_RATE, static_cast< unsigned int >( i + 1 ), kRateNames[ i ], static_cast< float >( i + 1 ) );

	SetParamInfo( P_STATUS, "Status", FF_TYPE_TEXT, "Not sending yet" );
	// Nothing starts here: Arena constructs every plugin at its startup scan.
	// The sender starts with the first frame it is given.
}

OmtOut::~OmtOut() = default;

FFResult OmtOut::InitGL( const FFGLViewportStruct* vp )
{
	somt::diag::init();
	if( !passShader.Compile( kVertex, kFragment ) || !captureShader.Compile( kVertex, kFragment ) )
	{
		somt::diag::error( "OMT Out: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}
	if( !quad.Initialise() )
	{
		somt::diag::error( "OMT Out: quad failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}
	return CFFGLPlugin::InitGL( vp );
}

void OmtOut::releaseSlots()
{
	for( Slot& slot : slots )
	{
		if( slot.fence )
			glDeleteSync( slot.fence );
		if( slot.pbo )
			glDeleteBuffers( 1, &slot.pbo );
		slot = Slot {};
	}
	inFlight = 0;
	nextSlot = 0;
}

FFResult OmtOut::DeInitGL()
{
	releaseSlots();
	if( fbo )
		glDeleteFramebuffers( 1, &fbo );
	if( colour )
		glDeleteTextures( 1, &colour );
	fbo = colour = 0;
	targetWidth = targetHeight = 0;
	passShader.FreeGLResources();
	captureShader.FreeGLResources();
	quad.Release();
	return FF_SUCCESS;
}

bool OmtOut::ensureTarget( int width, int height )
{
	if( fbo && width == targetWidth && height == targetHeight )
		return true;
	if( !fbo )
		glGenFramebuffers( 1, &fbo );
	if( !colour )
		glGenTextures( 1, &colour );
	{
		Scoped2DTextureBinding binding( colour );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	}
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour, 0 );
	const bool complete = glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;
	targetWidth = width;
	targetHeight = height;
	if( !complete )
		somt::diag::error( "OMT Out: capture framebuffer incomplete at " + std::to_string( width ) + "x"
		                   + std::to_string( height ) );
	return complete;
}

void OmtOut::harvest( bool wait )
{
	somt::ScopedPixelStore pixelStore;
	// Oldest first, and stop at the first that is not ready: frames go out
	// in the order they were drawn.
	while( inFlight > 0 )
	{
		const int index = ( nextSlot - inFlight + kSlots ) % kSlots;
		Slot& slot = slots[ index ];
		const GLenum state = glClientWaitSync( slot.fence, wait ? GL_SYNC_FLUSH_COMMANDS_BIT : 0,
		                                       wait ? 1000000000ull : 0 );
		if( state != GL_ALREADY_SIGNALED && state != GL_CONDITION_SATISFIED )
			return;
		glDeleteSync( slot.fence );
		slot.fence = nullptr;
		--inFlight;

		glBindBuffer( GL_PIXEL_PACK_BUFFER, slot.pbo );
		const size_t bytes = static_cast< size_t >( slot.width ) * static_cast< size_t >( slot.height ) * 4;
		const void* mapped = glMapBufferRange( GL_PIXEL_PACK_BUFFER, 0, static_cast< GLsizeiptr >( bytes ), GL_MAP_READ_BIT );
		if( mapped )
		{
			somt::OutgoingFrame& frame = sender.frameToFill( slot.width, slot.height );
			std::memcpy( frame.pixels.data(), mapped, bytes );
			frame.alpha = slot.alpha;
			frame.rateN = rateN;
			frame.rateD = rateD;
			frame.timestamp = slot.timestamp;
			glUnmapBuffer( GL_PIXEL_PACK_BUFFER );
			sender.submit();
		}
		glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
	}
}

void OmtOut::issueReadback( const FFGLTextureStruct& input, int64_t timestamp )
{
	const int width = static_cast< int >( input.Width );
	const int height = static_cast< int >( input.Height );
	if( width <= 0 || height <= 0 )
		return;

	if( inFlight == kSlots )
	{
		// The GPU is three frames behind. Drop the oldest rather than stall
		// the host waiting for it.
		const int index = ( nextSlot - inFlight + kSlots ) % kSlots;
		glDeleteSync( slots[ index ].fence );
		slots[ index ].fence = nullptr;
		--inFlight;
		++pipelineDrops;
	}

	somt::ScopedViewportAndFramebuffer restoreTarget;
	somt::ScopedPixelStore pixelStore;
	if( !ensureTarget( width, height ) )
		return;

	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glViewport( 0, 0, width, height );
	{
		ScopedShaderBinding shaderBinding( captureShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding textureBinding( input.Handle );
		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );
		captureShader.Set( "InputTexture", 0 );
		captureShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		captureShader.Set( "FlipY", 1 );
		captureShader.Set( "Unpremultiply", alpha ? 1 : 0 );
		quad.Draw();
	}

	Slot& slot = slots[ nextSlot ];
	const size_t bytes = static_cast< size_t >( width ) * static_cast< size_t >( height ) * 4;
	if( !slot.pbo )
		glGenBuffers( 1, &slot.pbo );
	glBindBuffer( GL_PIXEL_PACK_BUFFER, slot.pbo );
	if( slot.capacity != bytes )
	{
		glBufferData( GL_PIXEL_PACK_BUFFER, static_cast< GLsizeiptr >( bytes ), nullptr, GL_STREAM_READ );
		slot.capacity = bytes;
	}
	glPixelStorei( GL_PACK_ALIGNMENT, 4 );
	glPixelStorei( GL_PACK_ROW_LENGTH, 0 );
	glReadBuffer( GL_COLOR_ATTACHMENT0 );
	glReadPixels( 0, 0, width, height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr );
	slot.fence = glFenceSync( GL_SYNC_GPU_COMMANDS_COMPLETE, 0 );
	slot.width = width;
	slot.height = height;
	slot.alpha = alpha;
	slot.timestamp = timestamp;
	nextSlot = ( nextSlot + 1 ) % kSlots;
	++inFlight;
	// Make sure the readback is actually submitted, so the fence can pass
	// by the next frame even if the host renders nothing else meanwhile.
	glFlush();
}

bool OmtOut::dueThisFrame( int64_t& timestamp )
{
	using namespace std::chrono;
	const auto now = steady_clock::now();
	if( rateChoice == 0 )
	{
		// Auto: every frame the host renders, stamped when it was drawn.
		pacedChoice = -1;
		timestamp = duration_cast< duration< int64_t, std::ratio< 1, 10000000 > > >( now - epoch ).count();
		return true;
	}
	const Rate rate = kRates[ rateChoice - 1 ];
	const auto interval = duration_cast< steady_clock::duration >( duration< double >( static_cast< double >( rate.d ) / rate.n ) );
	if( pacedChoice != rateChoice || now - nextDue > interval * 4 )
	{
		// A new rate, or the host stalled: restart the timeline now.
		pacedChoice = rateChoice;
		nextDue = now;
		paceIndex = 0;
		paceStart = duration_cast< duration< int64_t, std::ratio< 1, 10000000 > > >( now - epoch ).count();
	}
	// Half a render frame of slack, so a 60 Hz host sending 60 never misses
	// a slot to jitter; the mean render interval is the measure of that.
	const auto slack = duration_cast< steady_clock::duration >( duration< double >( meanInterval * 0.5 ) );
	if( now + slack < nextDue )
		return false;
	// 100 ns units, on the ideal timeline: rate.d / rate.n seconds a frame.
	timestamp = paceStart + static_cast< int64_t >( paceIndex ) * 10000000ll * rate.d / rate.n;
	++paceIndex;
	nextDue += interval;
	return true;
}

void OmtOut::updateRate()
{
	const auto now = std::chrono::steady_clock::now();
	if( rateChoice > 0 )
	{
		rateN = kRates[ rateChoice - 1 ].n;
		rateD = kRates[ rateChoice - 1 ].d;
		lastFrameTime = now;
		return;
	}
	if( lastFrameTime.time_since_epoch().count() != 0 )
	{
		const double dt = std::chrono::duration< double >( now - lastFrameTime ).count();
		// A paused host or a dropped clip is not a frame rate.
		if( dt > 0.0 && dt < 0.25 )
			meanInterval = meanInterval == 0.0 ? dt : meanInterval + ( dt - meanInterval ) * 0.05;
	}
	lastFrameTime = now;
	if( meanInterval <= 0.0 )
		return;

	const double fps = 1.0 / meanInterval;
	Rate best = kAutoRates[ 0 ];
	double bestError = 1e9;
	for( const Rate& r : kAutoRates )
	{
		const double error = std::fabs( fps - static_cast< double >( r.n ) / r.d );
		if( error < bestError )
		{
			bestError = error;
			best = r;
		}
	}
	if( best.n == rateN && best.d == rateD )
	{
		candidateFrames = 0;
		return;
	}
	if( best.n != candidateN || best.d != candidateD )
	{
		candidateN = best.n;
		candidateD = best.d;
		candidateFrames = 0;
	}
	// Held for about a second of frames before it changes what is sent.
	if( ++candidateFrames > static_cast< int >( fps ) )
	{
		rateN = best.n;
		rateD = best.d;
		candidateFrames = 0;
	}
}

void OmtOut::refreshStatus( bool force )
{
	const auto now = std::chrono::steady_clock::now();
	if( !force && now - lastStatus < std::chrono::milliseconds( 250 ) )
		return;
	lastStatus = now;

	const SenderStatus st = sender.status();
	std::string text;
	if( !sendOn )
		text = "Off";
	else
	{
		switch( st.state )
		{
		case SenderStatus::State::Idle: text = st.address.empty() ? "Starting OMT..." : st.address + ", waiting"; break;
		case SenderStatus::State::Loading: text = "Starting OMT..."; break;
		case SenderStatus::State::NoRuntime: text = "OMT runtime not found - see the log"; break;
		case SenderStatus::State::Failed: text = "Could not create the sender - see the log"; break;
		case SenderStatus::State::Sending:
			text = st.address + ", " + std::to_string( st.width ) + "x" + std::to_string( st.height ) + " "
			       + formatRate( st.rateN, st.rateD ) + ", " + std::to_string( st.connections )
			       + ( st.connections == 1 ? " receiver" : " receivers" );
			break;
		}
	}
	if( text != status )
	{
		status = text;
		RaiseParamEvent( P_STATUS, FF_EVENT_FLAG_VALUE );
	}
}

FFResult OmtOut::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || !pGL->inputTextures[ 0 ] )
		return FF_FAIL;
	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];

	updateRate();
	if( sendOn )
	{
		sender.configure( name, static_cast< int >( kQualities[ std::clamp( quality, 0, 3 ) ] ) );
		harvest( false );
		int64_t timestamp = 0;
		if( dueThisFrame( timestamp ) )
			issueReadback( input, timestamp );
	}
	else
	{
		// Off means gone from the network, not a frozen source.
		sender.configure( name, static_cast< int >( kQualities[ std::clamp( quality, 0, 3 ) ] ), false );
		if( inFlight > 0 )
			releaseSlots();
	}
	refreshStatus( false );

	// The picture passes through untouched, into the host's own target.
	ScopedShaderBinding shaderBinding( passShader.GetGLID() );
	ScopedSamplerActivation sampler( 0 );
	Scoped2DTextureBinding textureBinding( input.Handle );
	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );
	passShader.Set( "InputTexture", 0 );
	passShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
	passShader.Set( "FlipY", 0 );
	passShader.Set( "Unpremultiply", 0 );
	quad.Draw();
	return FF_SUCCESS;
}

void OmtOut::flushForTest()
{
	harvest( true );
}

FFResult OmtOut::SetFloatParameter( unsigned int index, float value )
{
	switch( index )
	{
	case P_SEND: sendOn = value > 0.5f; return FF_SUCCESS;
	case P_QUALITY: quality = std::clamp( static_cast< int >( std::lround( value ) ), 0, 3 ); return FF_SUCCESS;
	case P_ALPHA: alpha = value > 0.5f; return FF_SUCCESS;
	case P_RATE: rateChoice = std::clamp( static_cast< int >( std::lround( value ) ), 0, kRateCount ); return FF_SUCCESS;
	default: return FF_FAIL;
	}
}

float OmtOut::GetFloatParameter( unsigned int index )
{
	switch( index )
	{
	case P_SEND: return sendOn ? 1.0f : 0.0f;
	case P_QUALITY: return static_cast< float >( quality );
	case P_ALPHA: return alpha ? 1.0f : 0.0f;
	case P_RATE: return static_cast< float >( rateChoice );
	default: return 0.0f;
	}
}

FFResult OmtOut::SetTextParameter( unsigned int index, const char* value )
{
	switch( index )
	{
	case P_NAME:
	{
		std::string text = value ? value : "";
		while( !text.empty() && std::isspace( static_cast< unsigned char >( text.back() ) ) )
			text.pop_back();
		while( !text.empty() && std::isspace( static_cast< unsigned char >( text.front() ) ) )
			text.erase( text.begin() );
		name = text.empty() ? std::string( "Resolume" ) : text;
		return FF_SUCCESS;
	}
	case P_STATUS: RaiseParamEvent( P_STATUS, FF_EVENT_FLAG_VALUE ); return FF_SUCCESS;
	default: return FF_FAIL;
	}
}

char* OmtOut::GetTextParameter( unsigned int index )
{
	switch( index )
	{
	case P_NAME: nameReturn = name; return const_cast< char* >( nameReturn.c_str() );
	case P_STATUS: statusReturn = status; return const_cast< char* >( statusReturn.c_str() );
	default: return CFFGLPlugin::GetTextParameter( index );
	}
}
