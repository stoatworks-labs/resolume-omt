#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include <FFGLSDK.h>

#include "StoatworksAboutParams.h"

#include "Sender.h"

/**
    SW OMT Out: whatever it is applied to (a clip, a layer, a group, the
    composition) goes out as an OMT source, and passes through unchanged.

    The render thread never waits for the GPU: each frame is drawn into a
    frame-sized buffer (flipped so the top row comes first, as OMT wants),
    read into one of three pixel-pack buffers with a fence, and handed to the
    sender when its fence has passed, normally on the next frame. So OMT Out
    adds one frame of latency and costs the render thread a copy.
*/
class OmtOut : public CFFGLPlugin
{
public:
	OmtOut();
	~OmtOut() override;

	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;

	enum Param : unsigned int
	{
		P_NAME,
		P_SEND,
		P_QUALITY,
		P_ALPHA,
		P_RATE,
		P_STATUS,
		// The fleet's About block, last: a text line and its buttons.
		P_ABOUT_FIRST,
		P_COUNT = P_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	// For the harness.
	somt::SenderStatus senderStatus() const { return sender.status(); }
	std::string statusText() const { return status; }
	uint64_t readbacksDropped() const { return pipelineDrops; }
	void rateInUse( int& n, int& d ) const
	{
		n = rateN;
		d = rateD;
	}
	/// Wait for every readback in flight and hand it over (harness only).
	void flushForTest();

private:
	struct Slot
	{
		GLuint pbo = 0;
		GLsync fence = nullptr;
		size_t capacity = 0;
		int width = 0;
		int height = 0;
		int64_t timestamp = 0;
		bool alpha = false;
	};
	static constexpr int kSlots = 3;

	void harvest( bool wait );
	void issueReadback( const FFGLTextureStruct& input, int64_t timestamp );
	void updateRate();
	void refreshStatus( bool force );
	bool ensureTarget( int width, int height );
	void releaseSlots();

	somt::Sender sender;

	ffglex::FFGLShader passShader;
	ffglex::FFGLShader captureShader;
	ffglex::FFGLScreenQuad quad;
	GLuint fbo = 0;
	GLuint colour = 0;
	int targetWidth = 0;
	int targetHeight = 0;
	Slot slots[ kSlots ];
	int nextSlot = 0;
	int inFlight = 0;
	uint64_t pipelineDrops = 0;

	std::string name = "Resolume";
	bool sendOn = true;
	int quality = 0;// index into the Quality menu
	bool alpha = false;
	int rateChoice = 0;// 0 = Auto
	std::string status = "Not sending yet";
	std::string nameReturn;
	std::string statusReturn;

	// Auto frame rate: the host's render rate, measured, snapped to a
	// broadcast rate and held unless another wins for a second.
	std::chrono::steady_clock::time_point lastFrameTime {};
	double meanInterval = 0.0;
	int rateN = 60;
	int rateD = 1;
	int candidateN = 60;
	int candidateD = 1;
	int candidateFrames = 0;
	std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
	// A fixed Frame Rate paces sending to it (Arena renders at the display's
	// rate, often 100 or 120 Hz). Frames go out on an ideal timeline.
	std::chrono::steady_clock::time_point nextDue {};
	int64_t paceIndex = 0;
	int64_t paceStart = 0;// 100 ns, so a restarted timeline never runs backwards
	int pacedChoice = -1;
	bool dueThisFrame( int64_t& timestamp );
	std::chrono::steady_clock::time_point lastStatus {};
};
