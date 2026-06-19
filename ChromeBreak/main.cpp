#include <iostream>
#include <Windows.h>
#include <winternl.h>
#include <vector>
#include <algorithm>
#include <psapi.h>
#include <thread>
#pragma comment(lib, "psapi.lib")

typedef NTSTATUS(NTAPI* NtQueryInformationProcess_t)(
	HANDLE,
	PROCESSINFOCLASS,
	PVOID,
	ULONG,
	PULONG
	);

BOOL CALLBACK HideChrome(HWND hwnd, LPARAM lParam)
{
	DWORD pid;

	GetWindowThreadProcessId(hwnd, &pid);

	if (pid == (DWORD)lParam)
	{
		
		
		ShowWindow(hwnd, SW_HIDE);
		return TRUE;
	}
	return TRUE;
	
}

HANDLE CreateChrome(std::string ChromePath)
{
	
	std::string ChromeStartup =
		"\"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe\" --headless";
	

	STARTUPINFOA si = { sizeof(si) };
	PROCESS_INFORMATION pi{};
	if (!CreateProcessA(
		NULL,
		ChromePath.data(),
		NULL,
		NULL,
		FALSE,
		0,
		NULL,
		NULL,
		&si,
		&pi
	))
	{
		return INVALID_HANDLE_VALUE;
	}
	return pi.hProcess;
}


PVOID GetModuleBaseInProcess(HANDLE hProcess, const std::string& modName)
{
	HMODULE mods[1024];
	DWORD needed = 0;

	if (!EnumProcessModules(hProcess, mods, sizeof(mods), &needed))
		return nullptr;

	for (DWORD i = 0; i < needed / sizeof(HMODULE); i++)
	{
		char name[MAX_PATH]{};
		GetModuleFileNameExA(hProcess, mods[i], name, MAX_PATH);
		std::string s(name);
		if (s.find(modName) != std::string::npos)
			return (PVOID)mods[i];
	}
	return nullptr;
}

int main()
{
	std::string ChromePath = "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe";
	HANDLE hProcess = CreateChrome(ChromePath);
	DWORD pid = GetProcessId(hProcess);
	Sleep(2000);
	EnumWindows(HideChrome, (LPARAM)pid);
	if (hProcess == INVALID_HANDLE_VALUE)
	{
		printf("Fucking Chrome didn't start!!");
		return 1;
	}
	printf("Chrome started!\n");

	auto NtQueryInformationProcess =
		(NtQueryInformationProcess_t)GetProcAddress(
			GetModuleHandleA("ntdll.dll"),
			"NtQueryInformationProcess"
		);

	PROCESS_BASIC_INFORMATION pbi;
	NtQueryInformationProcess(
		hProcess,
		ProcessBasicInformation,
		&pbi,
		sizeof(pbi),
		nullptr
	);
	/*
	PVOID imagebase = nullptr;

	ReadProcessMemory(
		hProcess,
		(BYTE*)pbi.PebBaseAddress + 0x10,
		&imagebase,
		sizeof(imagebase),
		NULL
	);
	*/
	Sleep(2000);
	PVOID imagebase = GetModuleBaseInProcess(hProcess, "chrome.dll");

	if (!imagebase)
	{
		std::cout << "Failed to locate chrome.dll" << std::endl;
	}

	std::cout << "Found image base address of chrome.dll: 0x" << imagebase << std::endl;

	IMAGE_DOS_HEADER dosHeader;
	ReadProcessMemory(
		hProcess,
		(LPCVOID)imagebase,
		&dosHeader,
		sizeof(dosHeader),
		NULL
	);

	if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE)
	{
		std::cout << "Failed to validate DoS Signature" << std::endl;
		return 1;
	}

	std::cout << "Validated DoS Signature" << std::endl;

	IMAGE_NT_HEADERS ntHeader;

	!ReadProcessMemory(
		hProcess,
		(BYTE*)imagebase + dosHeader.e_lfanew,
		&ntHeader,
		sizeof(ntHeader),
		NULL
	);
	if (ntHeader.Signature != IMAGE_NT_SIGNATURE)
	{
		std::cout << "Failed to validate Nt Signature" << std::endl;
		return 1;
	}
	std::cout << "Validated Nt Signature!" << std::endl;

	IMAGE_SECTION_HEADER sections[96];

	LPVOID sectionHeadersAddr =
		(BYTE*)imagebase +
		dosHeader.e_lfanew +
		sizeof(IMAGE_NT_HEADERS);

	if (!ReadProcessMemory(
		hProcess,
		sectionHeadersAddr,
		sections,
		sizeof(IMAGE_SECTION_HEADER) * ntHeader.FileHeader.NumberOfSections,
		nullptr))
	{
		std::cout << "Failed to read section headers" << std::endl;
		return 1;
	}

	PVOID textBase = nullptr;
	DWORD textSize = 0;

	PVOID rdataBase = nullptr;
	DWORD rdataSize = 0;

	for (WORD i = 0; i < ntHeader.FileHeader.NumberOfSections; i++)
	{
		char name[9]{};
		memcpy(name, sections[i].Name, 8);

		if (strcmp(name, ".text") == 0)
		{
			textBase =
				(BYTE*)imagebase + sections[i].VirtualAddress;

			textSize = sections[i].Misc.VirtualSize;
		}

		if (strcmp(name, ".rdata") == 0)
		{
			rdataBase =
				(BYTE*)imagebase + sections[i].VirtualAddress;

			rdataSize = sections[i].Misc.VirtualSize;
		}
	}
	std::cout << "rdataBase: 0x" << rdataBase << " Size: " << rdataSize << std::endl;
	std::cout << "textbase: 0x" << textBase << " Size: " << textSize << std::endl;

	std::vector<uint8_t> buffer(rdataSize);

	ReadProcessMemory(
		hProcess,
		rdataBase,
		buffer.data(),
		rdataSize,
		nullptr
	);

	if (buffer.empty())
	{
		std::cout << "Failed to Read rdata section into buffer!" << std::endl;
		CloseHandle(hProcess);
		return 1;
	}

	std::vector<uint8_t> buffer_text(textSize);

	ReadProcessMemory(
		hProcess,
		textBase,
		buffer_text.data(),
		textSize,
		nullptr
	);

	if (buffer_text.empty())
	{
		std::cout << "Failed to read text section into buffer!" << std::endl;
		CloseHandle(hProcess);
		return 1;
	}

	bool found = false;

	std::string target = "OSCrypt.AppBoundProvider.Decrypt.ResultCode";

	auto it = std::search(
		buffer.begin(),
		buffer.end(),
		target.begin(),
		target.end()
	);

	if (it != buffer.end())
	{
		size_t offset = it - buffer.begin();
		std::cout << "[rdata] Found at RVA offset 0x" << offset << std::endl;
		found = true;
		SuspendThread(hProcess);
	}
	else
	{
		std::cout << "Didn't find string in rdata section. Searching text section now" << std::endl;
		return 1;
	}
	auto it_text = std::search(
		buffer_text.begin(),
		buffer_text.end(),
		target.begin(),
		target.end()
	);
	if (found == false)
	{
		if (it_text != buffer_text.end())
		{
			size_t offset = it - buffer.begin();
			std::cout << "[text] Found at RVA offset 0x" << offset << std::endl;
			SuspendThread(hProcess);
		}
		else
		{
			std::cout << "Didn't find string in .text section..." << std::endl;
			return 1;
		}
	}
	std::cout << "[+]Suspended Chrome process.";
	CloseHandle(hProcess);
	return 0;
}