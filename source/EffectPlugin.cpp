// The OMT Out bundle's one registration; see SourcePlugin.cpp.
#include "OmtOut.h"

static CFFGLPluginInfo PluginInfo(
	PluginFactory< OmtOut >,// Create method
	"OM02",                 // Plugin unique ID, 4 chars
	"SW OMT Out",           // Plugin name (16 bytes, not terminated)
	2,                      // API major version
	1,                      // API minor version
	0,                      // Plugin major version
	1,                      // Plugin minor version
	FF_EFFECT,              // Plugin type
	"Sends whatever it is on (a clip, a layer, a group or the whole composition) to the network as an Open Media Transport (OMT) source, and passes the picture through unchanged.\n\nvMix, OBS and anything else that receives OMT will list it as HOST (Name).",
	"SW OMT Out: Open Media Transport sender" );
