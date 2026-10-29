#include "GameNetwork/GeneralsOnline/Plugins/PluginManager.h"
#include "GameNetwork/GeneralsOnline/NGMP_include.h"
#include "Common/GlobalData.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/Science.h"
#include "Common/SpecialPower.h"
#include "Common/ThingTemplate.h"
#include "Common/ThingFactory.h"
#include "Common/Upgrade.h"
#include "Common/Geometry.h"
#include "GameClient/InGameUI.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Display.h"
#include "GameClient/View.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/SpecialPowerModule.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/Module/SupplyTruckAIUpdate.h"
#include "Common/BuildAssistant.h" // CONSTRUCTION_COMPLETE
#include "GameNetwork/NetworkDefs.h" // MAX_SLOTS
#include "GameNetwork/GameInfo.h" // GameSlot::getTeamNumber

#include <algorithm>
#include <cctype>
#include <climits> // INT_MAX
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <stdio.h>
#include <string>

std::vector<GOPluginManager::LoadedPlugin> GOPluginManager::s_plugins;
std::vector<GOGameplayEventCallbacks> GOPluginManager::s_gameplayEventHooks;
std::vector<GORenderCallbacks> GOPluginManager::s_renderHooks;
GOPluginManager::NativeHandleProvider GOPluginManager::s_d3dDevice8Provider = nullptr;
GOPluginManager::NativeHandleProvider GOPluginManager::s_gameWindowProvider = nullptr;

// The free functions the host API table points at. All dispatch is gated on
// IsLocalPlayerObserver(), so plugins only ever run for observers.
namespace
{
	void HostAPI_Log(const char* msg)
	{
		GOPluginManager::Log(msg);
	}

	void HostAPI_RegisterGameplayEventHooks(const GOGameplayEventCallbacks* cb)
	{
		GOPluginManager::RegisterGameplayEventHooks(cb);
	}

	void HostAPI_RegisterRenderHooks(const GORenderCallbacks* cb)
	{
		GOPluginManager::RegisterRenderHooks(cb);
	}

	// ---- Player roster queries. Matches playerIndex against Player::getPlayerIndex(), not list
	// position - those aren't guaranteed to be the same thing. ----

	Player* FindPlayerByIndex(uint32_t playerIndex)
	{
		if (ThePlayerList == nullptr)
			return nullptr;
		Int count = ThePlayerList->getPlayerCount();
		for (Int i = 0; i < count; ++i)
		{
			Player* p = ThePlayerList->getNthPlayer(i);
			if (p != nullptr && (uint32_t)p->getPlayerIndex() == playerIndex)
				return p;
		}
		return nullptr;
	}

	uint32_t HostAPI_GetPlayerColor(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		return (p != nullptr) ? (uint32_t)p->getPlayerColor() : 0;
	}

	// Walks the engine's player-slot name keys; the callback returns false to stop early. Not free
	// per call - see plans\plugin-framework\design-notes.md.
	typedef bool (*SlotPlayerCallback)(Int slot, Player* player, void* userData);

	void ForEachSlotPlayer(SlotPlayerCallback callback, void* userData)
	{
		if (callback == nullptr || ThePlayerList == nullptr || TheNameKeyGenerator == nullptr)
			return;

		for (Int slot = 0; slot < MAX_SLOTS; ++slot)
		{
			AsciiString nameKeyStr;
			nameKeyStr.format("player%d", slot);

			Player* p = ThePlayerList->findPlayerWithNameKey(TheNameKeyGenerator->nameToKey(nameKeyStr));
			if (p == nullptr)
				continue;
			if (!callback(slot, p, userData))
				return;
		}
	}

	struct SlotRosterContext
	{
		uint32_t* outPlayerIndices;
		uint32_t maxCount;
		uint32_t count;
		bool activeOnly;    // false keeps defeated/resigned players in the roster
	};

	bool CollectSlotPlayer(Int slot, Player* player, void* userData)
	{
		SlotRosterContext* ctx = (SlotRosterContext*)userData;
		if (player->isPlayerObserver())
			return true;
		if (ctx->activeOnly && !player->isPlayerActive())
			return true;

		ctx->outPlayerIndices[ctx->count++] = (uint32_t)player->getPlayerIndex();
		return ctx->count < ctx->maxCount;
	}

	uint32_t HostAPI_GetActivePlayers(uint32_t* outPlayerIndices, uint32_t maxCount)
	{
		if (outPlayerIndices == nullptr || maxCount == 0)
			return 0;

		SlotRosterContext ctx = { outPlayerIndices, maxCount, 0, true };
		ForEachSlotPlayer(CollectSlotPlayer, &ctx);
		return ctx.count;
	}

	// Mirrors the same check the engine's own observer-only UI uses (e.g.
	// InGameUI's observer stats/notifications gating) - dead also counts as observing, since a dead
	// player becomes spectator-like. Plugins that display information about other players must gate
	// on this.
	uint8_t HostAPI_IsLocalPlayerObserver()
	{
		return GOPluginManager::IsLocalPlayerObserver() ? 1 : 0;
	}

	// General-power roster. A power counts as owned when the player has the required science and at
	// least one live object carries its module - the same test the engine's own observer UI uses.

	struct PowerModuleFind
	{
		const SpecialPowerTemplate* powerTemplate;
		SpecialPowerModuleInterface* module;
		Object* object; // the STRUCTURE carrying the module, for GOGeneralPowerInfo::buildingObjectId
	};

	// Prefers a structure as the anchor but accepts any carrier - see
	// plans\plugin-framework\design-notes.md.
	static void FindPowerModule(Object* obj, void* userData)
	{
		if (obj == nullptr)
			return;
		PowerModuleFind* find = (PowerModuleFind*)userData;
		// Step 1: a structure is the best possible anchor, so stop once one has been found.
		if (find->object != nullptr)
			return;

		SpecialPowerModuleInterface* module = obj->getSpecialPowerModule(find->powerTemplate);
		if (module == nullptr)
			return;

		// Step 2: a structure wins outright and also becomes the reported building.
		if (obj->isKindOf(KINDOF_STRUCTURE))
		{
			find->module = module;
			find->object = obj;
			return;
		}

		// Step 3: otherwise keep the first carrier of any kind, with no building to anchor to.
		if (find->module == nullptr)
			find->module = module;
	}

	uint32_t HostAPI_GetPlayerGeneralPowers(uint32_t playerIndex, GOGeneralPowerInfo* outPowers, uint32_t maxCount)
	{
		if (outPowers == nullptr || maxCount == 0)
			return 0;
		if (ThePlayerList == nullptr || TheControlBar == nullptr)
			return 0;

		Player* player = FindPlayerByIndex(playerIndex);
		if (player == nullptr)
			return 0;

		const PlayerTemplate* pt = player->getPlayerTemplate();
		if (pt == nullptr)
			return 0;
		AsciiString cmdSetName = pt->getSpecialPowerShortcutCommandSet();
		if (cmdSetName.isEmpty())
			return 0;

		const CommandSet* cmdSet = TheControlBar->findCommandSet(cmdSetName);
		if (cmdSet == nullptr)
			return 0;

		uint32_t count = 0;
		for (Int i = 0; i < MAX_COMMANDS_PER_SET && count < maxCount; ++i)
		{
			const CommandButton* btn = cmdSet->getCommandButton(i);
			if (btn == nullptr)
				continue;
			const SpecialPowerTemplate* sp = btn->getSpecialPowerTemplate();
			if (sp == nullptr)
				continue;

			ScienceType required = sp->getRequiredScience();
			if (required != SCIENCE_INVALID && !player->hasScience(required))
				continue;

			PowerModuleFind find;
			find.powerTemplate = sp;
			find.module = nullptr;
			find.object = nullptr;
			player->iterateObjects(FindPowerModule, &find);
			if (find.module == nullptr)
				continue;

			GOGeneralPowerInfo& info = outPowers[count];
			info.templateName = sp->getName().str();
			info.rechargeFrames = sp->getReloadTime();
			info.buildingObjectId = (find.object != nullptr) ? (uint32_t)find.object->getID() : 0;
			const UnsignedInt readyFrame = find.module->getReadyFrame();
			const UnsignedInt now = (TheGameLogic != nullptr) ? (UnsignedInt)TheGameLogic->getFrame() : 0;
			info.framesUntilReady = (readyFrame > now) ? (readyFrame - now) : 0;
			++count;
		}
		return count;
	}

	// ---- Icon drawing. Only valid to call from within a GORenderCallbacks::onDrawOverlay
	// callback - same 2D-context requirement as drawText2D/drawRect2D. ----

	void HostAPI_DrawTemplateIcon2D(const char* templateName, int32_t x, int32_t y, int32_t width, int32_t height)
	{
		if (templateName == nullptr || TheDisplay == nullptr)
			return;

		const Image* img = nullptr;
		if (TheThingFactory != nullptr)
		{
			const ThingTemplate* tmpl = TheThingFactory->findTemplate(AsciiString(templateName), FALSE);
			if (tmpl != nullptr)
				img = tmpl->getButtonImage();
		}
		if (img == nullptr && TheUpgradeCenter != nullptr)
		{
			const UpgradeTemplate* upgrade = TheUpgradeCenter->findUpgrade(templateName);
			if (upgrade != nullptr)
				img = upgrade->getButtonImage();
		}
		if (img != nullptr)
			TheDisplay->drawImage(img, x, y, x + width, y + height);
	}

	void HostAPI_DrawPowerIcon2D(uint32_t playerIndex, const char* powerTemplateName, int32_t x, int32_t y, int32_t width, int32_t height)
	{
		if (powerTemplateName == nullptr || TheDisplay == nullptr || TheControlBar == nullptr)
			return;

		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return;

		const PlayerTemplate* pt = p->getPlayerTemplate();
		if (pt == nullptr)
			return;

		AsciiString cmdSetName = pt->getSpecialPowerShortcutCommandSet();
		if (cmdSetName.isEmpty())
			return;

		const CommandSet* cmdSet = TheControlBar->findCommandSet(cmdSetName);
		if (cmdSet == nullptr)
			return;

		for (Int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
		{
			const CommandButton* btn = cmdSet->getCommandButton(i);
			if (btn == nullptr || btn->getSpecialPowerTemplate() == nullptr)
				continue;
			if (strcmp(btn->getSpecialPowerTemplate()->getName().str(), powerTemplateName) != 0)
				continue;

			const Image* img = btn->getButtonImage();
			if (img != nullptr)
				TheDisplay->drawImage(img, x, y, x + width, y + height);
			return;
		}
	}

	void HostAPI_DrawText2D(int32_t x, int32_t y, const char* asciiText, uint32_t colorARGB)
	{
		if (TheInGameUI != nullptr)
			TheInGameUI->drawPluginText2D((Int)x, (Int)y, asciiText, (Color)colorARGB);
	}

	void HostAPI_DrawText2DScaled(int32_t x, int32_t y, const char* asciiText, uint32_t colorARGB, float sizeScale, uint8_t bold)
	{
		if (TheInGameUI != nullptr)
			TheInGameUI->drawPluginText2DScaled((Int)x, (Int)y, asciiText, (Color)colorARGB, (Real)sizeScale, bold != 0);
	}

	void HostAPI_DrawRect2D(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t colorARGB, uint8_t filled)
	{
		if (TheDisplay == nullptr)
			return;

		if (filled != 0)
			TheDisplay->drawFillRect((Int)x, (Int)y, (Int)width, (Int)height, (Color)colorARGB);
		else
			TheDisplay->drawOpenRect((Int)x, (Int)y, (Int)width, (Int)height, 1.0f, (Color)colorARGB);
	}

	// One rotated quad however long the line is, unlike DrawRect2D which is axis-aligned.
	void HostAPI_DrawLine2D(int32_t x1, int32_t y1, int32_t x2, int32_t y2, float thickness, uint32_t colorARGB)
	{
		if (TheDisplay == nullptr)
			return;

		TheDisplay->drawLine((Int)x1, (Int)y1, (Int)x2, (Int)y2, (Real)thickness, (Color)colorARGB);
	}

	void HostAPI_GetScreenSize(int32_t* outWidth, int32_t* outHeight)
	{
		UnsignedInt w = (TheDisplay != nullptr) ? TheDisplay->getWidth() : 0;
		UnsignedInt h = (TheDisplay != nullptr) ? TheDisplay->getHeight() : 0;
		if (outWidth != nullptr)
			*outWidth = (int32_t)w;
		if (outHeight != nullptr)
			*outHeight = (int32_t)h;
	}

	// ---- Camera and view state. ----

	uint8_t HostAPI_GetCameraState(GOCameraState* out)
	{
		if (out == nullptr)
			return 0;
		memset(out, 0, sizeof(*out));
		if (TheTacticalView == nullptr)
			return 0;

		const Coord3D eye = TheTacticalView->get3DCameraPosition();
		const Coord3D dir = TheTacticalView->get3DCameraDirection();
		const float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);

		out->pos[0] = eye.x;
		out->pos[1] = eye.y;
		out->pos[2] = eye.z;

		// The engine's own look-at point, carried instead of trusting the reported pitch: the FX pitch
		// moves the target's Z, so pitch alone does not describe the view.
		const Coord3D look = TheTacticalView->getPosition();
		out->target[0] = look.x;
		out->target[1] = look.y;
		out->target[2] = look.z;

		out->yawRadians = atan2f(dir.y, dir.x);
		out->pitchRadians = (len > 0.0f) ? asinf(-dir.z / len) : 0.0f;

		// View::getFieldOfView is the horizontal FOV, which is the one Generals uses (View.h:246).
		// get3DCamera() lives on W3DView, not on View, and this manager only ever sees the base class.
		// The engine keeps it in radians while the ABI field promises degrees - convert here, or every
		// consumer that trusts the name is off by a factor of 57.
		out->fovHorizontalDegrees = (float)(TheTacticalView->getFieldOfView() * 180.0 / 3.14159265358979323846);

		if (TheDisplay != nullptr)
		{
			out->viewportWidth = (int32_t)TheDisplay->getWidth();
			out->viewportHeight = (int32_t)TheDisplay->getHeight();
		}

		if (TheGameLogic != nullptr)
		{
			out->logicFrame = (uint32_t)TheGameLogic->getFrame();
			out->paused = TheGameLogic->isGamePaused() ? 1 : 0;
		}

		out->valid = 1;
		return 1;
	}

	// ---- Map identity and live objects, for a capture that has to name and place what it saw. ----

	int32_t HostAPI_GetLocalPlayerIndex()
	{
		if (ThePlayerList == nullptr)
			return -1;
		Player* localPlayer = ThePlayerList->getLocalPlayer();
		if (localPlayer == nullptr)
			return -1;
		return (int32_t)localPlayer->getPlayerIndex();
	}

	uint8_t HostAPI_GetMapInfo(GOMapInfo* out)
	{
		if (out == nullptr)
			return 0;
		memset(out, 0, sizeof(*out));
		if (TheGlobalData == nullptr || TheTerrainLogic == nullptr)
			return 0;

		// The engine keeps the map's full path (Maps\<folder>\<file>.map); the stem is what a
		// human recognises and what the filename slug is built from.
		const char* full = TheGlobalData->m_mapName.str();
		const char* leaf = full;
		for (const char* p = full; p != nullptr && *p != '\0'; ++p)
		{
			if (*p == '\\' || *p == '/')
				leaf = p + 1;
		}
		size_t length = strlen(leaf);
		if (length > 4 && (leaf[length - 4] == '.'))
			length -= 4;
		if (length > 0)
		{
			if (length >= sizeof(out->name))
				length = sizeof(out->name) - 1;
			memcpy(out->name, leaf, length);
			out->name[length] = '\0';
		}

		// W3DTerrainLogic overrides this virtual; the base implementation is a stub, so an extent
		// of zero means "this build cannot answer" rather than "an empty map".
		Region3D extent;
		TheTerrainLogic->getExtent(&extent);
		const Real width = extent.hi.x - extent.lo.x;
		const Real height = extent.hi.y - extent.lo.y;
		if (width > 0.0f && height > 0.0f)
		{
			out->extent[0] = (int32_t)width;
			out->extent[1] = (int32_t)height;
		}

		out->valid = 1;
		return 1;
	}

	// Walks the engine's world object list, not a player roster, so neutral and civilian owners
	// are included - the object list is what destroy-all-objects iterates, not enumeratePlayerObjects.
	int32_t HostAPI_GetMapObjects(GOObjectInfo* outObjects, int32_t maxCount, int32_t kindMask, int32_t* outTotal)
	{
		if (outTotal != nullptr)
			*outTotal = 0;
		if (TheGameLogic == nullptr)
			return 0;

		int32_t written = 0;
		int32_t total = 0;
		for (Object* obj = TheGameLogic->getFirstObject(); obj != nullptr; obj = obj->getNextObject())
		{
			if (obj->isEffectivelyDead())
				continue;

			const ThingTemplate* thingTemplate = obj->getTemplate();
			if (thingTemplate == nullptr)
				continue;

			const bool isStructure = obj->isKindOf(KINDOF_STRUCTURE) ? true : false;
			const bool isUnit = (!isStructure && (obj->isKindOf(KINDOF_VEHICLE) || obj->isKindOf(KINDOF_INFANTRY))) ? true : false;
			const int32_t kind = isStructure ? GO_OBJECT_KIND_STRUCTURE : (isUnit ? GO_OBJECT_KIND_UNIT : GO_OBJECT_KIND_NONE);
			if (kind == GO_OBJECT_KIND_NONE || (kindMask & kind) == 0)
				continue;

			++total;
			if (outObjects == nullptr || written >= maxCount)
				continue;

			GOObjectInfo& info = outObjects[written];
			memset(&info, 0, sizeof(info));
			info.id = (int32_t)obj->getID();
			snprintf(info.template_name, sizeof(info.template_name), "%s", thingTemplate->getName().str());

			Player* owner = obj->getControllingPlayer();
			info.owner_index = (owner != nullptr) ? (int32_t)owner->getPlayerIndex() : -1;

			const Coord3D* pos = obj->getPosition();
			if (pos != nullptr)
			{
				info.pos[0] = pos->x;
				info.pos[1] = pos->y;
				info.pos[2] = pos->z;

				// A structure is projected at its base, which is where it actually stands; a unit at
				// its own position. The research measured ~51 world units of error when a roof point
				// is used instead at 38 degrees, so this is deliberate.
				if (TheTacticalView != nullptr)
				{
					ICoord2D screen;
					if (TheTacticalView->worldToScreen(pos, &screen))
					{
						const int viewWidth = (TheDisplay != nullptr) ? (int)TheDisplay->getWidth() : 0;
						const int viewHeight = (TheDisplay != nullptr) ? (int)TheDisplay->getHeight() : 0;
						const bool insideViewport = (TheDisplay == nullptr)
							|| (screen.x >= 0 && screen.y >= 0
								&& (int)screen.x < viewWidth && (int)screen.y < viewHeight);
						if (insideViewport)
						{
							info.screen[0] = (int32_t)screen.x;
							info.screen[1] = (int32_t)screen.y;
							info.on_screen = 1;
						}
					}
				}
			}

			info.radius = obj->getGeometryInfo().getBoundingCircleRadius();
			info.is_structure = isStructure ? 1 : 0;
			info.complete = isStructure
				? ((obj->getConstructionPercent() == (Real)CONSTRUCTION_COMPLETE) ? 1 : 0)
				: 1;
			++written;
		}

		if (outTotal != nullptr)
			*outTotal = total;
		return written;
	}


	uint32_t HostAPI_GetLogicFrame()
	{
		return (TheGameLogic != nullptr) ? (uint32_t)TheGameLogic->getFrame() : 0;
	}

	uint32_t HostAPI_GetLogicFramesPerSecond()
	{
		return (uint32_t)LOGICFRAMES_PER_SECOND;
	}

	// ---- Live production progress. Reads the producer's own queue, so the value is whatever the
	// simulation currently believes rather than a plugin-side reconstruction. ----

	ProductionUpdateInterface* FindProducer(uint32_t producerObjectId)
	{
		if (TheGameLogic == nullptr || producerObjectId == 0)
			return nullptr;
		Object* producer = TheGameLogic->findObjectByID((ObjectID)producerObjectId);
		return (producer != nullptr) ? producer->getProductionUpdateInterface() : nullptr;
	}

	float HostAPI_GetUnitProductionProgress(uint32_t producerObjectId, int32_t productionID)
	{
		ProductionUpdateInterface* production = FindProducer(producerObjectId);
		if (production == nullptr)
			return -1.0f;

		for (const ProductionEntry* entry = production->firstProduction(); entry != nullptr; entry = production->nextProduction(entry))
		{
			if (entry->getProductionType() == PRODUCTION_UNIT && (int32_t)entry->getProductionID() == productionID)
				return (float)entry->getPercentComplete();
		}
		return -1.0f;
	}

	float HostAPI_GetUpgradeProductionProgress(uint32_t producerObjectId, const char* upgradeTemplateName)
	{
		if (upgradeTemplateName == nullptr)
			return -1.0f;

		ProductionUpdateInterface* production = FindProducer(producerObjectId);
		if (production == nullptr)
			return -1.0f;

		for (const ProductionEntry* entry = production->firstProduction(); entry != nullptr; entry = production->nextProduction(entry))
		{
			if (entry->getProductionType() != PRODUCTION_UPGRADE)
				continue;
			const UpgradeTemplate* upgrade = entry->getProductionUpgrade();
			if (upgrade != nullptr && strcmp(upgrade->getUpgradeName().str(), upgradeTemplateName) == 0)
				return (float)entry->getPercentComplete();
		}
		return -1.0f;
	}

	// ---- World-space anchoring. ----

	uint8_t HostAPI_WorldToScreen(float worldX, float worldY, float worldZ, int32_t* outX, int32_t* outY)
	{
		if (TheTacticalView == nullptr)
			return 0;

		Coord3D world;
		world.x = worldX;
		world.y = worldY;
		world.z = worldZ;

		ICoord2D screen;
		if (!TheTacticalView->worldToScreen(&world, &screen))
			return 0;

		if (outX != nullptr)
			*outX = (int32_t)screen.x;
		if (outY != nullptr)
			*outY = (int32_t)screen.y;
		return 1;
	}

	// Edge inset for clamped off-screen indicators, as a fraction of view height so it scales with
	// resolution (0.0333 is the original 24 pixels at 720p).
	const float kEdgeIndicatorMarginFraction = 0.0333f;

	// Like HostAPI_WorldToScreen, but clamps an off-screen point to the view edge instead of
	// dropping it - see plans\plugin-framework\design-notes.md.
	uint8_t HostAPI_WorldToScreenClamped(float worldX, float worldY, float worldZ, int32_t* outX, int32_t* outY)
	{
		if (TheTacticalView == nullptr)
			return 0;

		Coord3D world;
		world.x = worldX;
		world.y = worldY;
		world.z = worldZ;

		// Step 1: project, accepting a point beyond the far clip plane as well as an off-frustum one.
		ICoord2D screen;
		const View::WorldToScreenReturn result = TheTacticalView->worldToScreenTriReturnAllowFarClip(&world, &screen);
		if (result == View::WTS_INVALID)
			return 0;

		if (result == View::WTS_OUTSIDE_FRUSTUM)
		{
			// Step 2: clamp against the tactical view's own rectangle, which is not always the
			// whole display - the projected position is already in full-display coordinates.
			Int originX = 0;
			Int originY = 0;
			TheTacticalView->getOrigin(&originX, &originY);
			const float viewW = (float)TheTacticalView->getWidth();
			const float viewH = (float)TheTacticalView->getHeight();
			const float margin = viewH * kEdgeIndicatorMarginFraction;
			if (viewW > margin * 2.0f && viewH > margin * 2.0f)
			{
				// Step 3: clamp along the ray from the view's centre to the raw (possibly far
				// off-screen) projected point, so the indicator sits on the nearest edge.
				const float cx = (float)originX + viewW * 0.5f;
				const float cy = (float)originY + viewH * 0.5f;
				float dx = (float)screen.x - cx;
				float dy = (float)screen.y - cy;
				const float halfW = viewW * 0.5f - margin;
				const float halfH = viewH * 0.5f - margin;
				const float absDx = (dx < 0.0f) ? -dx : dx;
				const float absDy = (dy < 0.0f) ? -dy : dy;
				float scale = 1.0f;
				if (absDx > halfW && absDx > 0.0f)
					scale = halfW / absDx;
				if (absDy > halfH && absDy > 0.0f)
				{
					const float scaleY = halfH / absDy;
					if (scaleY < scale)
						scale = scaleY;
				}
				if (scale < 1.0f)
				{
					dx *= scale;
					dy *= scale;
				}
				screen.x = (Int)(cx + dx);
				screen.y = (Int)(cy + dy);
			}
		}

		if (outX != nullptr)
			*outX = (int32_t)screen.x;
		if (outY != nullptr)
			*outY = (int32_t)screen.y;
		return 1;
	}

	// Classifies by the same KindOf flags the engine's own faction-agnostic code uses, so no
	// per-faction cases are needed - see plans\plugin-framework\design-notes.md.
	uint8_t HostAPI_GetObjectBuildingCategory(uint32_t objectId)
	{
		if (TheGameLogic == nullptr || objectId == 0)
			return GO_BUILDING_CATEGORY_NONE;

		Object* obj = TheGameLogic->findObjectByID((ObjectID)objectId);
		if (obj == nullptr || !obj->isKindOf(KINDOF_STRUCTURE))
			return GO_BUILDING_CATEGORY_NONE;

		if (obj->isKindOf(KINDOF_COMMANDCENTER))
			return GO_BUILDING_CATEGORY_COMMAND_CENTER;
		if (obj->isKindOf(KINDOF_FS_WARFACTORY))
			return GO_BUILDING_CATEGORY_WAR_FACTORY;
		if (obj->isKindOf(KINDOF_FS_BARRACKS))
			return GO_BUILDING_CATEGORY_BARRACKS;
		if (obj->isKindOf(KINDOF_FS_AIRFIELD))
			return GO_BUILDING_CATEGORY_AIRFIELD;
		if (obj->isKindOf(KINDOF_FS_SUPPLY_CENTER))
			return GO_BUILDING_CATEGORY_SUPPLY_STASH;
		return GO_BUILDING_CATEGORY_NONE;
	}

	const char* HostAPI_GetObjectTemplateName(uint32_t objectId)
	{
		// One static buffer, matching the other string accessors: the caller copies it out.
		static AsciiString s_objectTemplateName;
		s_objectTemplateName.clear();
		if (TheGameLogic == nullptr || objectId == 0)
			return s_objectTemplateName.str();

		Object* obj = TheGameLogic->findObjectByID((ObjectID)objectId);
		if (obj == nullptr || obj->getTemplate() == nullptr)
			return s_objectTemplateName.str();

		s_objectTemplateName = obj->getTemplate()->getName();
		return s_objectTemplateName.str();
	}

	uint8_t HostAPI_GetObjectScreenBounds(uint32_t objectId, int32_t* outX, int32_t* outY, int32_t* outWidth, int32_t* outHeight)
	{
		if (TheGameLogic == nullptr || TheTacticalView == nullptr || objectId == 0)
			return 0;

		Object* obj = TheGameLogic->findObjectByID((ObjectID)objectId);
		if (obj == nullptr)
			return 0;

		const Coord3D* pos = obj->getPosition();
		if (pos == nullptr)
			return 0;

		ICoord2D centre;
		if (!TheTacticalView->worldToScreen(pos, &centre))
			return 0;

		// Project a point one bounding radius to the side of the object as well; the horizontal
		// distance between the two projections is the object's on-screen size, which already
		// accounts for camera zoom and pitch without exposing any camera state to the plugin.
		const Real radius = obj->getGeometryInfo().getBoundingCircleRadius();
		Coord3D edgeWorld = *pos;
		edgeWorld.x += radius;

		ICoord2D edge;
		Int halfWidth = 0;
		if (TheTacticalView->worldToScreen(&edgeWorld, &edge))
			halfWidth = abs(edge.x - centre.x);
		if (halfWidth <= 0)
			halfWidth = 1;

		if (outX != nullptr)
			*outX = (int32_t)(centre.x - halfWidth);
		if (outY != nullptr)
			*outY = (int32_t)(centre.y - halfWidth);
		if (outWidth != nullptr)
			*outWidth = (int32_t)(halfWidth * 2);
		if (outHeight != nullptr)
			*outHeight = (int32_t)(halfWidth * 2);
		return 1;
	}

	// Projects the same point the engine draws its own health bar at - see plans\plugin-framework\design-notes.md.
	uint8_t HostAPI_GetObjectHealthBarScreenPosition(uint32_t objectId, int32_t* outX, int32_t* outY)
	{
		if (TheGameLogic == nullptr || TheTacticalView == nullptr || objectId == 0)
			return 0;

		Object* obj = TheGameLogic->findObjectByID((ObjectID)objectId);
		if (obj == nullptr)
			return 0;

		Coord3D pos;
		obj->getHealthBoxPosition(pos);

		ICoord2D screen;
		if (!TheTacticalView->worldToScreen(&pos, &screen))
			return 0;

		if (outX != nullptr)
			*outX = (int32_t)screen.x;
		if (outY != nullptr)
			*outY = (int32_t)screen.y;
		return 1;
	}

	// Camera control. Same look-at path the engine's own observer actions
	// use, so a plugin can jump the viewport to where a gameplay event happened when the user
	// clicks on it.

	void HostAPI_TeleportViewportTo(float worldX, float worldY, float worldZ)
	{
		if (TheTacticalView == nullptr)
			return;
		Coord3D target;
		target.x = worldX;
		target.y = worldY;
		target.z = worldZ;
		TheTacticalView->userLookAt(&target);
	}

	// ---- Clock-wedge draw primitives. Same 2D-context requirement as drawText2D/drawRect2D. ----

	void HostAPI_DrawRectClock2D(int32_t x, int32_t y, int32_t width, int32_t height, int32_t percent, uint32_t colorARGB)
	{
		if (TheDisplay != nullptr)
			TheDisplay->drawRectClock((Int)x, (Int)y, (Int)width, (Int)height, (Int)percent, (UnsignedInt)colorARGB);
	}

	void HostAPI_DrawRemainingRectClock2D(int32_t x, int32_t y, int32_t width, int32_t height, int32_t percent, uint32_t colorARGB)
	{
		if (TheDisplay != nullptr)
			TheDisplay->drawRemainingRectClock((Int)x, (Int)y, (Int)width, (Int)height, (Int)percent, (UnsignedInt)colorARGB);
	}

	// Both handles come from the device layer through GOPluginManager's provider seam.

	void* HostAPI_GetD3DDevice8()
	{
		return GOPluginManager::GetD3DDevice8();
	}

	void* HostAPI_GetGameWindow()
	{
		return GOPluginManager::GetGameWindow();
	}

	struct EnumerateObjectsContext
	{
		void (*callback)(uint32_t, float, float, float, void*);
		void* userData;
		uint32_t count;
	};

	void EnumerateObjectsCallback(Object* object, void* userData)
	{
		EnumerateObjectsContext* context = (EnumerateObjectsContext*)userData;
		if (object == nullptr || context->callback == nullptr)
			return;
		const Coord3D* position = object->getPosition();
		if (position == nullptr)
			return;
		context->callback((uint32_t)object->getID(), position->x, position->y, position->z, context->userData);
		++context->count;
	}

	uint32_t HostAPI_EnumeratePlayerObjects(uint32_t playerIndex,
		void (*callback)(uint32_t, float, float, float, void*), void* userData)
	{
		Player* player = FindPlayerByIndex(playerIndex);
		if (player == nullptr || callback == nullptr)
			return 0;
		EnumerateObjectsContext context = { callback, userData, 0 };
		player->iterateObjects(EnumerateObjectsCallback, &context);
		return context.count;
	}

	uint32_t HostAPI_GetContainedObjects(uint32_t containerObjectId,
		GOContainedObjectInfo* outObjects, uint32_t maxCount)
	{
		if (TheGameLogic == nullptr || containerObjectId == 0 || outObjects == nullptr || maxCount == 0)
			return 0;
		Object* container = TheGameLogic->findObjectByID((ObjectID)containerObjectId);
		if (container == nullptr || container->getContain() == nullptr)
			return 0;
		const ContainedItemsList* items = container->getContain()->getContainedItemsList();
		if (items == nullptr)
			return 0;
		uint32_t count = 0;
		for (ContainedItemsList::const_iterator it = items->begin(); it != items->end() && count < maxCount; ++it)
		{
			Object* object = *it;
			if (object == nullptr || object->getTemplate() == nullptr)
				continue;
			GOContainedObjectInfo& info = outObjects[count++];
			info.objectId = (uint32_t)object->getID();
			info.templateName = object->getTemplate()->getName().str();
			info.playerIndex = object->getControllingPlayer() != nullptr
				? (uint32_t)object->getControllingPlayer()->getPlayerIndex() : 0;
		}
		return count;
	}

	uint8_t HostAPI_GetObjectTargetPosition(uint32_t objectId, float* outX, float* outY, float* outZ)
	{
		if (TheGameLogic == nullptr || objectId == 0)
			return 0;

		Object* obj = TheGameLogic->findObjectByID((ObjectID)objectId);
		if (obj == nullptr)
			return 0;

		AIUpdateInterface* ai = obj->getAIUpdateInterface();
		if (ai == nullptr)
			return 0;

		// Attacking a specific object: point at that object's current position.
		Object* victim = ai->getCurrentVictim();
		if (victim != nullptr)
		{
			const Coord3D* pos = victim->getPosition();
			if (pos != nullptr)
			{
				if (outX != nullptr) *outX = pos->x;
				if (outY != nullptr) *outY = pos->y;
				if (outZ != nullptr) *outZ = pos->z;
				return 1;
			}
		}

		// A goal position only counts for an explicit attack order, never a move-to or guard - see
		// plans\plugin-framework\design-notes.md.
		const StateID state = ai->getCurrentStateID();
		if (state != AI_ATTACK_MOVE_TO && state != AI_ATTACK_POSITION)
			return 0;

		const Coord3D* goal = ai->getGoalPosition();
		if (goal == nullptr)
			return 0;

		if (outX != nullptr) *outX = goal->x;
		if (outY != nullptr) *outY = goal->y;
		if (outZ != nullptr) *outZ = goal->z;
		return 1;
	}

	uint8_t HostAPI_IsObjectAirborne(uint32_t objectId)
	{
		if (TheGameLogic == nullptr || objectId == 0)
			return 0;
		Object* obj = TheGameLogic->findObjectByID((ObjectID)objectId);
		return (obj != nullptr && obj->isAirborneTarget()) ? 1 : 0;
	}

	uint8_t HostAPI_IsObjectVehicle(uint32_t objectId)
	{
		if (TheGameLogic == nullptr || objectId == 0)
			return 0;
		Object* obj = TheGameLogic->findObjectByID((ObjectID)objectId);
		return (obj != nullptr && obj->isKindOf(KINDOF_VEHICLE)) ? 1 : 0;
	}

	uint8_t HostAPI_IsObjectProjectile(uint32_t objectId)
	{
		if (TheGameLogic == nullptr || objectId == 0)
			return 0;
		Object* obj = TheGameLogic->findObjectByID((ObjectID)objectId);
		return (obj != nullptr && obj->isKindOf(KINDOF_PROJECTILE)) ? 1 : 0;
	}

	uint32_t HostAPI_GetObjectContainerId(uint32_t objectId)
	{
		if (TheGameLogic == nullptr || objectId == 0)
			return 0;
		Object* obj = TheGameLogic->findObjectByID((ObjectID)objectId);
		if (obj == nullptr)
			return 0;
		const Object* container = obj->getContainedBy();
		return (container != nullptr) ? (uint32_t)container->getID() : 0;
	}

	// ---- Player card queries (Plan 5). Same FindPlayerByIndex resolution as every other
	// per-player query above. ----

	// Each of the string-returning queries below owns one distinct function-local buffer, so only
	// its own most recent return value is live - see plans\plugin-framework\design-notes.md.
	const char* HostAPI_GetPlayerName(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return "";
		// getPlayerDisplayName() is a UnicodeString; the ABI only carries UTF-8/ASCII.
		static AsciiString s_playerName;
		s_playerName.translate(p->getPlayerDisplayName());
		return s_playerName.str();
	}

	const char* HostAPI_GetPlayerFactionTemplate(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return "";
		// getSide() is the template's "Side" INI field ("AmericaLaserGeneral"), not
		// getPlayerTemplate()->getName(), which carries an unreadable "Faction" prefix.
		return p->getSide().str();
	}

	uint32_t HostAPI_GetPlayerMoney(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		return (p != nullptr) ? (uint32_t)p->getMoney()->countMoney() : 0;
	}

	// Both skill-point figures are absolute totals, so they are already a progress bar's numerator
	// and denominator - see plans\plugin-framework\design-notes.md.
	int32_t HostAPI_GetPlayerRank(uint32_t playerIndex, uint32_t* outCurrentXP, uint32_t* outNextXP)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return -1;
		const Int rankLevel = p->getRankLevel();
		if (rankLevel <= 0)
			return -1;
		if (outCurrentXP != nullptr)
			*outCurrentXP = (uint32_t)p->getSkillPoints();
		if (outNextXP != nullptr)
		{
			// At the rank cap there is no next rank, and the threshold is INT_MAX - report 0
			// rather than a meaningless huge denominator.
			const Int levelUp = p->getSkillPointsLevelUp();
			*outNextXP = (levelUp >= INT_MAX) ? 0 : (uint32_t)levelUp;
		}
		return (int32_t)(rankLevel - 1);
	}

	// getEnergy(), like getMoney() and getScoreKeeper(), returns the address of a Player member and
	// is never null once the Player itself resolved, so none of the three is null-checked.
	uint8_t HostAPI_GetPlayerPowerState(uint32_t playerIndex, uint32_t* outPowerGenerated, uint32_t* outPowerDrain)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return 0;
		const Energy* energy = p->getEnergy();
		if (outPowerGenerated != nullptr)
			*outPowerGenerated = (uint32_t)energy->getProduction();
		if (outPowerDrain != nullptr)
			*outPowerDrain = (uint32_t)energy->getConsumption();
		return 1;
	}

	struct CountContext { uint32_t count; };

	// A builder is anything carrying a DozerAIInterface, which covers all three factions - see
	// plans\plugin-framework\design-notes.md.
	void CountBuilderCallback(Object* obj, void* userData)
	{
		if (obj == nullptr)
			return;
		AIUpdateInterface* ai = obj->getAIUpdateInterface();
		if (ai != nullptr && ai->getDozerAIInterface() != nullptr)
			((CountContext*)userData)->count++;
	}

	uint32_t HostAPI_GetPlayerBuilderCount(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return 0;
		CountContext ctx = { 0 };
		p->iterateObjects(CountBuilderCallback, &ctx);
		return ctx.count;
	}

	// Counts units actually working the supply line right now, not every supply unit - see
	// plans\plugin-framework\design-notes.md.
	void CountActiveGathererCallback(Object* obj, void* userData)
	{
		if (obj == nullptr)
			return;
		AIUpdateInterface* ai = obj->getAIUpdateInterface();
		if (ai == nullptr)
			return;
		SupplyTruckAIInterface* supplyAI = ai->getSupplyTruckAIInterface();
		if (supplyAI != nullptr && supplyAI->isCurrentlyFerryingSupplies())
			((CountContext*)userData)->count++;
	}

	uint32_t HostAPI_GetPlayerActiveGathererCount(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return 0;
		CountContext ctx = { 0 };
		p->iterateObjects(CountActiveGathererCallback, &ctx);
		return ctx.count;
	}

	// The score screen's cumulative figure, deliberately raw rather than a rate - see plans\plugin-framework\design-notes.md.
	uint32_t HostAPI_GetPlayerTotalMoneyEarned(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return 0;
		return (uint32_t)p->getScoreKeeper()->getTotalMoneyEarned();
	}

	// Looks up by raw template name rather than a live object, so it also resolves a template a
	// plugin only knows the name of (e.g. GOContainedObjectInfo::templateName from a garrison query).
	const char* HostAPI_GetTemplateDisplayName(const char* templateName)
	{
		if (templateName == nullptr || templateName[0] == '\0' || TheThingFactory == nullptr)
			return "";
		const ThingTemplate* tt = TheThingFactory->findTemplate(AsciiString(templateName));
		if (tt == nullptr)
			return "";
		static AsciiString s_templateDisplayName;
		s_templateDisplayName.translate(tt->getDisplayName());
		return s_templateDisplayName.str();
	}

	struct SampleTemplateContext { AsciiString templateName; bool found; };

	// Same qualifying check as CountBuilderCallback, but stops recording once one match is found -
	// this only needs a representative icon, not an exact count.
	void SampleBuilderTemplateCallback(Object* obj, void* userData)
	{
		SampleTemplateContext* ctx = (SampleTemplateContext*)userData;
		if (ctx->found || obj == nullptr)
			return;
		AIUpdateInterface* ai = obj->getAIUpdateInterface();
		if (ai != nullptr && ai->getDozerAIInterface() != nullptr && obj->getTemplate() != nullptr)
		{
			ctx->templateName = obj->getTemplate()->getName();
			ctx->found = true;
		}
	}

	// Same qualifying check as CountActiveGathererCallback (isCurrentlyFerryingSupplies, not merely
	// "is a supply unit"), so the sampled icon matches what getPlayerActiveGathererCount is counting.
	void SampleGathererTemplateCallback(Object* obj, void* userData)
	{
		SampleTemplateContext* ctx = (SampleTemplateContext*)userData;
		if (ctx->found || obj == nullptr)
			return;
		AIUpdateInterface* ai = obj->getAIUpdateInterface();
		if (ai == nullptr)
			return;
		SupplyTruckAIInterface* supplyAI = ai->getSupplyTruckAIInterface();
		if (supplyAI != nullptr && supplyAI->isCurrentlyFerryingSupplies() && obj->getTemplate() != nullptr)
		{
			ctx->templateName = obj->getTemplate()->getName();
			ctx->found = true;
		}
	}

	const char* HostAPI_GetPlayerBuilderTemplateName(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return "";
		SampleTemplateContext ctx = {};
		p->iterateObjects(SampleBuilderTemplateCallback, &ctx);
		static AsciiString s_builderTemplateName;
		s_builderTemplateName = ctx.templateName;
		return s_builderTemplateName.str();
	}

	// The base game has three distinct DozerAIInterface-carrying templates (USA Dozer, China Dozer,
	// GLA Worker); the extra headroom is for mods that add their own - see
	// plans\plugin-framework\design-notes.md.
	const uint32_t kMaxBuilderTypeBuckets = 8;

	struct BuilderTypeBucket { AsciiString name; uint32_t count; };
	struct BuilderTypeCountContext
	{
		BuilderTypeBucket buckets[kMaxBuilderTypeBuckets];
		uint32_t bucketCount = 0;
	};

	// As CountBuilderCallback, but keyed by template so a captured Worker is not collapsed into a
	// native Dozer - see plans\plugin-framework\design-notes.md.
	void CountBuilderTypeCallback(Object* obj, void* userData)
	{
		if (obj == nullptr)
			return;
		AIUpdateInterface* ai = obj->getAIUpdateInterface();
		if (ai == nullptr || ai->getDozerAIInterface() == nullptr)
			return;
		const ThingTemplate* tt = obj->getTemplate();
		if (tt == nullptr)
			return;

		BuilderTypeCountContext* ctx = (BuilderTypeCountContext*)userData;
		const AsciiString& name = tt->getName();
		for (uint32_t i = 0; i < ctx->bucketCount; ++i)
		{
			if (ctx->buckets[i].name == name)
			{
				ctx->buckets[i].count++;
				return;
			}
		}
		if (ctx->bucketCount < kMaxBuilderTypeBuckets)
		{
			ctx->buckets[ctx->bucketCount].name = name;
			ctx->buckets[ctx->bucketCount].count = 1;
			ctx->bucketCount++;
		}
	}

	// Per-type breakdown of the player's live builder units - see PluginABI.h for the contract.
	uint32_t HostAPI_GetPlayerBuilderTemplateCounts(uint32_t playerIndex, const char** outNames, uint32_t* outCounts, uint32_t maxCount)
	{
		if (outNames == nullptr || outCounts == nullptr || maxCount == 0)
			return 0;
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return 0;

		BuilderTypeCountContext ctx;
		p->iterateObjects(CountBuilderTypeCallback, &ctx);

		// This function's own backing storage for the returned pointers.
		static AsciiString s_builderTemplateNames[kMaxBuilderTypeBuckets];
		const uint32_t n = (ctx.bucketCount < maxCount) ? ctx.bucketCount : maxCount;
		for (uint32_t i = 0; i < n; ++i)
		{
			s_builderTemplateNames[i] = ctx.buckets[i].name;
			outNames[i] = s_builderTemplateNames[i].str();
			outCounts[i] = ctx.buckets[i].count;
		}
		return n;
	}

	const char* HostAPI_GetPlayerGathererTemplateName(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		if (p == nullptr)
			return "";
		SampleTemplateContext ctx = {};
		p->iterateObjects(SampleGathererTemplateCallback, &ctx);
		static AsciiString s_gathererTemplateName;
		s_gathererTemplateName = ctx.templateName;
		return s_gathererTemplateName.str();
	}

	struct SlotLookupContext { uint32_t playerIndex; Int slot; };

	bool MatchSlotByPlayerIndex(Int slot, Player* player, void* userData)
	{
		SlotLookupContext* ctx = (SlotLookupContext*)userData;
		if ((uint32_t)player->getPlayerIndex() != ctx->playerIndex)
			return true;
		ctx->slot = slot;
		return false;
	}

	// playerIndex (Player::getPlayerIndex()) and the lobby slot index GameSlot is keyed by are two
	// different numbering spaces, so the slot has to be found first and then looked up in TheGameInfo.
	int32_t HostAPI_GetPlayerTeamNumber(uint32_t playerIndex)
	{
		if (TheGameInfo == nullptr)
			return -1;

		SlotLookupContext ctx = { playerIndex, -1 };
		ForEachSlotPlayer(MatchSlotByPlayerIndex, &ctx);
		if (ctx.slot < 0)
			return -1;

		const GameSlot* gameSlot = TheGameInfo->getConstSlot(ctx.slot);
		return (gameSlot != nullptr) ? gameSlot->getTeamNumber() : -1;
	}

	// As HostAPI_GetActivePlayers but without the isPlayerActive() filter, so the roster is stable
	// for the whole match - see plans\plugin-framework\design-notes.md.
	uint32_t HostAPI_GetMatchPlayers(uint32_t* outPlayerIndices, uint32_t maxCount)
	{
		if (outPlayerIndices == nullptr || maxCount == 0)
			return 0;

		SlotRosterContext ctx = { outPlayerIndices, maxCount, 0, false };
		ForEachSlotPlayer(CollectSlotPlayer, &ctx);
		return ctx.count;
	}

	uint8_t HostAPI_GetPlayerIsDefeated(uint32_t playerIndex)
	{
		Player* p = FindPlayerByIndex(playerIndex);
		return (p != nullptr && !p->isPlayerActive()) ? 1 : 0;
	}

	const char* HostAPI_GetUserDataPath()
	{
		// Cached in a function-local static because the ABI promises a pointer that stays valid for
		// the process lifetime, while getPath_UserData() hands back a temporary.
		static std::string s_userDataPath;
		if (s_userDataPath.empty() && TheGlobalData != nullptr)
			s_userDataPath = TheGlobalData->getPath_UserData().str();
		return s_userDataPath.c_str();
	}

	// --------------------------------------------------------------------------------------------
	// Capture: one PNG of the presented frame plus a JSON sidecar, written at the end of the frame
	// the plugin asked on so the pixels and the coordinates cannot disagree.
	// --------------------------------------------------------------------------------------------

	struct PendingCaptureRequest
	{
		bool pending = false;
		std::string name;
		std::string trigger;
		GOCameraState camera = {};
		bool hasCamera = false;
		std::vector<GOObjectInfo> objects;
	};

	PendingCaptureRequest s_pendingCapture;
	GOPluginManager::ScreenshotWriter s_screenshotWriter = nullptr;

	std::string FormatString(const char* fmt, ...)
	{
		char buffer[1024];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buffer, sizeof(buffer), fmt, args);
		va_end(args);
		return std::string(buffer);
	}

	std::string ToLowerAscii(std::string text)
	{
		for (size_t i = 0; i < text.size(); ++i)
		{
			const unsigned char ch = (unsigned char)text[i];
			if (ch >= 'A' && ch <= 'Z')
				text[i] = (char)(ch - 'A' + 'a');
		}
		return text;
	}

	// Lowercased [a-z0-9-], runs collapsed to one dash, at most 40 characters - the spec's filename
	// rule. Anything else in the map name (spaces, parentheses, non-ASCII) becomes a dash.
	std::string Slugify(const std::string& text)
	{
		std::string slug;
		bool lastWasDash = false;
		const std::string lower = ToLowerAscii(text);
		for (size_t i = 0; i < lower.size() && slug.size() < 40; ++i)
		{
			const unsigned char ch = (unsigned char)lower[i];
			const bool keep = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
			if (keep)
			{
				slug += (char)ch;
				lastWasDash = false;
			}
			else if (!lastWasDash && !slug.empty())
			{
				slug += '-';
				lastWasDash = true;
			}
		}
		while (!slug.empty() && slug[slug.size() - 1] == '-')
			slug.erase(slug.size() - 1);
		if (slug.empty())
			slug = "unknown-map";
		return slug;
	}

	std::string JsonEscape(const std::string& text)
	{
		std::string out;
		for (size_t i = 0; i < text.size(); ++i)
		{
			const unsigned char ch = (unsigned char)text[i];
			switch (ch)
			{
				case '"':  out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\b': out += "\\b"; break;
				case '\f': out += "\\f"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (ch < 0x20)
						out += FormatString("\\u%04x", (int)ch);
					else
						out += (char)ch;
					break;
			}
		}
		return out;
	}

	std::string JsonString(const std::string& text)
	{
		return std::string("\"") + JsonEscape(text) + "\"";
	}

	std::string JsonStringOrNull(const char* text)
	{
		if (text == nullptr || text[0] == '\0')
			return "null";
		return JsonString(text);
	}

	bool EnsureDirectory(const std::string& dir)
	{
		if (dir.empty())
			return false;
		std::string partial;
		for (size_t i = 0; i < dir.size(); ++i)
		{
			partial += dir[i];
			if ((dir[i] == '\\' || dir[i] == '/') && partial.size() > 3)
				CreateDirectoryA(partial.c_str(), nullptr);
		}
		CreateDirectoryA(dir.c_str(), nullptr);
		const DWORD attributes = GetFileAttributesA(dir.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}

	bool WriteTextFile(const std::string& path, const std::string& text)
	{
		FILE* file = fopen(path.c_str(), "wb");
		if (file == nullptr)
			return false;
		const size_t written = fwrite(text.data(), 1, text.size(), file);
		fclose(file);
		return written == text.size();
	}

	uint64_t Fnv1a64Update(uint64_t hash, const unsigned char* data, size_t length)
	{
		for (size_t i = 0; i < length; ++i)
		{
			hash ^= (uint64_t)data[i];
			hash *= 1099511628211ULL;
		}
		return hash;
	}

	bool HashFile(const std::string& path, uint64_t* outHash, unsigned long long* outBytes)
	{
		FILE* file = fopen(path.c_str(), "rb");
		if (file == nullptr)
			return false;
		uint64_t hash = 1469598103934665603ULL;
		unsigned long long total = 0;
		unsigned char buffer[65536];
		size_t read = 0;
		while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0)
		{
			hash = Fnv1a64Update(hash, buffer, read);
			total += read;
		}
		fclose(file);
		if (outHash != nullptr) *outHash = hash;
		if (outBytes != nullptr) *outBytes = total;
		return true;
	}

	// Reads `"key": "value"` from `text` at or after `from`. Only ever used on a sidecar this same
	// code wrote, so it can assume the compact formatting below.
	bool JsonFindString(const std::string& text, const char* key, size_t from, std::string* out, size_t* outEnd)
	{
		const std::string needle = std::string("\"") + key + "\"";
		size_t pos = text.find(needle, from);
		if (pos == std::string::npos)
			return false;
		pos = text.find(':', pos + needle.size());
		if (pos == std::string::npos)
			return false;
		++pos;
		while (pos < text.size() && (text[pos] == ' ' || text[pos] == '	'))
			++pos;
		if (pos >= text.size() || text[pos] != '"')
			return false;
		++pos;
		std::string value;
		while (pos < text.size() && text[pos] != '"')
		{
			if (text[pos] == '\\' && pos + 1 < text.size())
				++pos;
			value += text[pos++];
		}
		if (out != nullptr) *out = value;
		if (outEnd != nullptr) *outEnd = pos;
		return true;
	}

	bool JsonFindInt(const std::string& text, const char* key, size_t from, long long* out)
	{
		const std::string needle = std::string("\"") + key + "\"";
		size_t pos = text.find(needle, from);
		if (pos == std::string::npos)
			return false;
		pos = text.find(':', pos + needle.size());
		if (pos == std::string::npos)
			return false;
		++pos;
		while (pos < text.size() && (text[pos] == ' ' || text[pos] == '	'))
			++pos;
		char* end = nullptr;
		const long long value = strtoll(text.c_str() + pos, &end, 10);
		if (end == text.c_str() + pos)
			return false;
		if (out != nullptr) *out = value;
		return true;
	}

	const char* BuildingCategoryName(int32_t category)
	{
		switch (category)
		{
			case GO_BUILDING_CATEGORY_COMMAND_CENTER: return "command_center";
			case GO_BUILDING_CATEGORY_WAR_FACTORY:    return "war_factory";
			case GO_BUILDING_CATEGORY_BARRACKS:       return "barracks";
			case GO_BUILDING_CATEGORY_AIRFIELD:       return "airfield";
			case GO_BUILDING_CATEGORY_SUPPLY_STASH:   return "supply_stash";
			default:                                   return nullptr;
		}
	}

	struct GarrisonEntry
	{
		int32_t containerId = 0;
		std::string templateName;
		bool hasScreen = false;
		int32_t screenX = 0;
		int32_t screenY = 0;
		std::string occupantsJson;   // array body, already serialised
		int32_t occupantCount = 0;
	};

	std::string BuildOwnerJson(const GOObjectInfo& info, const Object* object)
	{
		const bool neutral = (object != nullptr) ? (object->isNeutralControlled() ? true : false)
		                                         : (info.owner_index < 0);
		std::string ownerName;
		uint32_t color = 0;
		if (info.owner_index >= 0)
		{
			ownerName = HostAPI_GetPlayerName((uint32_t)info.owner_index);
			color = HostAPI_GetPlayerColor((uint32_t)info.owner_index);
		}
		return FormatString("{\"player_index\":%d,\"name\":%s,\"color_argb\":%u,\"neutral\":%s}",
			(int)info.owner_index,
			JsonString(ownerName).c_str(),
			(unsigned)color,
			neutral ? "true" : "false");
	}

	std::string BuildGarrisonBlockJson(const GarrisonEntry& entry)
	{
		return FormatString("{\"container_object_id\":%d,\"template\":%s,\"screen\":{\"x\":%d,\"y\":%d},\"occupants\":[%s]}",
			(int)entry.containerId,
			JsonStringOrNull(entry.templateName.empty() ? nullptr : entry.templateName.c_str()).c_str(),
			(int)entry.screenX,
			(int)entry.screenY,
			entry.occupantsJson.c_str());
	}

	// Every live container with occupants, whether or not it is player-owned: the walk is the
	// engine's world list, so a civilian/neutral garrison building is included. Containers whose
	// point is off screen are dropped with the rest of the off-screen data.
	void CollectGarrisons(std::vector<GarrisonEntry>& out)
	{
		out.clear();
		if (TheGameLogic == nullptr || TheTacticalView == nullptr)
			return;

		for (Object* obj = TheGameLogic->getFirstObject(); obj != nullptr; obj = obj->getNextObject())
		{
			if (obj->isEffectivelyDead())
				continue;
			ContainModuleInterface* contain = obj->getContain();
			if (contain == nullptr)
				continue;
			const UnsignedInt containedCount = contain->getContainCount();
			if (containedCount == 0)
				continue;

			const Coord3D* pos = obj->getPosition();
			if (pos == nullptr)
				continue;
			ICoord2D screen;
			if (!TheTacticalView->worldToScreen(pos, &screen))
				continue;
			if (TheDisplay != nullptr && (screen.x < 0 || screen.y < 0
				|| (int)screen.x >= (int)TheDisplay->getWidth() || (int)screen.y >= (int)TheDisplay->getHeight()))
				continue;

			GarrisonEntry entry;
			entry.containerId = (int32_t)obj->getID();
			if (obj->getTemplate() != nullptr)
				entry.templateName = obj->getTemplate()->getName().str();
			entry.hasScreen = true;
			entry.screenX = (int32_t)screen.x;
			entry.screenY = (int32_t)screen.y;

			GOContainedObjectInfo occupants[64];
			const uint32_t occupantCount = HostAPI_GetContainedObjects((uint32_t)entry.containerId,
				occupants, (uint32_t)(sizeof(occupants) / sizeof(occupants[0])));
			std::string body;
			for (uint32_t i = 0; i < occupantCount; ++i)
			{
				if (i > 0)
					body += ",";
				body += FormatString("{\"object_id\":%u,\"template\":%s,\"player_index\":%d}",
					(unsigned)occupants[i].objectId,
					JsonStringOrNull(occupants[i].templateName).c_str(),
					(int)occupants[i].playerIndex);
			}
			entry.occupantsJson = body;
			entry.occupantCount = (int32_t)occupantCount;
			out.push_back(entry);
		}
	}

	std::string CaptureUtcNow()
	{
		SYSTEMTIME st;
		GetSystemTime(&st);
		return FormatString("%04d-%02d-%02dT%02d:%02d:%02dZ",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	}

	std::string CaptureLocalNow(int* outOffsetMinutes)
	{
		SYSTEMTIME st;
		GetLocalTime(&st);

		TIME_ZONE_INFORMATION tzi;
		DWORD zone = GetTimeZoneInformation(&tzi);
		int offsetMinutes = 0;
		if (zone != TIME_ZONE_ID_INVALID)
		{
			LONG bias = tzi.Bias;
			if (zone == TIME_ZONE_ID_DAYLIGHT)
				bias += tzi.DaylightBias;
			else
				bias += tzi.StandardBias;
			// Windows reports UTC = local + bias, so the local offset from UTC is the negative.
			offsetMinutes = -(int)bias;
		}
		if (outOffsetMinutes != nullptr)
			*outOffsetMinutes = offsetMinutes;

		const char sign = (offsetMinutes < 0) ? '-' : '+';
		const int absolute = (offsetMinutes < 0) ? -offsetMinutes : offsetMinutes;
		return FormatString("%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
			sign, absolute / 60, absolute % 60);
	}

	// Rewrites captures\manifest.js from the directory. A plain script, not JSON, because the studio
	// is opened from file:// and cannot fetch - the same trick as the backdrop manifest.
	void RebuildCaptureManifest(const std::string& directory)
	{
		std::vector<std::string> jsonFiles;
		WIN32_FIND_DATAA findData;
		HANDLE find = FindFirstFileA((directory + "*.json").c_str(), &findData);
		if (find != INVALID_HANDLE_VALUE)
		{
			do
			{
				if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
					jsonFiles.push_back(findData.cFileName);
			} while (FindNextFileA(find, &findData));
			FindClose(find);
		}
		std::sort(jsonFiles.begin(), jsonFiles.end());
		std::reverse(jsonFiles.begin(), jsonFiles.end());

		std::string script =
			"// Generated by the overlay plugin on every capture. A plain script, not JSON, because\n"
			"// a page opened from file:// cannot fetch - but it can load a script.\n"
			"window.OVERLAY_CAPTURES = [\n";

		for (size_t i = 0; i < jsonFiles.size(); ++i)
		{
			const std::string jsonPath = directory + jsonFiles[i];
			FILE* file = fopen(jsonPath.c_str(), "rb");
			if (file == nullptr)
				continue;
			std::string text;
			char buffer[8192];
			size_t read = 0;
			while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0)
				text.append(buffer, read);
			fclose(file);

			// Scope each lookup to its own block: "name" and "width" appear more than once.
			const size_t imageAt = text.find("\"image\"");
			const size_t captureAt = text.find("\"capture\"");
			const size_t mapAt = text.find("\"map\"");
			const size_t countsAt = text.find("\"counts\"");

			std::string pngFile, utc, mapName;
			long long width = 0, height = 0, buildingCount = 0;
			JsonFindString(text, "file", imageAt == std::string::npos ? 0 : imageAt, &pngFile, nullptr);
			JsonFindInt(text, "width", imageAt == std::string::npos ? 0 : imageAt, &width);
			JsonFindInt(text, "height", imageAt == std::string::npos ? 0 : imageAt, &height);
			JsonFindString(text, "utc", captureAt == std::string::npos ? 0 : captureAt, &utc, nullptr);
			JsonFindString(text, "name", mapAt == std::string::npos ? 0 : mapAt, &mapName, nullptr);
			JsonFindInt(text, "buildings", countsAt == std::string::npos ? 0 : countsAt, &buildingCount);
			if (pngFile.empty())
				continue;

			std::string jsonLeaf = jsonFiles[i];
			script += FormatString("  { name: %s, png: %s, json: %s, w: %lld, h: %lld, utc: %s, map: %s, buildingCount: %lld },\n",
				JsonString(pngFile).c_str(),
				JsonString(pngFile).c_str(),
				JsonString(jsonLeaf).c_str(),
				width, height,
				JsonString(utc).c_str(),
				JsonString(mapName).c_str(),
				buildingCount);
		}

		script += "];\n";
		WriteTextFile(directory + "manifest.js", script);
	}

	// Builds and writes the pair. Returns true only when both files exist at their final names.
	bool WriteCapturePair(const PendingCaptureRequest& request, std::string* outPngPath)
	{
		if (s_screenshotWriter == nullptr || TheGameLogic == nullptr || TheGlobalData == nullptr)
			return false;
		if (!TheGameLogic->isInGame())
			return false;

		const std::string userDataRoot = HostAPI_GetUserDataPath();
		if (userDataRoot.empty())
			return false;
		std::string captureDir = userDataRoot;
		if (captureDir[captureDir.size() - 1] != '\\' && captureDir[captureDir.size() - 1] != '/')
			captureDir += '\\';
		captureDir += "OverlayPlugin\\captures\\";
		if (!EnsureDirectory(captureDir))
			return false;

		// ---- Metadata, all from the frame the plugin asked on. ----
		GOCameraState camera = request.camera;
		if (!request.hasCamera && !HostAPI_GetCameraState(&camera))
			return false;

		GOMapInfo mapInfo = {};
		HostAPI_GetMapInfo(&mapInfo);

		std::vector<GOObjectInfo> objects = request.objects;
		if (objects.empty())
		{
			int32_t total = 0;
			objects.resize(4096);
			const int32_t written = HostAPI_GetMapObjects(objects.data(), (int32_t)objects.size(),
				GO_OBJECT_KIND_ALL, &total);
			objects.resize((size_t)(written > 0 ? written : 0));
		}

		std::vector<GarrisonEntry> garrisons;
		CollectGarrisons(garrisons);

		// ---- Filename: <slug>__<YYYYMMDD>-<HHMMSS>__<W>x<H>, collision suffix -2, -3, ... ----
		const std::string mapName = (mapInfo.name[0] != '\0') ? std::string(mapInfo.name) : std::string();
		std::string slug = Slugify(!request.name.empty() ? request.name : mapName);

		SYSTEMTIME now;
		GetLocalTime(&now);
		const std::string stem = FormatString("%s__%04d%02d%02d-%02d%02d%02d",
			slug.c_str(), now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);

		// A placeholder resolution first: the real one comes back from the writer, and the name has
		// to contain it. Try the name with the display size, then rename if the image disagrees.
		int imageWidth = (camera.viewportWidth > 0) ? camera.viewportWidth : 0;
		int imageHeight = (camera.viewportHeight > 0) ? camera.viewportHeight : 0;
		if ((imageWidth <= 0 || imageHeight <= 0) && TheDisplay != nullptr)
		{
			imageWidth = (int)TheDisplay->getWidth();
			imageHeight = (int)TheDisplay->getHeight();
		}

		std::string baseName;
		std::string pngPath;
		std::string tmpPngPath;
		for (int attempt = 1; attempt < 1000; ++attempt)
		{
			baseName = FormatString("%s__%dx%d%s", stem.c_str(), imageWidth, imageHeight,
				attempt == 1 ? "" : FormatString("-%d", attempt).c_str());
			pngPath = captureDir + baseName + ".png";
			if (GetFileAttributesA(pngPath.c_str()) == INVALID_FILE_ATTRIBUTES)
				break;
		}
		tmpPngPath = pngPath + ".tmp";

		int writtenWidth = imageWidth;
		int writtenHeight = imageHeight;
		if (!s_screenshotWriter(tmpPngPath.c_str(), &writtenWidth, &writtenHeight))
		{
			DeleteFileA(tmpPngPath.c_str());
			return false;
		}

		// The image size is authoritative for the filename, so rename when the display size and the
		// back buffer disagree (fullscreen scaling, a letterboxed render target).
		if (writtenWidth > 0 && writtenHeight > 0 && (writtenWidth != imageWidth || writtenHeight != imageHeight))
		{
			imageWidth = writtenWidth;
			imageHeight = writtenHeight;
			std::string renamedBase;
			std::string renamedPng;
			for (int attempt = 1; attempt < 1000; ++attempt)
			{
				renamedBase = FormatString("%s__%dx%d%s", stem.c_str(), imageWidth, imageHeight,
					attempt == 1 ? "" : FormatString("-%d", attempt).c_str());
				renamedPng = captureDir + renamedBase + ".png";
				if (GetFileAttributesA(renamedPng.c_str()) == INVALID_FILE_ATTRIBUTES)
					break;
			}
			baseName = renamedBase;
			pngPath = renamedPng;
		}

		uint64_t checksum = 0;
		unsigned long long imageBytes = 0;
		HashFile(tmpPngPath, &checksum, &imageBytes);

		// ---- Sidecar ----
		std::vector<std::string> degraded;
		auto noteDegraded = [&degraded](const char* path) {
			for (size_t i = 0; i < degraded.size(); ++i)
				if (degraded[i] == path)
					return;
			degraded.push_back(path);
		};

		int offsetMinutes = 0;
		const std::string localIso = CaptureLocalNow(&offsetMinutes);
		const std::string utcIso = CaptureUtcNow();
		const std::string captureId = FormatString("%04d%02d%02d-%02d%02d%02d-%04x",
			now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
			(unsigned)(GetTickCount() & 0xffff));

		std::string json;
		json += "{\n";
		json += "  \"schema\": \"overlayplugin.capture/1\",\n";
		json += FormatString("  \"capture\": {\"id\": %s, \"utc\": %s, \"local\": %s, \"trigger\": %s, \"plugin_version\": null, \"game_build\": null},\n",
			JsonString(captureId).c_str(), JsonString(utcIso).c_str(), JsonString(localIso).c_str(),
			JsonString(request.trigger.empty() ? "api" : request.trigger).c_str());
		noteDegraded("capture.plugin_version");
		noteDegraded("capture.game_build");

		const bool isReplay = TheGameLogic->isInReplayGame() ? true : false;
		json += FormatString("  \"match\": {\"kind\": %s, \"spectating\": %s, \"paused\": %s, \"speed\": null, \"logic_frame\": %u, \"logic_fps\": %u},\n",
			isReplay ? "\"replay\"" : "\"live\"",
			GOPluginManager::IsLocalPlayerObserver() ? "true" : "false",
			camera.paused ? "true" : "false",
			(unsigned)camera.logicFrame,
			(unsigned)LOGICFRAMES_PER_SECOND);
		noteDegraded("match.speed");

		json += FormatString("  \"map\": {\"name\": %s, \"display_name\": null, \"source\": %s, \"width_world\": %s, \"height_world\": %s, \"origin_world\": null},\n",
			mapName.empty() ? "null" : JsonString(mapName).c_str(),
			mapName.empty() ? "\"unknown\"" : "\"filename\"",
			mapInfo.extent[0] > 0 ? FormatString("%d", (int)mapInfo.extent[0]).c_str() : "null",
			mapInfo.extent[1] > 0 ? FormatString("%d", (int)mapInfo.extent[1]).c_str() : "null");
		noteDegraded("map.display_name");
		noteDegraded("map.origin_world");
		if (mapInfo.extent[0] <= 0) noteDegraded("map.width_world");
		if (mapInfo.extent[1] <= 0) noteDegraded("map.height_world");

		// The studio's existing reader wants a flat resolution; keep it beside the spec blocks.
		json += FormatString("  \"resolution\": {\"w\": %d, \"h\": %d},\n", imageWidth, imageHeight);
		json += FormatString("  \"image\": {\"file\": %s, \"format\": \"png\", \"width\": %d, \"height\": %d, \"bytes\": %llu, \"checksum\": {\"algorithm\": \"fnv1a64\", \"value\": \"%016llx\"}},\n",
			JsonString(baseName + ".png").c_str(), imageWidth, imageHeight, imageBytes, (unsigned long long)checksum);

		const int renderWidth = (camera.viewportWidth > 0) ? camera.viewportWidth : imageWidth;
		const int renderHeight = (camera.viewportHeight > 0) ? camera.viewportHeight : imageHeight;
		json += FormatString("  \"render\": {\"width\": %d, \"height\": %d, \"scale_to_image\": %.4f},\n",
			renderWidth, renderHeight,
			renderWidth > 0 ? (double)imageWidth / (double)renderWidth : 1.0);

		// ---- Camera. forward/right/up are derived from the eye and the engine's own look-at point,
		// which is the pair getCameraState exposes; near/far are not reachable, so they are null.
		const double dx = (double)camera.target[0] - (double)camera.pos[0];
		const double dy = (double)camera.target[1] - (double)camera.pos[1];
		const double dz = (double)camera.target[2] - (double)camera.pos[2];
		const double length = sqrt(dx * dx + dy * dy + dz * dz);
		double fx = 0.0, fy = 0.0, fz = 0.0;
		double rx = 0.0, ry = 0.0, rz = 0.0;
		double ux = 0.0, uy = 0.0, uz = 0.0;
		if (length > 1e-6)
		{
			fx = dx / length; fy = dy / length; fz = dz / length;
			// right = normalize(forward x worldUp), up = right x forward. Generals' ground plane is
			// Z-up, so this reproduces a conventional view basis without a matrix from the engine.
			rx = fy * 1.0 - fz * 0.0;
			ry = fz * 0.0 - fx * 1.0;
			rz = fx * 0.0 - fy * 0.0;
			const double rlen = sqrt(rx * rx + ry * ry + rz * rz);
			if (rlen > 1e-6) { rx /= rlen; ry /= rlen; rz /= rlen; }
			ux = ry * fz - rz * fy;
			uy = rz * fx - rx * fz;
			uz = rx * fy - ry * fx;
		}
		const double hfov = (double)camera.fovHorizontalDegrees;
		const double vfov = (renderWidth > 0)
			? 2.0 * atan(tan(hfov * 3.14159265358979323846 / 360.0) * (double)renderHeight / (double)renderWidth) * 180.0 / 3.14159265358979323846
			: 0.0;

		json += "  \"camera\": {\n";
		json += "    \"projection\": \"perspective\",\n";
		json += FormatString("    \"position\": {\"x\": %.3f, \"y\": %.3f, \"z\": %.3f},\n", camera.pos[0], camera.pos[1], camera.pos[2]);
		json += FormatString("    \"target\": {\"x\": %.3f, \"y\": %.3f, \"z\": %.3f},\n", camera.target[0], camera.target[1], camera.target[2]);
		json += FormatString("    \"forward\": {\"x\": %.6f, \"y\": %.6f, \"z\": %.6f},\n", fx, fy, fz);
		json += FormatString("    \"right\": {\"x\": %.6f, \"y\": %.6f, \"z\": %.6f},\n", rx, ry, rz);
		json += FormatString("    \"up\": {\"x\": %.6f, \"y\": %.6f, \"z\": %.6f},\n", ux, uy, uz);
		json += FormatString("    \"pitch_deg\": %.4f, \"yaw_deg\": %.4f, \"roll_deg\": null,\n",
			(double)camera.pitchRadians * 180.0 / 3.14159265358979323846,
			(double)camera.yawRadians * 180.0 / 3.14159265358979323846);
		json += FormatString("    \"height_world\": %.3f, \"zoom\": null,\n", camera.pos[2]);
		json += FormatString("    \"vfov_deg\": %.4f, \"hfov_deg\": %.4f, \"near\": null, \"far\": null, \"ground_z\": null,\n", vfov, hfov);
		json += FormatString("    \"viewport\": {\"x\": %d, \"y\": %d, \"width\": %d, \"height\": %d}\n",
			(int)camera.viewportX, (int)camera.viewportY, renderWidth, renderHeight);
		json += "  },\n";
		noteDegraded("camera.roll_deg");
		noteDegraded("camera.zoom");
		noteDegraded("camera.near");
		noteDegraded("camera.far");
		noteDegraded("camera.ground_z");

		// A projection matrix would have to reproduce the engine's own worldToScreen to 1 px; the
		// dots come straight from that call instead, so the matrix stays null rather than competing.
		json += "  \"transform\": {\"world_to_image\": null, \"ground_to_image\": null, \"image_to_ground\": null, \"residual_px\": null},\n";
		noteDegraded("transform.world_to_image");
		noteDegraded("transform.ground_to_image");
		noteDegraded("transform.image_to_ground");
		noteDegraded("transform.residual_px");

		// ---- Objects. Only what is inside the viewport: an entry without a dot is wasted data. ----
		int32_t buildingCount = 0;
		int32_t unitCount = 0;
		int32_t droppedOffscreen = 0;
		std::string buildingsJson, unitsJson;

		for (size_t i = 0; i < objects.size(); ++i)
		{
			const GOObjectInfo& info = objects[i];
			if (!info.on_screen)
			{
				++droppedOffscreen;
				continue;
			}

			const Object* object = TheGameLogic->findObjectByID((ObjectID)info.id);
			const std::string templateName(info.template_name);
			const char* displayNameRaw = templateName.empty() ? "" : HostAPI_GetTemplateDisplayName(templateName.c_str());
			std::string displayName(displayNameRaw);

			std::string badgeJson = "null";
			int32_t badgeX = 0, badgeY = 0;
			if (HostAPI_GetObjectHealthBarScreenPosition((uint32_t)info.id, &badgeX, &badgeY))
				badgeJson = FormatString("{\"x\": %d, \"y\": %d}", (int)badgeX, (int)badgeY);

			std::string garrisonJson;
			for (size_t g = 0; g < garrisons.size(); ++g)
			{
				if (garrisons[g].containerId == info.id)
				{
					garrisonJson = std::string(",\n      \"garrison\": ") + BuildGarrisonBlockJson(garrisons[g]);
					break;
				}
			}

			const std::string ownerJson = BuildOwnerJson(info, object);

			if (info.is_structure)
			{
				if (buildingCount > 0) buildingsJson += ",\n";
				const char* category = BuildingCategoryName((int32_t)HostAPI_GetObjectBuildingCategory((uint32_t)info.id));
				std::string heightJson = "null";
				if (object != nullptr)
					heightJson = FormatString("%.3f", object->getGeometryInfo().getMaxHeightAbovePosition());
				if (templateName.empty()) noteDegraded("buildings[].template");
				if (displayName.empty()) noteDegraded("buildings[].display_name");
				if (category == nullptr) noteDegraded("buildings[].category");
				if (heightJson == "null") noteDegraded("buildings[].height_world");
				if (info.radius <= 0.0f) noteDegraded("buildings[].footprint.radius_world");

				buildingsJson += FormatString(
					"    {\"object_id\": %d, \"template\": %s, \"display_name\": %s, \"category\": %s,\n"
					"     \"owner\": %s,\n"
					"     \"world\": {\"x\": %.3f, \"y\": %.3f, \"z\": %.3f}, \"height_world\": %s,\n"
					"     \"footprint\": {\"radius_world\": %s},\n"
					"     \"screen\": {\"x\": %d, \"y\": %d, \"clamped\": false}, \"badge\": %s%s}",
					(int)info.id,
					JsonStringOrNull(templateName.empty() ? nullptr : templateName.c_str()).c_str(),
					JsonStringOrNull(displayName.empty() ? nullptr : displayName.c_str()).c_str(),
					JsonStringOrNull(category).c_str(),
					ownerJson.c_str(),
					info.pos[0], info.pos[1], info.pos[2],
					heightJson.c_str(),
					info.radius > 0.0f ? FormatString("%.3f", info.radius).c_str() : "null",
					(int)info.screen[0], (int)info.screen[1],
					badgeJson.c_str(),
					garrisonJson.c_str());
				++buildingCount;
			}
			else
			{
				if (unitCount > 0) unitsJson += ",\n";
				if (templateName.empty()) noteDegraded("units[].template");
				if (displayName.empty()) noteDegraded("units[].display_name");
				if (info.radius <= 0.0f) noteDegraded("units[].radius_world");

				unitsJson += FormatString(
					"    {\"object_id\": %d, \"template\": %s, \"display_name\": %s,\n"
					"     \"owner\": %s,\n"
					"     \"world\": {\"x\": %.3f, \"y\": %.3f, \"z\": %.3f}, \"radius_world\": %s,\n"
					"     \"screen\": {\"x\": %d, \"y\": %d, \"clamped\": false}, \"badge\": %s, \"is_structure\": false%s}",
					(int)info.id,
					JsonStringOrNull(templateName.empty() ? nullptr : templateName.c_str()).c_str(),
					JsonStringOrNull(displayName.empty() ? nullptr : displayName.c_str()).c_str(),
					ownerJson.c_str(),
					info.pos[0], info.pos[1], info.pos[2],
					info.radius > 0.0f ? FormatString("%.3f", info.radius).c_str() : "null",
					(int)info.screen[0], (int)info.screen[1],
					badgeJson.c_str(),
					garrisonJson.c_str());
				++unitCount;
			}
		}

		json += "  \"buildings\": [\n" + buildingsJson + (buildingCount > 0 ? "\n  " : "") + "],\n";
		json += "  \"units\": [\n" + unitsJson + (unitCount > 0 ? "\n  " : "") + "],\n";

		std::string garrisonsJson;
		for (size_t g = 0; g < garrisons.size(); ++g)
		{
			if (g > 0) garrisonsJson += ",\n";
			garrisonsJson += "    " + BuildGarrisonBlockJson(garrisons[g]);
		}
		json += "  \"garrisons\": [" + (garrisons.empty() ? std::string("],\n") : ("\n" + garrisonsJson + "\n  ],\n"));

		json += FormatString("  \"counts\": {\"buildings\": %d, \"units\": %d, \"garrisons\": %d, \"dropped_offscreen\": %d},\n",
			(int)buildingCount, (int)unitCount, (int)garrisons.size(), (int)droppedOffscreen);

		json += "  \"degraded\": [";
		for (size_t i = 0; i < degraded.size(); ++i)
		{
			if (i > 0) json += ", ";
			json += JsonString(degraded[i]);
		}
		json += "]\n}\n";

		const std::string jsonFinalPath = captureDir + baseName + ".json";
		const std::string jsonTmpPath = jsonFinalPath + ".tmp";
		if (!WriteTextFile(jsonTmpPath, json))
		{
			DeleteFileA(tmpPngPath.c_str());
			return false;
		}

		// Rename only once both files are complete, so a watcher never sees half a pair.
		if (!MoveFileExA(tmpPngPath.c_str(), pngPath.c_str(), MOVEFILE_REPLACE_EXISTING))
		{
			DeleteFileA(tmpPngPath.c_str());
			DeleteFileA(jsonTmpPath.c_str());
			return false;
		}
		if (!MoveFileExA(jsonTmpPath.c_str(), jsonFinalPath.c_str(), MOVEFILE_REPLACE_EXISTING))
		{
			DeleteFileA(jsonTmpPath.c_str());
			return false;
		}

		RebuildCaptureManifest(captureDir);

		if (TheInGameUI != nullptr)
		{
			UnicodeString caption;
			caption.translate("Capture saved");
			UnicodeString fileName;
			fileName.translate((baseName + ".png").c_str());
			TheInGameUI->message(L"%s: %s", caption.str(), fileName.str());
		}

		if (outPngPath != nullptr)
			*outPngPath = pngPath;
		return true;
	}

	int32_t HostAPI_RequestCaptureEx(const GOCaptureRequest* request)
	{
		if (request == nullptr)
			return 0;
		if (TheGameLogic == nullptr || !TheGameLogic->isInGame())
			return 0;
		if (s_pendingCapture.pending)
			return 0;

		PendingCaptureRequest pending;
		pending.pending = true;
		if (request->name != nullptr)
			pending.name = request->name;
		if (request->trigger != nullptr)
			pending.trigger = request->trigger;
		if (request->camera != nullptr)
		{
			pending.camera = *request->camera;
			pending.hasCamera = true;
		}
		if (request->objects != nullptr && request->object_count > 0)
			pending.objects.assign(request->objects, request->objects + request->object_count);
		s_pendingCapture = pending;
		return 1;
	}

	int32_t HostAPI_RequestCapture(const char* nameOrNull)
	{
		GOCaptureRequest request = {};
		request.name = nameOrNull;
		request.trigger = "api";
		return HostAPI_RequestCaptureEx(&request);
	}
} // anonymous namespace


// ------------------------------------------------------------------------------------------------

void GOPluginManager::Log(const char* msg)
{
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] %s", msg);
}

// Called by GameEngineDevice once its display is up, before any plugin is loaded.
void GOPluginManager::SetNativeHandleProviders(NativeHandleProvider d3dDevice8, NativeHandleProvider gameWindow)
{
	s_d3dDevice8Provider = d3dDevice8;
	s_gameWindowProvider = gameWindow;
}

void* GOPluginManager::GetD3DDevice8()
{
	return (s_d3dDevice8Provider != nullptr) ? s_d3dDevice8Provider() : nullptr;
}

void* GOPluginManager::GetGameWindow()
{
	return (s_gameWindowProvider != nullptr) ? s_gameWindowProvider() : nullptr;
}

void GOPluginManager::SetScreenshotWriter(ScreenshotWriter writer)
{
	s_screenshotWriter = writer;
}

// Called by the display immediately before the frame is presented. The back buffer at this point
// holds the frame the plugin's metadata was sampled on, and the plugin's own EndScene-time drawing
// (its ImGui menu) has not happened yet, so the image is the game's frame without plugin pixels.
bool GOPluginManager::ProcessPendingCapture()
{
	if (!s_pendingCapture.pending)
		return false;

	// Take the request out first: a failed write must not retry every frame.
	const PendingCaptureRequest request = s_pendingCapture;
	s_pendingCapture = PendingCaptureRequest();

	std::string pngPath;
	if (!WriteCapturePair(request, &pngPath))
	{
		Log("capture failed - no pair written");
		return false;
	}

	Log(FormatString("capture written: %s", pngPath.c_str()).c_str());
	return true;
}


GOPluginHostAPI GOPluginManager::BuildHostAPI()
{
	GOPluginHostAPI api = {};
	api.abiVersion = GO_PLUGIN_ABI_VERSION;
	api.structSize = (uint32_t)sizeof(GOPluginHostAPI);
	api.log = HostAPI_Log;
	api.registerGameplayEventHooks = HostAPI_RegisterGameplayEventHooks;
	api.registerRenderHooks = HostAPI_RegisterRenderHooks;
	api.getPlayerColor = HostAPI_GetPlayerColor;
	api.getActivePlayers = HostAPI_GetActivePlayers;
	api.isLocalPlayerObserver = HostAPI_IsLocalPlayerObserver;
	api.getLocalPlayerIndex = HostAPI_GetLocalPlayerIndex;
	api.getPlayerGeneralPowers = HostAPI_GetPlayerGeneralPowers;
	api.drawTemplateIcon2D = HostAPI_DrawTemplateIcon2D;
	api.drawPowerIcon2D = HostAPI_DrawPowerIcon2D;
	api.drawText2D = HostAPI_DrawText2D;
	api.drawText2DScaled = HostAPI_DrawText2DScaled;
	api.drawRect2D = HostAPI_DrawRect2D;
	api.getScreenSize = HostAPI_GetScreenSize;
	api.getCameraState = HostAPI_GetCameraState;
	api.getMapInfo = HostAPI_GetMapInfo;
	api.getMapObjects = HostAPI_GetMapObjects;
	api.requestCapture = HostAPI_RequestCapture;
	api.requestCaptureEx = HostAPI_RequestCaptureEx;
	api.getLogicFrame = HostAPI_GetLogicFrame;
	api.getLogicFramesPerSecond = HostAPI_GetLogicFramesPerSecond;
	api.getUnitProductionProgress = HostAPI_GetUnitProductionProgress;
	api.getUpgradeProductionProgress = HostAPI_GetUpgradeProductionProgress;
	api.worldToScreen = HostAPI_WorldToScreen;
	api.worldToScreenClamped = HostAPI_WorldToScreenClamped;
	api.getObjectBuildingCategory = HostAPI_GetObjectBuildingCategory;
	api.getObjectTemplateName = HostAPI_GetObjectTemplateName;
	api.getObjectScreenBounds = HostAPI_GetObjectScreenBounds;
	api.getObjectHealthBarScreenPosition = HostAPI_GetObjectHealthBarScreenPosition;
	api.teleportViewportTo = HostAPI_TeleportViewportTo;
	api.drawRectClock2D = HostAPI_DrawRectClock2D;
	api.drawRemainingRectClock2D = HostAPI_DrawRemainingRectClock2D;
	api.getD3DDevice8 = HostAPI_GetD3DDevice8;
	api.getGameWindow = HostAPI_GetGameWindow;
	api.enumeratePlayerObjects = HostAPI_EnumeratePlayerObjects;
	api.getContainedObjects = HostAPI_GetContainedObjects;
	api.getObjectTargetPosition = HostAPI_GetObjectTargetPosition;
	api.isObjectAirborne = HostAPI_IsObjectAirborne;
	api.isObjectVehicle = HostAPI_IsObjectVehicle;
	api.getPlayerName = HostAPI_GetPlayerName;
	api.getPlayerFactionTemplate = HostAPI_GetPlayerFactionTemplate;
	api.getPlayerMoney = HostAPI_GetPlayerMoney;
	api.getPlayerRank = HostAPI_GetPlayerRank;
	api.getPlayerPowerState = HostAPI_GetPlayerPowerState;
	api.getPlayerBuilderCount = HostAPI_GetPlayerBuilderCount;
	api.getPlayerActiveGathererCount = HostAPI_GetPlayerActiveGathererCount;
	api.getPlayerTotalMoneyEarned = HostAPI_GetPlayerTotalMoneyEarned;
	api.getTemplateDisplayName = HostAPI_GetTemplateDisplayName;
	api.getPlayerBuilderTemplateName = HostAPI_GetPlayerBuilderTemplateName;
	api.getPlayerBuilderTemplateCounts = HostAPI_GetPlayerBuilderTemplateCounts;
	api.getPlayerGathererTemplateName = HostAPI_GetPlayerGathererTemplateName;
	api.getPlayerTeamNumber = HostAPI_GetPlayerTeamNumber;
	api.getMatchPlayers = HostAPI_GetMatchPlayers;
	api.getPlayerIsDefeated = HostAPI_GetPlayerIsDefeated;
	api.getUserDataPath = HostAPI_GetUserDataPath;
	api.drawLine2D = HostAPI_DrawLine2D;
	api.isObjectProjectile = HostAPI_IsObjectProjectile;
	api.getObjectContainerId = HostAPI_GetObjectContainerId;
	return api;
}

// The one table handed to every plugin. A function-local static because plugins retain the pointer
// indefinitely, so it must not be a temporary; every entry is a free function, so one instance is
// correct for all of them.
const GOPluginHostAPI& GOPluginManager::GetHostAPI()
{
	static const GOPluginHostAPI s_hostAPI = BuildHostAPI();
	return s_hostAPI;
}

void GOPluginManager::RegisterGameplayEventHooks(const GOGameplayEventCallbacks* cb)
{
	if (cb != nullptr)
		s_gameplayEventHooks.push_back(*cb);
}

void GOPluginManager::RegisterRenderHooks(const GORenderCallbacks* cb)
{
	if (cb != nullptr)
		s_renderHooks.push_back(*cb);
}

// Scans for a single flat "key": "value" string field rather than
// pulling the full json.hpp parser into this file for two optional log-line fields. Not a general
// JSON parser - no nested objects/arrays, only \" and \\ are unescaped - but the manifest below is
// purely informational, so anything this doesn't understand just yields an empty value.
static std::string ExtractJsonStringField(const std::string& json, const char* key)
{
	const std::string quotedKey = std::string("\"") + key + "\"";
	size_t pos = json.find(quotedKey);
	if (pos == std::string::npos)
		return std::string();

	pos = json.find(':', pos + quotedKey.size());
	if (pos == std::string::npos)
		return std::string();
	++pos;

	while (pos < json.size() && isspace((unsigned char)json[pos]))
		++pos;
	if (pos >= json.size() || json[pos] != '"')
		return std::string();
	++pos;

	std::string value;
	while (pos < json.size() && json[pos] != '"')
	{
		if (json[pos] == '\\' && pos + 1 < json.size())
			++pos;
		value += json[pos];
		++pos;
	}
	return value;
}

// Optional sidecar manifest (foo.goplugin.dll -> foo.goplugin.json) with author info for the load
// log. Purely informational - GOPluginInfo stays authoritative, so a missing or malformed manifest
// never refuses a plugin. Returns a log fragment, or "".
static std::string ReadManifestSummary(const char* dllPath)
{
	std::string path(dllPath);
	const size_t dot = path.rfind(".dll");
	if (dot == std::string::npos)
		return std::string();
	path.replace(dot, 4, ".json");

	FILE* f = fopen(path.c_str(), "rb");
	if (f == nullptr)
		return std::string();

	std::string text;
	char buffer[512];
	size_t got;
	while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0)
		text.append(buffer, got);
	fclose(f);

	const std::string author = ExtractJsonStringField(text, "plugin_author");
	const std::string website = ExtractJsonStringField(text, "website");
	if (author.empty() && website.empty())
		return std::string();

	std::string summary = " [by " + (author.empty() ? std::string("unknown") : author);
	if (!website.empty())
		summary += ", " + website;
	return summary + "]";
}

bool GOPluginManager::LoadPlugin(const char* dllPath)
{
	if (dllPath == nullptr)
		return false;

	HMODULE hModule = LoadLibraryA(dllPath);
	if (hModule == nullptr)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] Failed to load %s (err=%u)", dllPath, GetLastError());
		return false;
	}

	GOPluginGetInfoFunc fnGetInfo = (GOPluginGetInfoFunc)GetProcAddress(hModule, GO_PLUGIN_EXPORT_GETINFO_NAME);
	GOPluginInitializeFunc fnInitialize = (GOPluginInitializeFunc)GetProcAddress(hModule, GO_PLUGIN_EXPORT_INITIALIZE_NAME);
	GOPluginShutdownFunc fnShutdown = (GOPluginShutdownFunc)GetProcAddress(hModule, GO_PLUGIN_EXPORT_SHUTDOWN_NAME);
	GOPluginTickFunc fnTick = (GOPluginTickFunc)GetProcAddress(hModule, GO_PLUGIN_EXPORT_TICK_NAME); // optional

	if (fnGetInfo == nullptr || fnInitialize == nullptr || fnShutdown == nullptr)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] %s is missing required exports (GetInfo/Initialize/Shutdown)", dllPath);
		FreeLibrary(hModule);
		return false;
	}

	GOPluginInfo info = {};
	fnGetInfo(&info);

	if (info.abiVersion != GO_PLUGIN_ABI_VERSION)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] %s ABI version mismatch (plugin=%u, host=%u)",
			dllPath, info.abiVersion, (uint32_t)GO_PLUGIN_ABI_VERSION);
		FreeLibrary(hModule);
		return false;
	}

	LoadedPlugin loaded;
	loaded.module = hModule;
	loaded.path = dllPath;
	loaded.name = (info.name != nullptr) ? info.name : "(unnamed)";
	loaded.version = (info.version != nullptr) ? info.version : "(unknown)";
	loaded.shutdown = fnShutdown;
	loaded.tick = fnTick;
	loaded.info = info;

	// A plugin may register hooks during Initialize() and then still return false. Snapshot both
	// vector sizes and roll back on failure, or those callbacks keep pointing into the DLL we are
	// about to FreeLibrary() and the next dispatch calls through freed memory.
	const size_t gameplayHooksBefore = s_gameplayEventHooks.size();
	const size_t renderHooksBefore = s_renderHooks.size();
	if (!fnInitialize(&GetHostAPI()))
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] %s Initialize() returned failure", dllPath);
		s_gameplayEventHooks.resize(gameplayHooksBefore);
		s_renderHooks.resize(renderHooksBefore);
		FreeLibrary(hModule);
		return false;
	}

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] Loaded %s v%s (%s) hooks=0x%X%s",
		loaded.name.c_str(), loaded.version.c_str(), dllPath, info.hookCategories,
		ReadManifestSummary(dllPath).c_str());

	s_plugins.push_back(loaded);
	return true;
}

// Loads every *.goplugin.dll directly inside pluginDir. Returns how many loaded.
static int LoadPluginsInFolder(const std::string& pluginDir)
{
	const std::string searchPattern = pluginDir + "\\*.goplugin.dll";

	WIN32_FIND_DATAA findData;
	HANDLE hFind = FindFirstFileA(searchPattern.c_str(), &findData);
	if (hFind == INVALID_HANDLE_VALUE)
		return 0;

	int loaded = 0;
	do
	{
		if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;

		const std::string fullPath = pluginDir + "\\" + findData.cFileName;
		if (GOPluginManager::LoadPlugin(fullPath.c_str()))
			++loaded;
	}
	while (FindNextFileA(hFind, &findData) != 0);

	FindClose(hFind);
	return loaded;
}

void GOPluginManager::LoadPluginsFromDirectory(const char* directoryPath)
{
	if (directoryPath == nullptr)
		return;

	const std::string root(directoryPath);

	// One folder per plugin, holding its DLL plus an optional .json manifest, so each plugin's
	// files (DLL, manifest, any of its own data) stay together and the plugins directory does not
	// become a pile of loose DLLs.
	int loaded = 0;
	int folders = 0;

	WIN32_FIND_DATAA findData;
	HANDLE hFind = FindFirstFileA((root + "\\*").c_str(), &findData);
	if (hFind != INVALID_HANDLE_VALUE)
	{
		do
		{
			if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
				continue;
			if (strcmp(findData.cFileName, ".") == 0 || strcmp(findData.cFileName, "..") == 0)
				continue;

			++folders;
			loaded += LoadPluginsInFolder(root + "\\" + findData.cFileName);
		}
		while (FindNextFileA(hFind, &findData) != 0);

		FindClose(hFind);
	}

	// A DLL dropped loose in plugins\ is not picked up. Say so loudly rather than silently doing
	// nothing: a plugin that never runs looks exactly like a plugin that runs and draws nothing,
	// and telling those apart cost a full round of testing once already.
	WIN32_FIND_DATAA strayData;
	HANDLE hStray = FindFirstFileA((root + "\\*.goplugin.dll").c_str(), &strayData);
	if (hStray != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (strayData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;

			NetworkLog(ELogVerbosity::LOG_RELEASE,
				"[Plugin] IGNORED %s\\%s - plugins live in their own folder. Move it to %s\\<name>\\%s",
				root.c_str(), strayData.cFileName, root.c_str(), strayData.cFileName);
		}
		while (FindNextFileA(hStray, &strayData) != 0);

		FindClose(hStray);
	}

	if (loaded == 0)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[Plugin] No plugins loaded (%s, %d folder(s) scanned)",
			root.c_str(), folders);
	}
}

void GOPluginManager::UnloadAll()
{
	for (auto it = s_plugins.rbegin(); it != s_plugins.rend(); ++it)
	{
		if (it->shutdown != nullptr)
			it->shutdown();
		if (it->module != nullptr)
			FreeLibrary(it->module);
	}

	s_plugins.clear();
	s_gameplayEventHooks.clear();
	s_renderHooks.clear();
}

// One gate for the whole framework: callbacks carry other players' queues, powers and buildings, so
// none of it may reach a plugin while the local client is a participant. Same condition the engine's
// own observer-only UI uses; replay playback passes, its local player being the observer.
bool GOPluginManager::IsLocalPlayerObserver()
{
	if (ThePlayerList == nullptr)
		return false;
	Player* localPlayer = ThePlayerList->getLocalPlayer();
	return (localPlayer != nullptr && (localPlayer->isPlayerObserver() || localPlayer->isPlayerDead())) ? true : false;
}

void GOPluginManager::Tick()
{
	if (!IsLocalPlayerObserver())
		return;

	for (LoadedPlugin& plugin : s_plugins)
	{
		if (plugin.tick != nullptr)
			plugin.tick();
	}
}

// ---- IGameplayEventHooks dispatch. All no-ops unless IsLocalPlayerObserver() - a match
// participant never receives plugin callbacks. ----
void GOPluginManager::DispatchUnitQueued(const GOUnitEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUnitQueued != nullptr) cb.onUnitQueued(&ev);
}

void GOPluginManager::DispatchUnitCancelled(const GOUnitEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUnitCancelled != nullptr) cb.onUnitCancelled(&ev);
}

void GOPluginManager::DispatchUnitCompleted(const GOUnitEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUnitCompleted != nullptr) cb.onUnitCompleted(&ev);
}

void GOPluginManager::DispatchUpgradeQueued(const GOUpgradeEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUpgradeQueued != nullptr) cb.onUpgradeQueued(&ev);
}

void GOPluginManager::DispatchUpgradeCancelled(const GOUpgradeEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUpgradeCancelled != nullptr) cb.onUpgradeCancelled(&ev);
}

void GOPluginManager::DispatchUpgradeCompleted(const GOUpgradeEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onUpgradeCompleted != nullptr) cb.onUpgradeCompleted(&ev);
}

void GOPluginManager::DispatchBuildingDestroyed(const GOBuildingEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onBuildingDestroyed != nullptr) cb.onBuildingDestroyed(&ev);
}

void GOPluginManager::DispatchSpecialPowerTriggered(const GOSpecialPowerEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onSpecialPowerTriggered != nullptr) cb.onSpecialPowerTriggered(&ev);
}

void GOPluginManager::DispatchObjectDamaged(const GOCombatEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onObjectDamaged != nullptr) cb.onObjectDamaged(&ev);
}

void GOPluginManager::DispatchObjectHealed(const GOCombatEvent& ev)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GOGameplayEventCallbacks& cb : s_gameplayEventHooks)
		if (cb.onObjectHealed != nullptr) cb.onObjectHealed(&ev);
}

// ---- IRenderHooks dispatch. Same observer gate as the gameplay events. ----

void GOPluginManager::DispatchDrawOverlay()
{
	if (!IsLocalPlayerObserver())
		return;
	for (GORenderCallbacks& cb : s_renderHooks)
		if (cb.onDrawOverlay != nullptr) cb.onDrawOverlay();
}

void GOPluginManager::DispatchRawKeyUp(uint32_t scanCode, uint32_t modifierFlags)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GORenderCallbacks& cb : s_renderHooks)
		if (cb.onRawKeyUp != nullptr) cb.onRawKeyUp(scanCode, modifierFlags);
}

void GOPluginManager::DispatchMouseMove(int32_t x, int32_t y)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GORenderCallbacks& cb : s_renderHooks)
		if (cb.onMouseMove != nullptr) cb.onMouseMove(x, y);
}

void GOPluginManager::DispatchMouseButtonDown(uint8_t buttonIndex, int32_t x, int32_t y, uint32_t modifierFlags)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GORenderCallbacks& cb : s_renderHooks)
		if (cb.onMouseButtonDown != nullptr) cb.onMouseButtonDown(buttonIndex, x, y, modifierFlags);
}

void GOPluginManager::DispatchMouseButtonUp(uint8_t buttonIndex, int32_t x, int32_t y, uint32_t modifierFlags)
{
	if (!IsLocalPlayerObserver())
		return;
	for (GORenderCallbacks& cb : s_renderHooks)
		if (cb.onMouseButtonUp != nullptr) cb.onMouseButtonUp(buttonIndex, x, y, modifierFlags);
}
