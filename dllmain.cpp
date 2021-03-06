// dllmain.cpp : Defines the entry point for the DLL application.
#include "stdafx.h"
#include <Windows.h>
#include <ntstatus.h>
#include "dllmain.h"
#include <map>
#include <vector>
#include <cmath>
#include <TlHelp32.h>
#include <Psapi.h>
#include <Winternl.h>
#include <cstdint>
#include <string>


typedef LONG    NTSTATUS;

typedef NTSTATUS(WINAPI *pNtQIT)(HANDLE, LONG, PVOID, ULONG, PULONG);

using namespace std;

#define ThreadQuerySetWin32StartAddress 9

#define MAKEULONGLONG(ldw, hdw) ((ULONGLONG(hdw) << 32) | ((ldw) & 0xFFFFFFFF))
#define STATUS_SUCCESS    ((NTSTATUS)0x00000000L)


map<DWORD, DWORD64> GetThreadsStartAddresses(vector<DWORD> tids)
{
	map<DWORD, DWORD64> tidsStartAddresses;

	if (tids.empty())
		return tidsStartAddresses;

	for (int i(0); i < tids.size(); ++i) {
		HANDLE hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, tids[i]);
		PVOID startAddress = NULL;
		ULONG returnLength = NULL;
		NTSTATUS NtQIT = NtQueryInformationThread(hThread, (THREADINFOCLASS)ThreadQuerySetWin32StartAddress, &startAddress, sizeof(startAddress), &returnLength);
		CloseHandle(hThread);
		if (tids[i] && startAddress)
		{
			tidsStartAddresses[tids[i]] = (DWORD64)startAddress;
		}
	}

	return tidsStartAddresses;
}

map<wstring, DWORD64> GetModulesNamesAndBaseAddresses(DWORD pid)
{
	map<wstring, DWORD64> modsStartAddrs;

	if (!pid)
		return modsStartAddrs;

	HMODULE hMods[1024];
	DWORD cbNeeded;
	unsigned int i;

	HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
	if (!hProcess)
		return modsStartAddrs;

	// Get a list of all the modules in this process
	if (!EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
		CloseHandle(hProcess);
		return modsStartAddrs;
	}

	// Get each module's infos
	for (i = 0; i < (cbNeeded / sizeof(HMODULE)); i++) {
		TCHAR szModName[MAX_PATH];
		if (!GetModuleFileNameEx(hProcess, hMods[i], szModName, sizeof(szModName) / sizeof(TCHAR))) // Get the full path to the module's file
			continue;
		wstring modName = szModName;
		int pos = modName.find_last_of(L"\\");
		modName = modName.substr(pos + 1, modName.length());

		MODULEINFO modInfo;
		if (!GetModuleInformation(hProcess, hMods[i], &modInfo, sizeof(modInfo)))
			continue;

		DWORD64 baseAddr = (DWORD64)modInfo.lpBaseOfDll;
		modsStartAddrs[modName] = baseAddr;
	}

	// Release the handle to the process
	CloseHandle(hProcess);
	return modsStartAddrs;
}


vector<DWORD> GetTIDChronologically(DWORD pid)
{
	map<ULONGLONG, DWORD> tidsWithStartTimes;
	vector<DWORD> tids;

	if (pid == NULL)
		return tids;

	DWORD dwMainThreadID = NULL;
	ULONGLONG ullMinCreateTime = MAXULONGLONG;
	HANDLE hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (hThreadSnap != INVALID_HANDLE_VALUE) {
		THREADENTRY32 th32;
		th32.dwSize = sizeof(THREADENTRY32);
		BOOL bOK = TRUE;
		for (bOK = Thread32First(hThreadSnap, &th32); bOK; bOK = Thread32Next(hThreadSnap, &th32)) {
			if (th32.th32OwnerProcessID == pid) {
				HANDLE hThread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, th32.th32ThreadID);
				if (hThread) {
					FILETIME afTimes[4] = { 0 };
					if (GetThreadTimes(hThread, &afTimes[0], &afTimes[1], &afTimes[2], &afTimes[3])) {
						ULONGLONG ullTest = MAKEULONGLONG(afTimes[0].dwLowDateTime, afTimes[0].dwHighDateTime);
						tidsWithStartTimes[ullTest] = th32.th32ThreadID;
					}
					CloseHandle(hThread);
				}
			}
		}
		CloseHandle(hThreadSnap);
	}

	for (auto const& thread : tidsWithStartTimes) // maps are natively ordered by key
		tids.push_back(thread.second);

	return tids;
}

map<DWORD, wstring> GetTIDsModuleStartAddr(DWORD pid)
{
	map<DWORD, wstring> tidsStartModule;

	map<wstring, DWORD64> modsStartAddrs = GetModulesNamesAndBaseAddresses(pid);
	if (modsStartAddrs.empty())
		return tidsStartModule;

	vector<DWORD> tids = GetTIDChronologically(pid);
	if (tids.empty())
		return tidsStartModule;

	map<DWORD, DWORD64> tidsStartAddresses = GetThreadsStartAddresses(tids);
	if (tidsStartAddresses.empty())
		return tidsStartModule;


	for (auto const& thisTid : tidsStartAddresses) {
		DWORD tid = thisTid.first;
		DWORD64 startAddress = thisTid.second;
		DWORD64 nearestModuleAtLowerAddrBase = 0;
		wstring nearestModuleAtLowerAddrName = L"";
		for (auto const& thisModule : modsStartAddrs) 
		{
			wstring moduleName = thisModule.first;
			DWORD64 moduleBase = thisModule.second;
			if (moduleBase > startAddress)
				continue;
			if (moduleBase > nearestModuleAtLowerAddrBase) {
				nearestModuleAtLowerAddrBase = moduleBase;
				nearestModuleAtLowerAddrName = moduleName;
			}
		}
		if (nearestModuleAtLowerAddrBase > 0 && nearestModuleAtLowerAddrName != L"")
		{
			tidsStartModule[tid] = nearestModuleAtLowerAddrName;
			
		}
	}


	return tidsStartModule;
}

vector<DWORD> GetThreadsOfPID(DWORD dwOwnerPID)
{
	vector<DWORD> threadIDs;
	HANDLE hThreadSnap = INVALID_HANDLE_VALUE;
	THREADENTRY32 te32;

	hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (hThreadSnap == INVALID_HANDLE_VALUE)
		return threadIDs;
	te32.dwSize = sizeof(THREADENTRY32);

	if (!Thread32First(hThreadSnap, &te32)) {
		CloseHandle(hThreadSnap);
		return threadIDs;
	}

	do {
		if (te32.th32OwnerProcessID == dwOwnerPID)
			threadIDs.push_back(te32.th32ThreadID);
	} while (Thread32Next(hThreadSnap, &te32));


	return threadIDs;
}


DWORD WINAPI GetThreadStartAddress(HANDLE hThread)
{
	NTSTATUS ntStatus;
	HANDLE hDupHandle;
	DWORD dwStartAddress;

	pNtQIT NtQueryInformationThread = (pNtQIT)GetProcAddress(GetModuleHandle(L"ntdll.dll"), "NtQueryInformationThread");

	if (NtQueryInformationThread == NULL)
		return 0;

	HANDLE hCurrentProcess = GetCurrentProcess();
	if (!DuplicateHandle(hCurrentProcess, hThread, hCurrentProcess, &hDupHandle, THREAD_QUERY_INFORMATION, FALSE, 0)) {
		SetLastError(ERROR_ACCESS_DENIED);

		return 0;
	}

	ntStatus = NtQueryInformationThread(hDupHandle, ThreadQuerySetWin32StartAddress, &dwStartAddress, sizeof(DWORD), NULL);
	CloseHandle(hDupHandle);
	if (ntStatus != STATUS_SUCCESS)
		return 0;

	return dwStartAddress;

}

DWORD GetThreadsStartAddressesDWORD(DWORD tids)
{
	DWORD tidsStartAddresses;


	HANDLE hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, tids);
	PVOID startAddress = NULL;
	ULONG returnLength = NULL;
	NTSTATUS NtQIT = NtQueryInformationThread(hThread, (THREADINFOCLASS)ThreadQuerySetWin32StartAddress, &startAddress, sizeof(startAddress), &returnLength);
	CloseHandle(hThread);
	if (tids && startAddress)
	{
		tidsStartAddresses = (DWORD64)startAddress;
	}


	return tidsStartAddresses;
}

bool verify_thread_start_address(HANDLE thread_handle)
{
	if (!thread_handle || thread_handle == INVALID_HANDLE_VALUE)
		return false;

	constexpr const auto thread_query_start_address = static_cast<THREADINFOCLASS>(9);

	if (!thread_handle || thread_handle == INVALID_HANDLE_VALUE)
		return false; //this shouldn't happen

	uintptr_t start_address = 0;
	uint32_t return_length = 0;

	//cast return_length to a PULONG if you have problems
	const auto status = NtQueryInformationThread(thread_handle, thread_query_start_address, reinterpret_cast<void**>(&start_address), sizeof(start_address), (PULONG)&return_length);

	if (!NT_SUCCESS(status))
	{
		//error handle...
	}

	//now make sure that start_address is within a loaded module

	return true;
}


int GetNumberOfModules()
{
	HMODULE hMods[1024];
	HANDLE hProcess;
	DWORD bNeeded;
	int i;
	int Total = 0;
	// Print the process identifier.


	// Get a handle to the process.

	hProcess = OpenProcess(PROCESS_QUERY_INFORMATION |
		PROCESS_VM_READ,
		FALSE, GetCurrentProcessId());
	if (NULL == hProcess)
		return 1;

	// Get a list of all the modules in this process.

	if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &bNeeded))
	{
		
		/*
		for (i = 0; i < (cbNeeded / sizeof(HMODULE)); i++)
		{
			const int NumberOfModules = (cbNeeded / sizeof(HMODULE));
			TCHAR szModName[MAX_PATH];
			// Get the full path to the module's file.

			if (GetModuleFileNameEx(hProcess, hMods[i], szModName,
				sizeof(szModName) / sizeof(TCHAR)))
			{
				// Print the module name and handle value.

				
				
			}
		}*/
		return (bNeeded / sizeof(HMODULE));
	}
	else
	{
		TerminateProcess(hProcess, 0);
		exit(1);
	}
	CloseHandle(hProcess);
	return Total;
	// Release the handle to the process.



}

DWORD ThreadID = 0;
DWORD WINAPI Start(LPVOID)
{
	int StartingModules = GetNumberOfModules();
	HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION |
		PROCESS_VM_READ,
		FALSE, GetCurrentProcessId());

	
	while (1)
	{
		if (StartingModules != GetNumberOfModules())
		{
			TerminateProcess(hProcess, 0);
			exit(1);
		}


		Sleep(10);
	}
}

BOOL APIENTRY DllMain(HMODULE hModule,
	DWORD  ul_reason_for_call,
	LPVOID lpReserved
)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		CreateThread(NULL, NULL, &Start, NULL, NULL, NULL);
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

