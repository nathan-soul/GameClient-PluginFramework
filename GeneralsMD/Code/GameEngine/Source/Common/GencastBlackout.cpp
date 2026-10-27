#include "PreRTS.h"

#if defined(GENERALS_ONLINE)

#include "Common/GencastBlackout.h"
#include "Common/GameAudio.h"
#include "Common/NameKeyGenerator.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"

/// The HUD is one window tree: ControlBar.wnd holds the minimap, the command bar, the money and the
/// power display. Hiding that root is what removes the minimap - W3DInGameUI::draw keeps repainting
/// windows while blacked out, because that is also how the quit menu is drawn, and a defeated
/// streamer must not be able to read a minimap that the field is supposed to hide.
static NameKeyType s_controlBarNameKey = NAMEKEY_INVALID;
static Bool s_controlBarFound = FALSE;
static Bool s_controlBarWasHidden = FALSE;

static GameWindow *gencastControlBar()
{
	if (s_controlBarFound)
		return TheWindowManager == nullptr ? nullptr : TheWindowManager->winGetWindowFromId(nullptr, s_controlBarNameKey);

	if (TheWindowManager == nullptr || TheNameKeyGenerator == nullptr)
		return nullptr;

	s_controlBarNameKey = TheNameKeyGenerator->nameToKey("ControlBar.wnd");
	s_controlBarFound = TRUE;

	return TheWindowManager->winGetWindowFromId(nullptr, s_controlBarNameKey);
}

Bool TheGenCastBlackout = FALSE;

void GencastSetBlackout(Bool on)
{
	if (TheGenCastBlackout == on)
		return;

	TheGenCastBlackout = on;

	// The HUD out of the way (or back): the minimap must go with the field, not stay behind it.
	GameWindow *controlBar = gencastControlBar();

	if (controlBar != nullptr)
	{
		if (on)
		{
			s_controlBarWasHidden =
				(controlBar->winGetStatus() & WIN_STATUS_HIDDEN) ? TRUE : FALSE;
			controlBar->winHide(TRUE);
		}
		else
		{
			controlBar->winHide(s_controlBarWasHidden ? TRUE : FALSE);
		}
	}

	if (TheAudio == nullptr)
		return;

	if (on)
		TheAudio->muteAudio(AudioManager::MuteAudioReason_GencastBlackout);
	else
		TheAudio->unmuteAudio(AudioManager::MuteAudioReason_GencastBlackout);
}

#endif // GENERALS_ONLINE
