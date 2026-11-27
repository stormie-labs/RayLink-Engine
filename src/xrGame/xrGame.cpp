////////////////////////////////////////////////////////////////////////////
//	Module 		: xrGame.cpp
//	Created 	: 07.01.2001
//  Modified 	: 27.05.2004
//	Author		: Aleksandr Maksimchuk and Oles' Shyshkovtsov
//	Description : Defines the entry point for the DLL application.
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "object_factory.h"
#include "ui/xrUIXmlParser.h"
#include "xr_level_controller.h"
#include "profiler.h"

//AVO: lua re-org
#ifdef USE_LUAJIT_ONE //defined in project props
#pragma comment(lib, "LuaJIT-1.1.8.lib")
#else
#pragma comment(lib, "lua51.lib" )
#endif
//#include "lua/library_linkage.h"
//-AVO

//#pragma comment(lib,"ode.lib")
//#pragma comment(lib,"xrEngine.lib")

extern "C" {
DLL_API DLL_Pure* __cdecl xrFactory_Create(CLASS_ID clsid)
{
	DLL_Pure* object = object_factory().client_object(clsid);
	// coop: the null check stands in Release too, where the stock one is DEBUG only.
	// Only a script class (object_item_script: its Lua creator failed) gives nothing; every caller gets nullptr and a line.
	if (!object)
	{
		string16 name;
		CLSID2TEXT(clsid, name);
		Msg("! xrFactory_Create: the class [%s] gave no object (its Lua creator failed, see the Lua error above)", name);
		return (nullptr);
	}
	object->CLS_ID = clsid;
	return (object);
}

DLL_API void __cdecl xrFactory_Destroy(DLL_Pure* O)
{
	xr_delete(O);
}
};

void CCC_RegisterCommands();
void setup_luabind_allocator();

//BOOL APIENTRY DllMain(HANDLE hModule, u32 ul_reason_for_call, LPVOID lpReserved)
BOOL DllMainXrGame(HANDLE hModule, u32 ul_reason_for_call, LPVOID lpReserved)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		{
			// register console commands
			CCC_RegisterCommands();
			// keyboard binding
			CCC_RegisterInput();

			setup_luabind_allocator();
#ifdef DEBUG
			g_profiler			= xr_new<CProfiler>();
#endif
			break;
		}

	case DLL_PROCESS_DETACH:
		{
			break;
		}
	}
	return (TRUE);
}
