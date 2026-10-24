#pragma once

#include "Lib/BaseType.h"

/*
**	Gencast blackout.
**
**	A defeated player who is still streaming cannot be dropped from the match: dropping them
**	ends the broadcast for every watcher. They stay in the game and keep feeding the relay, but
**	their own view is blacked out and their audio muted, so being eliminated cannot be used to
**	watch for the surviving players.
**
**	Set by GameLogic's end-of-frame cleanup, read by the renderer and the audio update path.
**	Always FALSE when the local player is not streaming, which keeps vanilla behaviour identical.
*/

extern Bool TheGenCastBlackout;

/// Sets the flag and, on a change, mutes or unmutes game audio for the streaming player.
/// Cheap to call every frame: it returns immediately unless the state actually changed.
void GencastSetBlackout(Bool on);
