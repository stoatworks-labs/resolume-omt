#pragma once

#include <FFGLSDK.h>

/**
    Save and restore the bits of GL state these plugins change that the SDK's
    scoped bindings do not cover. Resolume shares one context between every
    plugin and its own renderer, so a pixel-store setting left behind here
    corrupts somebody else's upload.
*/
namespace somt
{

class ScopedPixelStore
{
public:
	ScopedPixelStore()
	{
		glGetIntegerv( GL_UNPACK_ROW_LENGTH, &unpackRowLength );
		glGetIntegerv( GL_UNPACK_ALIGNMENT, &unpackAlignment );
		glGetIntegerv( GL_PACK_ROW_LENGTH, &packRowLength );
		glGetIntegerv( GL_PACK_ALIGNMENT, &packAlignment );
		glGetIntegerv( GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer );
		glGetIntegerv( GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer );
	}
	~ScopedPixelStore()
	{
		glPixelStorei( GL_UNPACK_ROW_LENGTH, unpackRowLength );
		glPixelStorei( GL_UNPACK_ALIGNMENT, unpackAlignment );
		glPixelStorei( GL_PACK_ROW_LENGTH, packRowLength );
		glPixelStorei( GL_PACK_ALIGNMENT, packAlignment );
		glBindBuffer( GL_PIXEL_UNPACK_BUFFER, static_cast< GLuint >( unpackBuffer ) );
		glBindBuffer( GL_PIXEL_PACK_BUFFER, static_cast< GLuint >( packBuffer ) );
	}
	ScopedPixelStore( const ScopedPixelStore& ) = delete;
	ScopedPixelStore& operator=( const ScopedPixelStore& ) = delete;

private:
	GLint unpackRowLength = 0, unpackAlignment = 4, packRowLength = 0, packAlignment = 4;
	GLint unpackBuffer = 0, packBuffer = 0;
};

class ScopedViewportAndFramebuffer
{
public:
	ScopedViewportAndFramebuffer()
	{
		glGetIntegerv( GL_VIEWPORT, viewport );
		glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo );
		glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &readFbo );
	}
	~ScopedViewportAndFramebuffer()
	{
		glBindFramebuffer( GL_DRAW_FRAMEBUFFER, static_cast< GLuint >( drawFbo ) );
		glBindFramebuffer( GL_READ_FRAMEBUFFER, static_cast< GLuint >( readFbo ) );
		glViewport( viewport[ 0 ], viewport[ 1 ], viewport[ 2 ], viewport[ 3 ] );
	}
	ScopedViewportAndFramebuffer( const ScopedViewportAndFramebuffer& ) = delete;
	ScopedViewportAndFramebuffer& operator=( const ScopedViewportAndFramebuffer& ) = delete;

private:
	GLint viewport[ 4 ] = { 0, 0, 0, 0 };
	GLint drawFbo = 0, readFbo = 0;
};

} // namespace somt
