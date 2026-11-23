// The OMT In bundle's one registration. FFGL resolves one plugMain per
// binary, so the source and the effect are two bundles; this file is listed
// only in the source's target.
#include "OmtIn.h"

static CFFGLPluginInfo PluginInfo(
	PluginFactory< OmtIn >,// Create method
	"OM01",                // Plugin unique ID, 4 chars
	"SW OMT In",           // Plugin name (16 bytes, not terminated)
	2,                     // API major version
	1,                     // API minor version
	0,                     // Plugin major version
	1,                     // Plugin minor version
	FF_SOURCE,             // Plugin type
	"Receives an Open Media Transport (OMT) source from the network.\n\nPick a source from the menu, which lists what is on the network live, or type a name or an omt:// address. vMix, OBS and anything else that sends OMT can feed Resolume this way, with alpha.",
	"SW OMT In: Open Media Transport receiver" );
