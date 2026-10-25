/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

///////////////////////////////////////////////////////////////////////////////////////
// FILE: LiveObserverSession.h
// Description: Queues and pumps a live-observer session. Joining a livestream is a two-step
// affair that no single screen can own: the intent is recorded from whichever screen picked the
// game, and the connect-then-start-playback sequence is pumped by whichever screen the player is
// standing on when it completes. This module holds that state machine so no menu has to.
///////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Lib/BaseType.h"

#include <functional>

#if defined(GENERALS_ONLINE)

/// Queue a live-observer session for the given lobby. Records the intent only: the join waits on
/// the relay's HEADER and then starts a game, neither of which may happen while a screen is still
/// animating, so the screen the player lands on performs it via LiveObserverStartPendingSession().
///
/// password is the password of a password-protected stream (sent with the watch-ticket request);
/// displayName is the lobby's name, used to title the password reprompt popup; delaySeconds is
/// the lobby's broadcast delay (-1 = unknown), which pre-seeds the countdown and join timeout
/// until GO's admission hold or the relay's ROLE pins the real value; matchId names which match
/// of that lobby to watch (direct mode - empty means "whichever the relay considers newest").
void StartLiveObserverSession(const AsciiString& lobbyId,
	const AsciiString& password = AsciiString::TheEmptyString,
	const AsciiString& displayName = AsciiString::TheEmptyString,
	Int delaySeconds = -1,
	const AsciiString& matchId = AsciiString::TheEmptyString);

/// Abort a queued or connecting live-observer session. The only other way out is the join
/// timeout; the read-only lobby view needs LEAVE to cancel mid-wait.
void CancelLiveObserverPendingSession(void);

/// TRUE while a live-observer session is queued or connecting (between
/// StartLiveObserverSession and playback starting). Used by the lobby-observer screen to tell
/// "still waiting" from "the join was abandoned" without owning the session's internals.
Bool LiveObserverPendingSessionActive(void);

/// Start a queued live-observer session if one is pending and the shell has settled; a no-op
/// otherwise. Returns TRUE when playback actually started, which means the calling screen should
/// now stand itself down so the running game is visible.
///
/// Must be pumped from every shell screen the browser can be reached from, because a session is
/// queued from whichever one the player happens to be on and only that screen can tear itself
/// down afterwards.
Bool LiveObserverStartPendingSession(void);

// ---------------------------------------------------------------------------------------
// The watch password gate
// ---------------------------------------------------------------------------------------
//
// Both ways into a watch - the read-only pre-game view and a live stream - are gated by the same
// question, "would the relay accept this password right now?", and answered by the same call
// (POST /direct/join). Only what a screen does with the answer differs, so the request and its
// classification live here, once, and the two call sites say what entering means for them.
//
// This deliberately runs before either path changes a screen. The pre-game view has no join of
// its own to fail, so without a gate it opens for any password at all; the live path used to pop
// the shell before anyone knew the password was wrong, which put the error on the main menu.
//
// The request is synchronous with the relay's five second timeout, so it runs on a worker thread
// and its answer is collected by liveWatchPumpPasswordCheck() - never from inside a frame.

/// What the relay answered about one watch password.
enum class WatchPasswordVerdict
{
	Accepted,			///< 200: the password is right and the stream is live
	AcceptedNotLive,	///< 404: the password is right, but there is no stream yet (pre-game)
	WrongPassword,		///< 401: the password is wrong, so the player must be asked again
	Gone,				///< 410: the relay has already dropped that lobby
	Unwatchable,		///< 403 or another refusal: no password can get this player in
	Unreachable,		///< the relay did not answer at all
	Unchecked			///< no direct transport to ask; the caller keeps its old behaviour
};

/// The continuation for one check, invoked on the main thread by liveWatchPumpPasswordCheck()
/// exactly once for every check that starts.
using WatchPasswordCallback = std::function<void(WatchPasswordVerdict verdict)>;

/// Ask the relay whether it would accept this password, without opening a session and without
/// touching any screen. A check already in the air is superseded: only the newest answer is
/// delivered, and a callback of a superseded or cancelled check is never called.
///
/// matchId names the game the way the watch itself will, because the probe is the same
/// POST /direct/join: without it a finished match in its post-game hold (or the second game of a
/// reused lobby) is probed as "the newest game that has not ended", which answers 404 and would
/// refuse a password that is in fact right.
void liveWatchValidatePassword(const AsciiString& lobbyId, const AsciiString& password,
	const AsciiString& matchId, WatchPasswordCallback done);

/// Deliver a finished check's answer to its continuation. Must be pumped by every screen the
/// password popup can be open over; it runs UI code, so only from the main thread.
/// Returns TRUE when an answer was delivered this call.
Bool liveWatchPumpPasswordCheck(void);

/// Drop a running check and its answer, so an answer that lands after the player left the screen
/// cannot open a view or start a session on a screen they are no longer on.
void CancelLiveWatchPasswordCheck(void);

#endif // GENERALS_ONLINE
