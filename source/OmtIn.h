#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <FFGLSDK.h>

#include "Receiver.h"

/**
    SW OMT In: an OMT source on the network as a Resolume source.

    The Source menu lists what discovery hears, live. Picking one writes its
    full name into Address, and Address is what is connected to and what a
    saved composition keeps: a name ("STUDIO (vMix 1)") or an omt:// URL
    typed by hand. A source that is offline stays in the menu, marked, so a
    composition opened before the sender starts shows what it is waiting for.
*/
class OmtIn : public CFFGLPlugin
{
public:
	OmtIn();
	~OmtIn() override;

	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;

	// For the harness.
	enum Param : unsigned int
	{
		P_SOURCE,
		P_ADDRESS,
		P_FIT,
		P_NO_SIGNAL,
		P_KEEP_ALPHA,
		P_STATUS,
		P_COUNT
	};
	enum Fit
	{
		FIT_FIT,
		FIT_FILL,
		FIT_STRETCH,
		FIT_NATIVE
	};
	enum NoSignal
	{
		NS_HOLD,
		NS_TRANSPARENT,
		NS_BLACK
	};
	static constexpr int kMenuSources = 30;
	static constexpr int kMenuElements = 2 + kMenuSources;// None, Address, sources
	const std::vector< std::string >& menuNames() const { return menuLabels; }
	const std::vector< float >& menuValues() const { return menuKeys; }
	std::string statusText() const { return status; }
	somt::ReceiverStatus receiverStatus() const { return receiver.status(); }
	uint64_t framesUploaded() const { return uploads; }

private:
	void setAddress( const std::string& address, bool fromHost );
	void refreshMenu( bool force );
	void refreshStatus( bool force );
	bool upload( const somt::VideoFrame& frame );

	somt::Receiver receiver;
	bool subscribed = false;

	ffglex::FFGLShader shader;
	ffglex::FFGLScreenQuad quad;
	GLuint texture = 0;
	int textureWidth = 0;
	int textureHeight = 0;
	somt::VideoFrame front;
	bool haveFrame = false;
	bool frameAlpha = false;
	float frameAspect = 1.0f;
	uint64_t uploads = 0;

	float sourceValue = 0.0f;
	std::string address;
	int fit = FIT_FIT;
	int noSignal = NS_HOLD;
	bool keepAlpha = true;
	std::string status = "Pick a source";
	// GetTextParameter hands the host a bare pointer, so each must outlive the
	// call, and one per parameter so reading one never moves the other.
	std::string addressReturn;
	std::string statusReturn;

	std::vector< std::string > menuLabels;
	std::vector< float > menuKeys;
	uint64_t menuGeneration = ~0ull;
	std::string menuAddress;
	std::chrono::steady_clock::time_point lastStatus {};
};
