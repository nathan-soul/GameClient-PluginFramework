#include "PreRTS.h"

#if defined(GENERALS_ONLINE)

#include "Common/GencastBlackout.h"
#include "Common/GameAudio.h"
#include "Common/NameKeyGenerator.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GUICallbacks.h"	// ToggleControlBar: the same call the observer HUD button makes

/// The HUD is not drawn by the HUD pass: the minimap, the command bar, the money and the power
/// display are all windows under ControlBar.wnd:ControlBarParent, and W3DInGameUI::draw keeps
/// repainting windows while blacked out (that is how the quit menu gets drawn). Hiding that root by
/// hand was the wrong tool - GameWindowManager::winRepaint walks the flat window list and looks at
/// each window's own hidden bit, so the children kept drawing.
///
/// The game already has the right tool: TOGGLE_CONTROL_BAR. Its handler in CommandXlat.cpp hides
/// ControlBar.wnd:ControlBarParent, grows the viewport to the full height and drops the special
/// power shortcut - which is exactly what an observer's "hide the HUD" button does. Use it rather
/// than a second implementation that has to keep the viewport and the bar in step by itself.
static Bool s_gencastHidControlBar = FALSE;

static GameWindow *gencastControlBarParent()
{
	if (TheWindowManager == nullptr || TheNameKeyGenerator == nullptr)
		return nullptr;

	static NameKeyType s_key = NAMEKEY_INVALID;

	if (s_key == NAMEKEY_INVALID)
		s_key = TheNameKeyGenerator->nameToKey("ControlBar.wnd:ControlBarParent");

	return TheWindowManager->winGetWindowFromId(nullptr, s_key);
}

Bool TheGenCastBlackout = FALSE;

void GencastSetBlackout(Bool on)
{
	if (TheGenCastBlackout == on)
		return;

	TheGenCastBlackout = on;

	if (on)
	{
		// A player who had already hidden the bar themselves keeps it hidden when the blackout
		// lifts: we only put back what we took away.
		GameWindow *bar = gencastControlBarParent();

		if (bar != nullptr && !bar->winIsHidden())
		{
			s_gencastHidControlBar = TRUE;
			ToggleControlBar();
		}
	}
	else if (s_gencastHidControlBar)
	{
		// The match is over (or the stream stopped): the HUD goes back exactly as it was.
		GameWindow *bar = gencastControlBarParent();

		if (bar != nullptr && bar->winIsHidden())
			ToggleControlBar();

		s_gencastHidControlBar = FALSE;
	}

	if (TheAudio == nullptr)
		return;

	if (on)
		TheAudio->muteAudio(AudioManager::MuteAudioReason_GencastBlackout);
	else
		TheAudio->unmuteAudio(AudioManager::MuteAudioReason_GencastBlackout);
}

#endif // GENERALS_ONLINE
