#include "PreRTS.h"

#if defined(GENERALS_ONLINE)

#include "Common/GencastBlackout.h"
#include "Common/GameAudio.h"
#include "Common/NameKeyGenerator.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"

#include <vector>

/// The HUD is one window tree: ControlBar.wnd holds the minimap, the command bar, the money and the
/// power display. W3DInGameUI::draw keeps repainting windows while blacked out - that is how the
/// quit menu is drawn, and a menu that cannot paint reads as a frozen client - so the way to take
/// the minimap away with the field is to hide the windows themselves.
///
/// Hiding the root alone is not enough: GameWindowManager::winRepaint walks the flat window list and
/// skips a window on its own hidden bit, so every child has to be hidden too. What was hidden on the
/// way in is remembered, so a window the game had already hidden (an unused purchase button, say) is
/// still hidden when the blackout lifts.
struct GencastHiddenWindow
{
	GameWindow *window;
	Bool wasHidden;
};

static NameKeyType s_controlBarNameKey = NAMEKEY_INVALID;
static std::vector<GencastHiddenWindow> s_hiddenByBlackout;

static void gencastHideTree(GameWindow *window, Bool hide)
{
	if (window == nullptr)
		return;

	for (GameWindow *child = window->winGetChild(); child != nullptr; child = child->winGetNext())
	{
		gencastHideTree(child, hide);
	}

	if (hide)
	{
		GencastHiddenWindow entry;
		entry.window = window;
		entry.wasHidden = (window->winGetStatus() & WIN_STATUS_HIDDEN) ? TRUE : FALSE;

		s_hiddenByBlackout.push_back(entry);

		window->winHide(TRUE);
	}
	else
	{
		window->winHide(FALSE);
	}
}

static void gencastHideHud(Bool hide)
{
	if (hide)
	{
		if (TheWindowManager == nullptr || TheNameKeyGenerator == nullptr)
			return;

		if (s_controlBarNameKey == NAMEKEY_INVALID)
			s_controlBarNameKey = TheNameKeyGenerator->nameToKey("ControlBar.wnd");

		s_hiddenByBlackout.clear();
		gencastHideTree(TheWindowManager->winGetWindowFromId(nullptr, s_controlBarNameKey), TRUE);

		return;
	}

	for (const GencastHiddenWindow& entry : s_hiddenByBlackout)
	{
		if (entry.window == nullptr)
			continue;

		entry.window->winHide(entry.wasHidden ? TRUE : FALSE);
	}

	s_hiddenByBlackout.clear();
}

Bool TheGenCastBlackout = FALSE;

void GencastSetBlackout(Bool on)
{
	if (TheGenCastBlackout == on)
		return;

	TheGenCastBlackout = on;

	// The minimap must go with the field, not stay behind it.
	gencastHideHud(on);

	if (TheAudio == nullptr)
		return;

	if (on)
		TheAudio->muteAudio(AudioManager::MuteAudioReason_GencastBlackout);
	else
		TheAudio->unmuteAudio(AudioManager::MuteAudioReason_GencastBlackout);
}

#endif // GENERALS_ONLINE
