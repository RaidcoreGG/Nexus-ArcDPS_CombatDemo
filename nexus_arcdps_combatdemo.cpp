/*
* arcdps combat api example
*/

#include <stdint.h>
#include <stdio.h>
#include <Windows.h>
#include "imgui/imgui.h"
#include "nexus/Nexus.h"

/* arcdps export table */
typedef struct arcdps_exports {
	uintptr_t size; /* size of exports table */
	uint32_t sig; /* pick a number between 0 and uint32_t max that isn't used by other modules */
	uint32_t imguivers; /* set this to IMGUI_VERSION_NUM. if you don't use imgui, 18000 (as of 2021-02-02) */
	const char* out_name; /* name string */
	const char* out_build; /* build string */
	void* wnd_nofilter; /* wndproc callback, fn(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam), return assigned to umsg */
	void* combat; /* combat event callback, fn(cbtevent* ev, ag* src, ag* dst, char* skillname, uint64_t id, uint64_t revision) */
	void* imgui; /* ::present callback, before imgui::render, fn(uint32_t not_charsel_or_loading, uint32_t hide_if_combat_or_ooc) */
	void* options_end; /* ::present callback, appending to the end of options window in arcdps, fn() */
	void* combat_local;  /* combat event callback like area but from chat log, fn(cbtevent* ev, ag* src, ag* dst, char* skillname, uint64_t id, uint64_t revision) */
	void* wnd_filter; /* wndproc callback like wnd_nofilter above, input filered using modifiers */
	void* options_windows; /* called once per 'window' option checkbox, with null at the end, non-zero return disables arcdps drawing that checkbox, fn(char* windowname) */
} arcdps_exports;

/* combat event - see evtc docs for details, revision param in combat cb is equivalent of revision byte header */
typedef struct cbtevent {
	uint64_t time;
	uint64_t src_agent;
	uint64_t dst_agent;
	int32_t value;
	int32_t buff_dmg;
	uint32_t overstack_value;
	uint32_t skillid;
	uint16_t src_instid;
	uint16_t dst_instid;
	uint16_t src_master_instid;
	uint16_t dst_master_instid;
	uint8_t iff;
	uint8_t buff;
	uint8_t result;
	uint8_t is_activation;
	uint8_t is_buffremove;
	uint8_t is_ninety;
	uint8_t is_fifty;
	uint8_t is_moving;
	uint8_t is_statechange;
	uint8_t is_flanking;
	uint8_t is_shields;
	uint8_t is_offcycle;
	uint8_t pad61;
	uint8_t pad62;
	uint8_t pad63;
	uint8_t pad64;
} cbtevent;

/* agent short */
typedef struct ag {
	char* name; /* agent name. may be null. valid only at time of event. utf8 */
	uintptr_t id; /* agent unique identifier */
	uint32_t prof; /* profession at time of event. refer to evtc notes for identification */
	uint32_t elite; /* elite spec at time of event. refer to evtc notes for identification */
	uint32_t self; /* 1 if self, 0 if not */
	uint16_t team; /* sep21+ */
} ag;

/* proto/globals */
HMODULE hSelf;
uint32_t cbtcount = 0;
AddonDefinition AddonDef{};
AddonAPI* APIDefs = nullptr;
void mod_init(AddonAPI*);
void mod_release();
UINT mod_wnd(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void mod_combat(void*);

/* arcdps exports */
void* filelog;
void* arclog;

/* dll main -- winapi */
BOOL APIENTRY DllMain(HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH: hSelf = hModule; break;
	case DLL_PROCESS_DETACH: break;
	case DLL_THREAD_ATTACH: break;
	case DLL_THREAD_DETACH: break;
	}
	return TRUE;
}

extern "C" __declspec(dllexport) AddonDefinition* GetAddonDef()
{
	AddonDef.Signature = 0xFFFA;
	AddonDef.APIVersion = NEXUS_API_VERSION;
	AddonDef.Name = "combatdemo";
	AddonDef.Version.Major = 0;
	AddonDef.Version.Minor = 1;
	AddonDef.Version.Build = 0;
	AddonDef.Version.Revision = 0;
	AddonDef.Author = "Raidcore (Port)";
	AddonDef.Description = "arcdps combatdemo";
	AddonDef.Load = mod_init;
	AddonDef.Unload = mod_release;
	AddonDef.Flags = EAddonFlags_None;

	return &AddonDef;
}

void mod_init(AddonAPI* aApi)
{
	APIDefs = aApi;
	ImGui::SetCurrentContext(APIDefs->ImguiContext);
	ImGui::SetAllocatorFunctions((void* (*)(size_t, void*))APIDefs->ImguiMalloc, (void(*)(void*, void*))APIDefs->ImguiFree); // on imgui 1.80+

	APIDefs->SubscribeEvent("EV_ARCDPS_COMBATEVENT_LOCAL_RAW", mod_combat);
	APIDefs->SubscribeEvent("EV_ARCDPS_COMBATEVENT_SQUAD_RAW", mod_combat);

	APIDefs->Log(ELogLevel_INFO, "combatdemo: done mod_init");
}

void mod_release()
{
	APIDefs->UnsubscribeEvent("EV_ARCDPS_COMBATEVENT_LOCAL_RAW", mod_combat);
	APIDefs->UnsubscribeEvent("EV_ARCDPS_COMBATEVENT_SQUAD_RAW", mod_combat);
}

UINT mod_wnd(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	/* much lazy */
	char buff[4096];
	char* p = &buff[0];

	/* yes */
	p += _snprintf_s(p, 400, _TRUNCATE, "combatdemo: ==== wndproc %llx ====\n", (uintptr_t)hWnd);
	p += _snprintf_s(p, 400, _TRUNCATE, "umsg %u, wparam %lld, lparam %lld\n", uMsg, wParam, lParam);

	/* hotkey */
	if (uMsg == WM_KEYDOWN) {
		if (wParam == 0x43) {
			/* do something with c and don't pass to arc/game */
			return 0;
		}
	}

	/* print */
	//log_arc(&buff[0]);
	//log_file(&buff[0]);
	return uMsg;
}

struct EvCombatData
{
	cbtevent* ev;
	ag* src;
	ag* dst;
	char* skillname;
	uint64_t id;
	uint64_t revision;
};

/* combat callback -- may be called asynchronously, use id param to keep track of order, first event id will be 2. return ignored */
/* at least one participant will be party/squad or minion of, or a buff applied by squad in the case of buff remove. not all statechanges present, see evtc statechange enum */
void mod_combat(void* aEventArgs)
{
	EvCombatData* cbtEv = (EvCombatData*)aEventArgs;

	/* much lazy */
	char buff[4096];
	char* p = &buff[0];

	/* ev is null. dst will only be valid on tracking add. skillname will also be null */
	if (!cbtEv->ev) {

		/* notify tracking change */
		if (!cbtEv->src->elite) {

			/* add */
			if (cbtEv->src->prof) {
				p += _snprintf_s(p, 400, _TRUNCATE, "==== cbtnotify ====\n");
				p += _snprintf_s(p, 400, _TRUNCATE, "agent added: %s:%s (%0llx), instid: %u, prof: %u, elite: %u, self: %u, team: %u, subgroup: %u\n", cbtEv->src->name, cbtEv->dst->name, cbtEv->src->id, cbtEv->dst->id, cbtEv->dst->prof, cbtEv->dst->elite, cbtEv->dst->self, cbtEv->src->team, cbtEv->dst->team);
			}

			/* remove */
			else {
				p += _snprintf_s(p, 400, _TRUNCATE, "==== cbtnotify ====\n");
				p += _snprintf_s(p, 400, _TRUNCATE, "agent removed: %s (%0llx)\n", cbtEv->src->name, cbtEv->src->id);
			}
		}

		/* target change */
		else if (cbtEv->src->elite == 1) {
			p += _snprintf_s(p, 400, _TRUNCATE, "==== cbtnotify ====\n");
			p += _snprintf_s(p, 400, _TRUNCATE, "new target: %0llx\n", cbtEv->src->id);
		}
	}

	/* combat event. skillname may be null. non-null skillname will remain static until client exit. refer to evtc notes for complete detail */
	else {

		/* default names */
		if (!cbtEv->src->name || !strlen(cbtEv->src->name)) cbtEv->src->name = (char*)"(area)";
		if (!cbtEv->dst->name || !strlen(cbtEv->dst->name)) cbtEv->dst->name = (char*)"(area)";

		/* common */
		p += _snprintf_s(p, 400, _TRUNCATE, "combatdemo: ==== cbtevent %u at %llu ====\n", cbtcount, cbtEv->ev->time);
		p += _snprintf_s(p, 400, _TRUNCATE, "source agent: %s (%0llx:%u, %lx:%lx), master: %u\n", cbtEv->src->name, cbtEv->ev->src_agent, cbtEv->ev->src_instid, cbtEv->src->prof, cbtEv->src->elite, cbtEv->ev->src_master_instid);
		if (cbtEv->ev->dst_agent) p += _snprintf_s(p, 400, _TRUNCATE, "target agent: %s (%0llx:%u, %lx:%lx)\n", cbtEv->dst->name, cbtEv->ev->dst_agent, cbtEv->ev->dst_instid, cbtEv->dst->prof, cbtEv->dst->elite);
		else p += _snprintf_s(p, 400, _TRUNCATE, "target agent: n/a\n");

		/* statechange */
		if (cbtEv->ev->is_statechange) {
			p += _snprintf_s(p, 400, _TRUNCATE, "is_statechange: %u\n", cbtEv->ev->is_statechange);
		}

		/* activation */
		else if (cbtEv->ev->is_activation) {
			p += _snprintf_s(p, 400, _TRUNCATE, "is_activation: %u\n", cbtEv->ev->is_activation);
			p += _snprintf_s(p, 400, _TRUNCATE, "skill: %s:%u\n", cbtEv->skillname, cbtEv->ev->skillid);
			p += _snprintf_s(p, 400, _TRUNCATE, "ms_expected: %d\n", cbtEv->ev->value);
		}

		/* buff remove */
		else if (cbtEv->ev->is_buffremove) {
			p += _snprintf_s(p, 400, _TRUNCATE, "is_buffremove: %u\n", cbtEv->ev->is_buffremove);
			p += _snprintf_s(p, 400, _TRUNCATE, "skill: %s:%u\n", cbtEv->skillname, cbtEv->ev->skillid);
			p += _snprintf_s(p, 400, _TRUNCATE, "ms_duration: %d\n", cbtEv->ev->value);
			p += _snprintf_s(p, 400, _TRUNCATE, "ms_intensity: %d\n", cbtEv->ev->buff_dmg);
		}

		/* buff */
		else if (cbtEv->ev->buff) {

			/* damage */
			if (cbtEv->ev->buff_dmg) {
				p += _snprintf_s(p, 400, _TRUNCATE, "is_buff: %u\n", cbtEv->ev->buff);
				p += _snprintf_s(p, 400, _TRUNCATE, "skill: %s:%u\n", cbtEv->skillname, cbtEv->ev->skillid);
				p += _snprintf_s(p, 400, _TRUNCATE, "dmg: %d\n", cbtEv->ev->buff_dmg);
				p += _snprintf_s(p, 400, _TRUNCATE, "is_shields: %u\n", cbtEv->ev->is_shields);
			}

			/* application */
			else {
				p += _snprintf_s(p, 400, _TRUNCATE, "is_buff: %u\n", cbtEv->ev->buff);
				p += _snprintf_s(p, 400, _TRUNCATE, "skill: %s:%u\n", cbtEv->skillname, cbtEv->ev->skillid);
				p += _snprintf_s(p, 400, _TRUNCATE, "raw ms: %d\n", cbtEv->ev->value);
				p += _snprintf_s(p, 400, _TRUNCATE, "overstack ms: %u\n", cbtEv->ev->overstack_value);
			}
		}

		/* strike */
		else {
			p += _snprintf_s(p, 400, _TRUNCATE, "is_buff: %u\n", cbtEv->ev->buff);
			p += _snprintf_s(p, 400, _TRUNCATE, "skill: %s:%u\n", cbtEv->skillname, cbtEv->ev->skillid);
			p += _snprintf_s(p, 400, _TRUNCATE, "dmg: %d\n", cbtEv->ev->value);
			p += _snprintf_s(p, 400, _TRUNCATE, "is_moving: %u\n", cbtEv->ev->is_moving);
			p += _snprintf_s(p, 400, _TRUNCATE, "is_ninety: %u\n", cbtEv->ev->is_ninety);
			p += _snprintf_s(p, 400, _TRUNCATE, "is_flanking: %u\n", cbtEv->ev->is_flanking);
			p += _snprintf_s(p, 400, _TRUNCATE, "is_shields: %u\n", cbtEv->ev->is_shields);
		}

		/* common */
		p += _snprintf_s(p, 400, _TRUNCATE, "iff: %u\n", cbtEv->ev->iff);
		p += _snprintf_s(p, 400, _TRUNCATE, "result: %u\n", cbtEv->ev->result);
		cbtcount += 1;
	}

	/* print */
	APIDefs->Log(ELogLevel_DEBUG, &buff[0]);
}