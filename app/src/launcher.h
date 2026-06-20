// launcher.h — launch another (signed) PS4 app, e.g. the native Media Player
// (CUSA02012), which has the hardware-decoder entitlement our homebrew lacks.
#ifndef PS4CAST_LAUNCHER_H
#define PS4CAST_LAUNCHER_H

// Naive (sandboxed) launch — returns NOT_ALLOWED from homebrew.
int launch_app(const char *title_id, const char *arg);

// Privileged launch via ShellUI (load prx + dlsym + LaunchByUri). 0 = ok.
int launch_by_uri(const char *uri);

// SystemService browser launch probe. Uses the already-linked system service
// library instead of loading ShellUIUtil/LncUtil dynamically.
int launch_web_browser(const char *url);

// Preferred handoff path for real casting. The PS4 native browser/player fetches
// the URL directly, so the phone can disconnect after sending the command.
int handoff_play_url(const char *url);
int handoff_stop(void);
const char *handoff_status(void);

// Detailed diagnostics from the last launch_by_uri call.
const char *launch_debug(void);

#endif
