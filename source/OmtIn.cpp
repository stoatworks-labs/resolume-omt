#include "OmtIn.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "Diag.h"
#include "Discovery.h"
#include "GLState.h"
#include "OmtRuntime.h"

using namespace ffglex;
using somt::ReceiverStatus;

namespace
{
constexpr float kValueNone = 0.0f;
constexpr float kValueTyped = 1.0f;

const char* const kVertex = R"(#version 410 core
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;
out vec2 uv;
void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

// Frame rows arrive top first and GL textures are bottom first, hence the
// flip. The host composites premultiplied colour, so straight alpha from the
// sender is premultiplied here; outside the frame (Fit's bars) is clear.
const char* const kFragment = R"(#version 410 core
uniform sampler2D Frame;
uniform vec2 Scale;
uniform int Opaque;
uniform int Straight;
in vec2 uv;
out vec4 fragColor;
void main()
{
	vec2 f = ( uv - 0.5 ) * Scale + 0.5;
	if( f.x < 0.0 || f.y < 0.0 || f.x > 1.0 || f.y > 1.0 )
	{
		fragColor = vec4( 0.0 );
		return;
	}
	vec4 c = texture( Frame, vec2( f.x, 1.0 - f.y ) );
	if( Opaque == 1 )
		c.a = 1.0;
	else if( Straight == 1 )
		c.rgb *= c.a;
	fragColor = c;
}
)";

bool isUrl( const std::string& s )
{
	return s.rfind( "omt://", 0 ) == 0;
}

std::string formatRate( int n, int d )
{
	if( n <= 0 || d <= 0 )
		return "";
	const double fps = static_cast< double >( n ) / static_cast< double >( d );
	char buffer[ 32 ];
	if( std::fabs( fps - std::round( fps ) ) < 0.005 )
		std::snprintf( buffer, sizeof( buffer ), "%d fps", static_cast< int >( std::lround( fps ) ) );
	else
		std::snprintf( buffer, sizeof( buffer ), "%.2f fps", fps );
	return buffer;
}

// What receivers call a source is "HOST (Name)"; the menu is narrow, so it
// shows that as is but trims anything absurd.
std::string menuLabel( const std::string& name )
{
	return name.size() > 60 ? name.substr( 0, 57 ) + "..." : name;
}
} // namespace

OmtIn::OmtIn()
{
	SetMinInputs( 0 );
	SetMaxInputs( 0 );

	SetOptionParamInfo( P_SOURCE, "Source", kMenuElements, kValueNone );
	SetParamElementInfo( P_SOURCE, 0, "None", kValueNone );
	SetParamElementInfo( P_SOURCE, 1, "Typed address", kValueTyped );
	for( int i = 0; i < kMenuSources; ++i )
		SetParamElementInfo( P_SOURCE, static_cast< unsigned int >( 2 + i ), "-", -1.0f - static_cast< float >( i ) );

	SetParamInfo( P_ADDRESS, "Address", FF_TYPE_TEXT, "" );

	SetOptionParamInfo( P_FIT, "Fit", 4, static_cast< float >( FIT_FIT ) );
	SetParamElementInfo( P_FIT, 0, "Fit", static_cast< float >( FIT_FIT ) );
	SetParamElementInfo( P_FIT, 1, "Fill", static_cast< float >( FIT_FILL ) );
	SetParamElementInfo( P_FIT, 2, "Stretch", static_cast< float >( FIT_STRETCH ) );
	SetParamElementInfo( P_FIT, 3, "Native 1:1", static_cast< float >( FIT_NATIVE ) );

	SetOptionParamInfo( P_NO_SIGNAL, "No Signal", 3, static_cast< float >( NS_HOLD ) );
	SetParamElementInfo( P_NO_SIGNAL, 0, "Hold last frame", static_cast< float >( NS_HOLD ) );
	SetParamElementInfo( P_NO_SIGNAL, 1, "Transparent", static_cast< float >( NS_TRANSPARENT ) );
	SetParamElementInfo( P_NO_SIGNAL, 2, "Black", static_cast< float >( NS_BLACK ) );

	SetParamInfo( P_KEEP_ALPHA, "Keep Alpha", FF_TYPE_BOOLEAN, true );
	SetParamInfo( P_STATUS, "Status", FF_TYPE_TEXT, "Pick a source" );

	menuLabels.assign( kMenuElements, "-" );
	menuKeys.resize( kMenuElements );
	menuLabels[ 0 ] = "None";
	menuLabels[ 1 ] = "Typed address";
	menuKeys[ 0 ] = kValueNone;
	menuKeys[ 1 ] = kValueTyped;
	for( int i = 0; i < kMenuSources; ++i )
		menuKeys[ 2 + i ] = -1.0f - static_cast< float >( i );

	// Nothing else here: Arena constructs every plugin at its startup scan.
	// The runtime, discovery and any receiver start on the first frame.
}

OmtIn::~OmtIn()
{
	if( subscribed )
		somt::Discovery::instance().unsubscribe();
}

FFResult OmtIn::InitGL( const FFGLViewportStruct* vp )
{
	somt::diag::init();
	if( !shader.Compile( kVertex, kFragment ) )
	{
		somt::diag::error( "OMT In: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}
	if( !quad.Initialise() )
	{
		somt::diag::error( "OMT In: quad failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}
	return CFFGLPlugin::InitGL( vp );
}

FFResult OmtIn::DeInitGL()
{
	shader.FreeGLResources();
	quad.Release();
	if( texture )
	{
		glDeleteTextures( 1, &texture );
		texture = 0;
	}
	textureWidth = textureHeight = 0;
	haveFrame = false;
	return FF_SUCCESS;
}

void OmtIn::setAddress( const std::string& value, bool fromHost )
{
	std::string trimmed = value;
	while( !trimmed.empty() && std::isspace( static_cast< unsigned char >( trimmed.back() ) ) )
		trimmed.pop_back();
	while( !trimmed.empty() && std::isspace( static_cast< unsigned char >( trimmed.front() ) ) )
		trimmed.erase( trimmed.begin() );
	if( trimmed == address )
		return;
	address = trimmed;
	if( !fromHost )
		RaiseParamEvent( P_ADDRESS, FF_EVENT_FLAG_VALUE );
	refreshMenu( true );
}

void OmtIn::refreshMenu( bool force )
{
	uint64_t generation = 0;
	std::vector< std::string > found = somt::Discovery::instance().sources( &generation );
	if( !force && generation == menuGeneration && address == menuAddress )
		return;
	menuGeneration = generation;
	menuAddress = address;

	std::vector< std::string > labels = { "None", "Typed address" };
	std::vector< float > keys = { kValueNone, kValueTyped };
	const bool addressListed = std::find( found.begin(), found.end(), address ) != found.end();
	const bool offlineEntry = !address.empty() && !isUrl( address ) && !addressListed;
	const size_t room = static_cast< size_t >( kMenuSources ) - ( offlineEntry ? 1 : 0 );
	for( size_t i = 0; i < found.size() && i < room; ++i )
	{
		labels.push_back( menuLabel( found[ i ] ) );
		keys.push_back( somt::sourceKey( found[ i ] ) );
	}
	if( offlineEntry )
	{
		labels.push_back( menuLabel( address ) + " (offline)" );
		keys.push_back( somt::sourceKey( address ) );
	}
	for( int i = 0; labels.size() < static_cast< size_t >( kMenuElements ); ++i )
	{
		labels.push_back( "-" );
		keys.push_back( -1.0f - static_cast< float >( i ) );
	}

	if( labels != menuLabels || keys != menuKeys )
	{
		menuLabels = labels;
		menuKeys = keys;
		SetParamElements( P_SOURCE, labels, keys, true );
	}

	// The menu follows Address, never the other way round.
	float wanted = kValueNone;
	if( !address.empty() )
		wanted = ( isUrl( address ) || ( !addressListed && !offlineEntry ) ) ? kValueTyped : somt::sourceKey( address );
	if( wanted != sourceValue )
	{
		sourceValue = wanted;
		RaiseParamEvent( P_SOURCE, FF_EVENT_FLAG_VALUE );
	}
}

void OmtIn::refreshStatus( bool force )
{
	const auto now = std::chrono::steady_clock::now();
	if( !force && now - lastStatus < std::chrono::milliseconds( 250 ) )
		return;
	lastStatus = now;

	std::string text;
	const std::string runtimeError = somt::Discovery::instance().runtimeError();
	const ReceiverStatus st = receiver.status();
	if( !runtimeError.empty() || st.state == ReceiverStatus::State::NoRuntime )
		text = "OMT runtime not found - see the log";
	else if( address.empty() )
		text = "Pick a source";
	else
	{
		switch( st.state )
		{
		case ReceiverStatus::State::Idle:
		case ReceiverStatus::State::Loading: text = "Starting OMT..."; break;
		case ReceiverStatus::State::NoRuntime: break;
		case ReceiverStatus::State::Connecting: text = "Connecting to " + address + "..."; break;
		case ReceiverStatus::State::Live:
			text = std::to_string( st.width ) + "x" + std::to_string( st.height ) + " " + formatRate( st.rateN, st.rateD )
			       + ( st.alpha ? " with alpha" : "" );
			break;
		case ReceiverStatus::State::NoSignal:
			text = "No signal for " + std::to_string( static_cast< int >( st.secondsSinceFrame ) ) + " s";
			break;
		}
	}
	if( text != status )
	{
		status = text;
		RaiseParamEvent( P_STATUS, FF_EVENT_FLAG_VALUE );
	}
}

bool OmtIn::upload( const somt::VideoFrame& frame )
{
	somt::ScopedPixelStore pixelStore;
	glBindBuffer( GL_PIXEL_UNPACK_BUFFER, 0 );
	Scoped2DTextureBinding binding( 0 );
	if( !texture )
		glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	if( frame.width != textureWidth || frame.height != textureHeight )
	{
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, frame.width, frame.height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
		              nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		textureWidth = frame.width;
		textureHeight = frame.height;
	}
	glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
	glPixelStorei( GL_UNPACK_ROW_LENGTH, frame.stride / 4 );
	glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
	                 frame.pixels.data() );
	++uploads;
	return true;
}

FFResult OmtIn::ProcessOpenGL( ProcessOpenGLStruct* )
{
	if( !subscribed )
	{
		somt::Discovery::instance().subscribe();
		subscribed = true;
	}

	if( receiver.target() != address )
		haveFrame = false;// a different source: never show the last one's frame
	receiver.setTarget( address );
	refreshMenu( false );

	if( receiver.takeFrame( front ) )
	{
		upload( front );
		haveFrame = true;
		frameAlpha = front.codec == static_cast< uint32_t >( OMTCodec_BGRA ) && ( front.flags & OMTVideoFlags_Alpha );
		frameAspect = front.aspect > 0.01f ? front.aspect
		                                   : static_cast< float >( front.width ) / static_cast< float >( front.height );
	}
	refreshStatus( false );

	GLfloat previousClear[ 4 ];
	glGetFloatv( GL_COLOR_CLEAR_VALUE, previousClear );
	const ReceiverStatus st = receiver.status();
	const bool lost = st.state == ReceiverStatus::State::NoSignal;
	const bool black = !address.empty() && ( !haveFrame || lost ) && noSignal == NS_BLACK;
	glClearColor( 0.0f, 0.0f, 0.0f, black ? 1.0f : 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );
	glClearColor( previousClear[ 0 ], previousClear[ 1 ], previousClear[ 2 ], previousClear[ 3 ] );

	if( !haveFrame || address.empty() || ( lost && noSignal != NS_HOLD ) )
		return FF_SUCCESS;

	const float vw = static_cast< float >( std::max( 1u, currentViewport.width ) );
	const float vh = static_cast< float >( std::max( 1u, currentViewport.height ) );
	const float outAspect = vw / vh;
	float sx = 1.0f, sy = 1.0f;
	switch( fit )
	{
	case FIT_FIT:
		if( frameAspect > outAspect )
			sy = frameAspect / outAspect;
		else
			sx = outAspect / frameAspect;
		break;
	case FIT_FILL:
		if( frameAspect > outAspect )
			sx = outAspect / frameAspect;
		else
			sy = frameAspect / outAspect;
		break;
	case FIT_STRETCH: break;
	case FIT_NATIVE:
		sx = vw / static_cast< float >( textureWidth );
		sy = vh / static_cast< float >( textureHeight );
		break;
	}

	ScopedShaderBinding shaderBinding( shader.GetGLID() );
	ScopedSamplerActivation sampler( 0 );
	Scoped2DTextureBinding textureBinding( texture );
	const GLint filter = fit == FIT_NATIVE ? GL_NEAREST : GL_LINEAR;
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter );
	shader.Set( "Frame", 0 );
	shader.Set( "Scale", sx, sy );
	shader.Set( "Opaque", ( !keepAlpha || !frameAlpha ) ? 1 : 0 );
	shader.Set( "Straight", ( front.flags & OMTVideoFlags_PreMultiplied ) ? 0 : 1 );
	quad.Draw();
	return FF_SUCCESS;
}

FFResult OmtIn::SetFloatParameter( unsigned int index, float value )
{
	switch( index )
	{
	case P_SOURCE:
	{
		if( value == kValueNone )
		{
			sourceValue = kValueNone;
			setAddress( "", false );
			return FF_SUCCESS;
		}
		if( value == kValueTyped || value < 0.0f )
		{
			// "Typed address" leaves Address alone; a spare slot means nothing.
			// Either way the menu goes back to showing what Address says.
			RaiseParamEvent( P_SOURCE, FF_EVENT_FLAG_VALUE );
			return FF_SUCCESS;
		}
		for( size_t i = 2; i < menuKeys.size(); ++i )
		{
			if( menuKeys[ i ] != value )
				continue;
			std::string name = menuLabels[ i ];
			const std::string suffix = " (offline)";
			if( name.size() > suffix.size() && name.compare( name.size() - suffix.size(), suffix.size(), suffix ) == 0 )
				name = address;// the offline entry is Address itself
			else
			{
				// Labels may be trimmed; take the full name from discovery.
				for( const std::string& source : somt::Discovery::instance().sources() )
					if( somt::sourceKey( source ) == value )
						name = source;
			}
			sourceValue = value;
			setAddress( name, false );
			return FF_SUCCESS;
		}
		// Not listed yet (a composition loading before discovery has heard
		// the source): Address, restored beside it, is what connects.
		return FF_SUCCESS;
	}
	case P_FIT: fit = std::clamp( static_cast< int >( std::lround( value ) ), 0, 3 ); return FF_SUCCESS;
	case P_NO_SIGNAL: noSignal = std::clamp( static_cast< int >( std::lround( value ) ), 0, 2 ); return FF_SUCCESS;
	case P_KEEP_ALPHA: keepAlpha = value > 0.5f; return FF_SUCCESS;
	default: return FF_FAIL;
	}
}

float OmtIn::GetFloatParameter( unsigned int index )
{
	switch( index )
	{
	case P_SOURCE: return sourceValue;
	case P_FIT: return static_cast< float >( fit );
	case P_NO_SIGNAL: return static_cast< float >( noSignal );
	case P_KEEP_ALPHA: return keepAlpha ? 1.0f : 0.0f;
	default: return 0.0f;
	}
}

FFResult OmtIn::SetTextParameter( unsigned int index, const char* value )
{
	switch( index )
	{
	case P_ADDRESS: setAddress( value ? value : "", true ); return FF_SUCCESS;
	case P_STATUS:
		// Read-only: whatever was typed (or saved) is replaced by the real one.
		RaiseParamEvent( P_STATUS, FF_EVENT_FLAG_VALUE );
		return FF_SUCCESS;
	default: return FF_FAIL;
	}
}

char* OmtIn::GetTextParameter( unsigned int index )
{
	switch( index )
	{
	case P_ADDRESS: addressReturn = address; return const_cast< char* >( addressReturn.c_str() );
	case P_STATUS: statusReturn = status; return const_cast< char* >( statusReturn.c_str() );
	default: return CFFGLPlugin::GetTextParameter( index );
	}
}
