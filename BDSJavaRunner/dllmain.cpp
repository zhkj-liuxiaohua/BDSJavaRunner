// dllmain.cpp : 定义 DLL 应用程序的入口点。
#include "pch.h"

// 当使用预编译的头时，需要使用此源文件，编译才能成功。

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "./Detours/lib.X64/detours.lib")
#pragma comment(lib, "./json/lib/jsoncpp.lib")

void init();
void exit();

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        init();
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        exit();
        break;
    }
    return TRUE;
}

