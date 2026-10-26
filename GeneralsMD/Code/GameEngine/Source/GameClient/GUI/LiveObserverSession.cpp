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
// FILE: LiveObserverSession.cpp
// The live-observer join state machine. Every shell screen that can queue or pump a session calls
// in here - the Watch Live browser, the pre-game lobby view, the Online welcome screen, the main
// menu and the password popup - and none of them owns the state.
///////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#if defined(GENERALS_ONLINE)

#include "GameClient/LiveObserverSession.h"

#include "Common/GlobalData.h"
#include "Common/GencastTransport.h"		// the password gate's /direct/join probe
#include "Common/LiveObserver.h"
#include "Common/Recorder.h"
#include "GameClient/ClientInstance.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindowManager.h"	// MessageBoxYesNo, via liveWatchPromptTakeOver
#include "GameClient/GameWindowTransitions.h"
#include "GameClient/GUICallbacks.h"		// liveWatchOpenPasswordPopup
#include "GameClient/MessageBox.h"
#include "GameClient/Shell.h"
#include "GameNetwork/GameSpyOverlay.h"
#include "GameNetwork/GeneralsOnline/GeneralsOnline_Settings.h"	// direct mode: the gate's scope
#include "GameNetwork/GeneralsOnline/OnlineServices_Auth.h"		// GetUserID: the join's watcher id
#include "GameNetwork/GeneralsOnline/OnlineServices_Init.h"		// NGMP_OnlineServicesManager

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <windows.h>						// timeGetTime

// The queued session. Written by StartLiveObserverSession from whichever screen picked the
// game, read by the pump below from whichever screen the player is standing on.
static Bool startLiveObserverGame = FALSE;
static AsciiString m_liveObserverStartLobbyId;
static AsciiString s_liveObserverPassword;		// password for a password-protected stream
static AsciiString s_liveObserverDisplayName;	// lobby name, for the password reprompt title
static Int s_liveObserverDelaySeconds = -1;		// lobby broadcast delay, -1 = unknown
static AsciiString s_liveObserverMatchId;		// which match of that lobby (direct mode, may be empty)
static Bool s_liveObserverKickExisting = FALSE;	// replace this account's session on that match

// Records the intent only. doLiveObserverGameStart() blocks waiting for the relay's HEADER, so it
// must not run while another screen is up; the pump fires it once the shell has settled.
void StartLiveObserverSession(const AsciiString& lobbyId,
	const AsciiString& password, const AsciiString& displayName, Int delaySeconds,
	const AsciiString& matchId, Bool kickExisting)
{
	if (lobbyId.isEmpty())
		return;

	// Deliberately does not set m_afterIntro: that re-enters the intro/movie machinery, which sets
	// m_breakTheMovie and disables rendering until a menu or load screen clears it, freezing the
	// screen while the logic keeps running. The shell map stays off so nothing competes with the
	// replay about to start.
	if (TheWritableGlobalData)
	{
		TheWritableGlobalData->m_playIntro = FALSE;
		TheWritableGlobalData->m_playSizzle = FALSE;
		TheWritableGlobalData->m_shellMapOn = FALSE;
	}

	// Multi-instance support, like replay mode.
	rts::ClientInstance::setMultiInstance(TRUE);
	rts::ClientInstance::skipPrimaryInstance();

	m_liveObserverStartLobbyId = lobbyId;
	s_liveObserverPassword = password;
	s_liveObserverDisplayName = displayName;
	s_liveObserverDelaySeconds = delaySeconds;
	s_liveObserverMatchId = matchId;
	s_liveObserverKickExisting = kickExisting;
	startLiveObserverGame = TRUE;

	liveObserverLog("StartLiveObserverSession: queued lobby %s match %s%s\n", lobbyId.str(),
		matchId.isEmpty() ? "(unspecified)" : matchId.str(),
		kickExisting ? " (taking over this account's session)" : "");
}

// The join splits into a non-blocking connect and a playback start. The wait between them, for
// the relay to deliver the header plus enough body to cover the broadcast delay, must not block
// the main loop or the shell cannot draw the countdown explaining it. Readiness itself lives on
// LiveObserver::isPlaybackReady; this file only sequences it.
enum ObserverJoinPhase
{
	kObserverJoinIdle,		// nothing pending
	kObserverJoinWaiting,	// connected; waiting for the file to cover the delay
};
static ObserverJoinPhase s_observerJoinPhase = kObserverJoinIdle;

// Mirrors the timeout path in LiveObserverStartPendingSession.
void CancelLiveObserverPendingSession(void)
{
	liveObserverLog("CancelLiveObserverPendingSession: cancelling queued observer join\n");

	if (s_observerJoinPhase == kObserverJoinWaiting && TheLiveObserver != nullptr)
	{
		liveObserverEndSession();
	}

	s_observerJoinPhase = kObserverJoinIdle;
	startLiveObserverGame = FALSE;
	m_liveObserverStartLobbyId.clear();

	s_liveObserverKickExisting = FALSE;

	// A cancelled session must not leak its password into the next join.
	s_liveObserverPassword.clear();
	s_liveObserverDisplayName.clear();
	s_liveObserverDelaySeconds = -1;
	// Nor its match: the next session names its own game, and a stale id would ask the relay for
	// a match this viewer never picked.
	s_liveObserverMatchId.clear();
}

Bool LiveObserverPendingSessionActive(void)
{
	return startLiveObserverGame;
}

// Phase 1: end any previous session and start connecting. Non-blocking - the network thread
// does the work and publishes the header and watermarks as it goes.
static Bool doLiveObserverConnect(void)
{
	liveObserverInitLog(m_liveObserverStartLobbyId.str());

	// End any previous session outright. This destroys the old LiveObserver and releases the
	// Recorder's read handle on its file - both are needed, because the live file is named after
	// the streamer's game, so rejoining a game already watched targets the same path, which
	// openLiveFile() cannot delete or recreate while either handle is open.
	liveObserverEndSession();

	TheLiveObserver = createLiveObserver();
	if (!TheLiveObserver)
	{
		liveObserverLog("doLiveObserverConnect: createLiveObserver() returned NULL!\n");
		return FALSE;
	}

	liveObserverLog("doLiveObserverConnect: connecting to relay for lobby %s\n",
		m_liveObserverStartLobbyId.str());
	TheLiveObserver->connect(m_liveObserverStartLobbyId, s_liveObserverPassword.str(),
		s_liveObserverDelaySeconds, s_liveObserverMatchId, s_liveObserverKickExisting);
	return TRUE;
}

// Phase 2: the file is playable (LiveObserver::isPlaybackReady), so start playback. Returns TRUE
// only when playback actually started, so the shell screen that pumped this can tear itself down
// and reveal the game.
static Bool doLiveObserverStartPlayback(void)
{
	if (TheLiveObserver == nullptr)
		return FALSE;

	const AsciiString filename = TheLiveObserver->getLiveReplayFilename();
	if (!TheRecorder->startLiveObserverPlayback(filename))
	{
		liveObserverLog("doLiveObserverStartPlayback: FAILED - playbackFile returned false\n");
		liveObserverEndSession();
		return FALSE;
	}

	liveObserverLog("doLiveObserverStartPlayback: playback started\n");
	return TRUE;
}

// Fire a queued session once the shell has settled, then keep pumping while the relay builds the
// file. Returns FALSE while still buffering, so the shell keeps drawing the countdown, and TRUE
// once playback has started, so the caller can stand itself down and reveal the game.
Bool LiveObserverStartPendingSession(void)
{
	// The password gate in front of this queue is part of the same watch pipeline: its answer is
	// collected here so a re-prompt opened from the rejection path below - which lands on whatever
	// screen the player was returned to - still completes. Every screen that pumps a session pumps
	// the gate with it, which is why no screen has to remember to.
	liveWatchPumpPasswordCheck();

	if (!startLiveObserverGame)
		return FALSE;

	if (s_observerJoinPhase == kObserverJoinIdle)
	{
		if (!TheShell->isAnimFinished() || !TheTransitionHandler->isFinished())
			return FALSE;

		if (!doLiveObserverConnect())
		{
			startLiveObserverGame = FALSE;
			return FALSE;
		}
		s_observerJoinPhase = kObserverJoinWaiting;
		liveObserverLog("LiveObserverStartPendingSession: connected, waiting for the file to cover the delay (up to %ums)\n",
			TheLiveObserver->getJoinTimeoutMs());
		return FALSE;
	}

	// kObserverJoinWaiting - pump the wait. Every failure path clears the join state.
	if (TheLiveObserver == nullptr)
	{
		s_observerJoinPhase = kObserverJoinIdle;
		startLiveObserverGame = FALSE;
		return FALSE;
	}

	if (TheLiveObserver->isPlaybackReady())
	{
		s_observerJoinPhase = kObserverJoinIdle;
		startLiveObserverGame = FALSE;
		return doLiveObserverStartPlayback();
	}

	// The one reprompt path: it covers both a wrong password typed into the browser popup and the
	// pre-game handoff of a passworded lobby.
	if (TheLiveObserver->isPasswordRejected())
	{
		liveObserverLog("LiveObserverStartPendingSession: password rejected for lobby %s\n",
			m_liveObserverStartLobbyId.str());

		liveObserverEndSession();
		s_observerJoinPhase = kObserverJoinIdle;
		startLiveObserverGame = FALSE;

		// The queue statics still hold the lobby id, display name and match - they are cleared by
		// CancelLiveObserverPendingSession or overwritten by the next StartLiveObserverSession.
		// The reprompt keeps all three, so the retry asks the relay about the same game.
		GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"),
			TheGameText->fetch("GUI:JoinFailedBadPassword"), []()
			{
				liveWatchOpenPasswordPopup(m_liveObserverStartLobbyId, s_liveObserverDisplayName,
					FALSE, s_liveObserverMatchId);
			});

		return FALSE;
	}

	// This account already watches this match, somewhere else. Only the player can say whether
	// that other connection may be dropped, and the answer decides the retry: with kick_existing
	// the relay closes it and hands this one the session.
	if (TheLiveObserver->isDuplicateSession())
	{
		const std::string since = TheLiveObserver->getDuplicateSince();
		const std::string client = TheLiveObserver->getDuplicateClient();

		liveObserverLog("LiveObserverStartPendingSession: duplicate session for lobby %s "
			"(since=%s client=%s), asking whether to take it over\n",
			m_liveObserverStartLobbyId.str(), since.empty() ? "?" : since.c_str(),
			client.empty() ? "?" : client.c_str());

		liveObserverEndSession();
		s_observerJoinPhase = kObserverJoinIdle;
		startLiveObserverGame = FALSE;

		liveWatchPromptTakeOver(since, client, []()
			{
				// Re-queue the same watch; the statics still name the game, the password and the
				// match, so the retry is the same question with the takeover flag set.
				const AsciiString lobbyId = m_liveObserverStartLobbyId;
				const AsciiString password = s_liveObserverPassword;
				const AsciiString displayName = s_liveObserverDisplayName;
				const AsciiString matchId = s_liveObserverMatchId;
				const Int delaySeconds = s_liveObserverDelaySeconds;

				StartLiveObserverSession(lobbyId, password, displayName, delaySeconds, matchId, TRUE);
			});

		return FALSE;
	}

	if (timeGetTime() > TheLiveObserver->getJoinDeadlineMs())
	{
		liveObserverLog("LiveObserverStartPendingSession: timed out waiting for a playable file - abandoning the join "
			"(now=%ums deadline=%ums connected=%d serverHeld=%d delayWait=%d)\n",
			timeGetTime(), TheLiveObserver->getJoinDeadlineMs(),
			TheLiveObserver->isConnected() ? 1 : 0,
			TheLiveObserver->isServerHeld() ? 1 : 0,
			TheLiveObserver->isWaitingForBroadcastDelay() ? 1 : 0);
		liveObserverEndSession();
		s_observerJoinPhase = kObserverJoinIdle;
		startLiveObserverGame = FALSE;
		return FALSE;
	}

	return FALSE;
}

// ============================================================================
// The watch password gate
// ============================================================================
//
// One question, one request, one place: POST /direct/join with the password, classified into a
// verdict. What the answer means for a screen is the call sites' business - this half only talks
// to the relay and hands back what it said.
//
// The relay's order matters and is what makes a single request enough for both paths
// (DirectEndpoints.Join): it tests the password *before* it tests whether a stream exists. So 401
// is always "wrong password", while 404 on a password-protected lobby can only mean "the password
// is right and there is no stream yet", which is exactly the pre-game answer.

namespace
{
	// One check at a time. The epoch is what makes a superseded or cancelled check harmless: the
	// worker re-reads it under the lock before publishing, so an answer the player has walked away
	// from is dropped instead of opening a screen they left.
	std::mutex s_passwordGateMutex;
	UnsignedInt s_passwordGateEpoch = 0;
	Bool s_passwordGateRunning = FALSE;
	Bool s_passwordGateAnswered = FALSE;
	WatchPasswordVerdict s_passwordGateVerdict = WatchPasswordVerdict::Unreachable;
	WatchPasswordCallback s_passwordGateDone;

	WatchPasswordVerdict classifyWatchPasswordAnswer(const GencastAttemptResult& attempt)
	{
		switch (attempt.kind)
		{
			case EGencastAttempt::Success:
				return WatchPasswordVerdict::Accepted;

			case EGencastAttempt::WrongPassword:
				return WatchPasswordVerdict::WrongPassword;

			// Not a password answer: the password was accepted, but this account already has a
			// session on this match, so no new one was issued. That is not a rejection the player
			// can fix by typing again, and it is not a reason to refuse the screen either - the
			// join that follows is the moment the takeover question belongs, because only there
			// is there a session of the player's own to keep.
			case EGencastAttempt::Duplicate:
				return WatchPasswordVerdict::Accepted;

			case EGencastAttempt::NotFound:
				// See above: the password is tested before liveness, so this is not a rejection.
				return WatchPasswordVerdict::AcceptedNotLive;

			case EGencastAttempt::Held:		// Services-only, cannot come from this probe
			case EGencastAttempt::Failed:
			default:
				break;
		}

		// Failed covers every status the transport does not name: 410 is a lobby the relay has
		// already dropped, 400 a malformed request, and anything else is a refusal no password
		// can fix.
		switch (attempt.httpStatus)
		{
			case 410:
				return WatchPasswordVerdict::Gone;

			case 403:
				// Wrong watch group: the account is not allowed to see this game at all, which
				// the player needs told apart from a wrong password.
				return WatchPasswordVerdict::NotInWatchGroup;

			case 400:
				return WatchPasswordVerdict::Unwatchable;

			case 0:
				return WatchPasswordVerdict::Unreachable;

			default:
				return WatchPasswordVerdict::Unwatchable;
		}
	}

	void watchPasswordCheckThread(std::string lobbyId, std::string password, std::string matchId,
		uint64_t watcherId, UnsignedInt epoch)
	{
		WatchPasswordVerdict verdict = WatchPasswordVerdict::Unreachable;

		// The relay transport directly, not GencastTransport::Create(): Create() re-runs the mode
		// probe, which can reach GO for a HEAD request, and GO's interfaces are not for a worker
		// thread. The mode decision was already taken on the main thread before this thread
		// existed. Same reasoning as the browser's own list fetch.
		std::unique_ptr<GencastTransport> transport(CreateRelayGencastTransport());

		if (!transport)
		{
			verdict = WatchPasswordVerdict::Unchecked;
		}
		else
		{
			// The same join the watch itself will make, match id and watcher id included: probing
			// another game of that lobby (or another account) would answer for a session this
			// player never asked for.
			const GencastAttemptResult attempt =
				transport->AttemptWatchSession(lobbyId, password, matchId, watcherId, false);
			verdict = classifyWatchPasswordAnswer(attempt);
		}

		liveObserverLog("liveWatchPasswordGate: lobby=%s match=%s verdict=%d\n", lobbyId.c_str(),
			matchId.empty() ? "(unspecified)" : matchId.c_str(), (Int)verdict);

		std::lock_guard<std::mutex> lock(s_passwordGateMutex);

		if (epoch != s_passwordGateEpoch)
		{
			liveObserverLog("liveWatchPasswordGate: lobby=%s answer superseded or cancelled\n",
				lobbyId.c_str());
			return;
		}

		s_passwordGateRunning = FALSE;
		s_passwordGateAnswered = TRUE;
		s_passwordGateVerdict = verdict;
	}
}

void liveWatchValidatePassword(const AsciiString& lobbyId, const AsciiString& password,
	const AsciiString& matchId, WatchPasswordCallback done)
{
	if (!done)
		return;

	if (lobbyId.isEmpty())
	{
		done(WatchPasswordVerdict::Unchecked);
		return;
	}

	// Direct mode is decided here, on the main thread: resolving the mode can run a blocking HEAD
	// against GO, and that does not belong on a worker. Same test the announce uses for "direct
	// mode is on". Without a relay there is no endpoint to ask, and GO's ticket step stays the
	// password gate it is today.
	if (!NGMP_OnlineServicesManager::Settings.Relay_Direct() ||
		NGMP_OnlineServicesManager::Settings.Relay_Direct_Url().empty())
	{
		done(WatchPasswordVerdict::Unchecked);
		return;
	}

	UnsignedInt epoch = 0;

	{
		std::lock_guard<std::mutex> lock(s_passwordGateMutex);

		// Supersede whatever is still in the air: the newest attempt is the one the player is
		// waiting on, and the older answer belongs to a password they have already replaced.
		epoch = ++s_passwordGateEpoch;
		s_passwordGateRunning = TRUE;
		s_passwordGateAnswered = FALSE;
		s_passwordGateDone = std::move(done);
	}

	// The account the probe is for, read here: every join carries a watcher id, and GO's auth
	// interface belongs to the main thread, not to the worker below.
	uint64_t watcherId = 0;
	NGMP_OnlineServices_AuthInterface* auth =
		NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();

	if (auth != nullptr)
	{
		const int64_t userId = auth->GetUserID();

		if (userId > 0)
		{
			watcherId = (uint64_t)userId;
		}
	}

	liveObserverLog("liveWatchPasswordGate: lobby=%s asking the relay (watcher=%llu)\n",
		lobbyId.str(), (unsigned long long)watcherId);

	try
	{
		std::thread(watchPasswordCheckThread, std::string(lobbyId.str()),
			std::string(password.str()), std::string(matchId.str()), watcherId, epoch).detach();
	}
	catch (...)
	{
		// No worker, so no answer will ever arrive. Report "cannot check" instead of leaving the
		// popup closed with nothing pending behind it, which would look like a dead button.
		liveObserverLog("liveWatchPasswordGate: lobby=%s could not start a worker\n", lobbyId.str());

		std::lock_guard<std::mutex> lock(s_passwordGateMutex);
		s_passwordGateRunning = FALSE;
		s_passwordGateAnswered = TRUE;
		s_passwordGateVerdict = WatchPasswordVerdict::Unchecked;
	}
}

Bool liveWatchPumpPasswordCheck(void)
{
	WatchPasswordCallback done;
	WatchPasswordVerdict verdict = WatchPasswordVerdict::Unreachable;

	{
		std::lock_guard<std::mutex> lock(s_passwordGateMutex);

		if (!s_passwordGateAnswered)
			return FALSE;

		s_passwordGateAnswered = FALSE;
		verdict = s_passwordGateVerdict;
		done = std::move(s_passwordGateDone);
		s_passwordGateDone = nullptr;
	}

	// Outside the lock: the continuation pushes shell screens, opens message boxes and starts
	// sessions, and none of that may run while a worker is waiting to publish.
	if (done)
		done(verdict);

	return TRUE;
}

void CancelLiveWatchPasswordCheck(void)
{
	std::lock_guard<std::mutex> lock(s_passwordGateMutex);

	if (!s_passwordGateRunning && !s_passwordGateAnswered && !s_passwordGateDone)
		return;

	liveObserverLog("liveWatchPasswordGate: cancelled\n");

	++s_passwordGateEpoch;		// an answer still in the air is now stale
	s_passwordGateRunning = FALSE;
	s_passwordGateAnswered = FALSE;
	s_passwordGateDone = nullptr;
}

// ============================================================================
// The takeover question
// ============================================================================
//
// Message boxes take a bare function pointer for their buttons, so the continuation has to
// outlive the call that raises the box. One at a time, like the boxes themselves.
static std::function<void()> s_takeOverConfirm;

static void onTakeOverConfirmed()
{
	std::function<void()> confirmed = std::move(s_takeOverConfirm);
	s_takeOverConfirm = nullptr;

	if (confirmed)
		confirmed();
}

void liveWatchPromptTakeOver(const std::string& since, const std::string& client,
	std::function<void()> onConfirm)
{
	s_takeOverConfirm = std::move(onConfirm);

	// The two facts the relay reported, so the player can recognise the other connection: when it
	// started and which client it came from. Either may be missing (an older relay), and then the
	// wording stays general rather than printing an empty bracket.
	UnicodeString when;
	UnicodeString from;
	when.translate(AsciiString(since.empty() ? "?" : since.c_str()));
	from.translate(AsciiString(client.empty() ? "?" : client.c_str()));

	UnicodeString body;

	if (since.empty() && client.empty())
	{
		body.format(L"This account is already watching this game.\n\n"
			L"Disconnect that connection and watch here instead?\n\n"
			L"Only do this if the account is yours. If you did not start that other connection, "
			L"someone else is using your account to watch - disconnect it and change your password.");
	}
	else
	{
		body.format(L"This account is already watching this game (since %ls, client %ls).\n\n"
			L"Disconnect that connection and watch here instead?\n\n"
			L"Only do this if the account is yours. If you did not start that other connection, "
			L"someone else is using your account to watch - disconnect it and change your password.",
			when.str(), from.str());
	}

	MessageBoxYesNo(UnicodeString(L"Already watching"), body, onTakeOverConfirmed, nullptr);
}

#endif // GENERALS_ONLINE
