#pragma once

#if defined(GENERALS_ONLINE)

/*
**	Gencast client log.
**
**	Direct mode is tested by someone at another keyboard, so "look at live_observer_debug.log" is
**	not a debugging loop. Every line written to those files is mirrored to a per-install log and,
**	when direct mode is configured, shipped to the relay's /direct/clientlog endpoint tagged with
**	an id that is stable for this install. The relay writes it next to its own log, so client and
**	server lines can be read in one place.
**
**	Nothing here may interfere with the game: the queue is bounded, a line push is a queue push,
**	the POST happens on a detached worker with a 3 s timeout, and every failure is silent.
*/

/// Stable id for this install ("<hostname>-<6 hex>"), created on first use. Never NULL.
const char* GencastClientId();

/// The instance number this process reports with its lines (streamer and observer can share
/// an install, so they share an id but not an instance).
int GencastClientInstance();

/// Queue one line for the local file and, when configured, the relay. Thread-safe; no-op on NULL.
void GencastClientLogLine(const char* text);

/// printf-style convenience: formats, then GencastClientLogLine.
void GencastDebugLog(const char* fmt, ...);

#endif // GENERALS_ONLINE
