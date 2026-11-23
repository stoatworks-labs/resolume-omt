#pragma once

#include <string>

/**
    Logging for a plugin that lives inside somebody else's process.

    The fleet's smallest `diag`: a dated log file and nothing else. No crash
    handler (a plugin must not take over the host's signal handling) and no
    bundle command (there is no UI to hang one off).

    What it covers here is the failure an operator cannot see: the OMT
    runtime not loading, a sender that could not be created, a receiver that
    never got a frame. From the Resolume side all of those look like "the
    clip is black" or "nothing shows up in vMix". The log says which.

    ~/Library/Logs/resolume-omt/resolume-omt.<date>.log on macOS,
    %LOCALAPPDATA%\resolume-omt\logs on Windows; RESOLUME_OMT_LOG_DIR
    overrides both.
*/
namespace somt::diag
{

/// Open the log file and record the plugin build, once per process image.
void init();

void info( const std::string& message );
void warn( const std::string& message );
void error( const std::string& message );

std::string logPath();

} // namespace somt::diag
