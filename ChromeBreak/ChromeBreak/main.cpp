#include <iostream>
#include <Windows.h>
#include <winternl.h>
#include <vector>
#include <algorithm>
#include <psapi.h>
#include <thread>
#include <Zydis/Zydis.h>
#include <tlhelp32.h>
#pragma comment(lib, "psapi.lib")


struct LeaHit {
	uintptr_t instructionRVA;
	uintptr_t targetRVA;
};


typedef NTSTATUS(NTAPI* NtQueryInformationProcess_t)(
	HANDLE,
	PROCESSINFOCLASS,
	PVOID,
	ULONG,
	PULONG
	);
std::vector<LeaHit> FindLeaReferencingRVA(
	const uint8_t* moduleBuffer,   // local copy of the module's memory
	size_t bufferSize,
	uintptr_t moduleBaseRVA,       // usually 0 if buffer starts at module base
	uintptr_t targetRVA)
{
	std::vector<LeaHit> hits;

	ZydisDecoder decoder;
	ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

	ZydisFormatter formatter;
	ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);

	size_t offset = 0;
	ZydisDecodedInstruction instr;
	ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

	while (offset < bufferSize) {
		ZyanStatus status = ZydisDecoderDecodeFull(
			&decoder,
			moduleBuffer + offset,
			bufferSize - offset,
			&instr,
			operands
		);

		if (!ZYAN_SUCCESS(status)) {
			offset++; // skip a byte and resync
			continue;
		}

		if (instr.mnemonic == ZYDIS_MNEMONIC_LEA) {
			// operands[1] is the memory operand for "lea reg, [mem]"
			if (operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
				operands[1].mem.base == ZYDIS_REGISTER_RIP)
			{
				uintptr_t instrRVA = moduleBaseRVA + offset;
				uintptr_t nextInstrRVA = instrRVA + instr.length;
				uintptr_t targetAddrRVA = nextInstrRVA + operands[1].mem.disp.value;

				if (targetAddrRVA == targetRVA) {
					hits.push_back({ instrRVA, targetAddrRVA });
				}
			}
		}

		offset += instr.length;
	}

	return hits;
}

void KillEveryChrome()
{
	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	PROCESSENTRY32 pe;
	pe.dwSize = sizeof(PROCESSENTRY32);

	if (Process32First(hSnapshot, &pe))
	{
		do
		{
			if (wcscmp(pe.szExeFile, L"chrome.exe") == 0)
			{
				HANDLE hChrome = OpenProcess(DBG_TERMINATE_PROCESS, FALSE, pe.th32ProcessID);
				TerminateProcess(hChrome, 0);
			}
		} while (Process32Next(hSnapshot, &pe));
	}
}

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

bool FinalBreak(HANDLE hProcess, uintptr_t addr)
{
	DWORD oldproect;

	BYTE int3 = 0xCC;
	SIZE_T written = 0;

	DWORD oldProtect;

	if (!VirtualProtectEx(hProcess, (void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect))
	{
		std::cout << "Failed to set new virtual protect" << std::endl;
		return false;
	}

	if (!WriteProcessMemory(hProcess, (void*)addr, &int3, 1, &written))
		return false;

	VirtualProtectEx(hProcess, (void*)addr, 1, oldProtect, &oldProtect);

	return written == 1;
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
	KillEveryChrome();
	Sleep(2000);
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

	ReadProcessMemory(
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
	DWORD textRVA = 0;
	DWORD textSize = 0;

	PVOID rdataBase = nullptr;
	DWORD rdataRVA = 0;
	DWORD rdataSize = 0;

	for (WORD i = 0; i < ntHeader.FileHeader.NumberOfSections; i++)
	{
		char name[9]{};
		memcpy(name, sections[i].Name, 8);

		if (strcmp(name, ".text") == 0)
		{
			textRVA = sections[i].VirtualAddress;
			textBase =
				(BYTE*)imagebase + sections[i].VirtualAddress;

			textSize = sections[i].Misc.VirtualSize;
		}

		if (strcmp(name, ".rdata") == 0)
		{
			rdataRVA = sections[i].VirtualAddress;
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
		uintptr_t targetRVA = rdataRVA + offset;

		std::cout << "[rdata] Found at RVA offset 0x" << std::hex << offset << std::endl;
		std::cout << "[rdata] Target RVA: " << std::hex << targetRVA << std::endl;
		found = true;

		auto xrefs = FindLeaReferencingRVA(
			buffer_text.data(),
			buffer_text.size(),
			textRVA,
			targetRVA
		);

		if (xrefs.empty())
		{
			std::cout << "Didn't find LEA instruction" << std::endl;

		}
		else
		{
			for (auto& hit : xrefs)
			{
				std::cout << "LEA at RVA 0x" << std::hex << hit.instructionRVA
					<< " -> Target RVA 0x" << std::hex << hit.targetRVA << std::endl;

				uintptr_t addr = (uintptr_t)imagebase + hit.targetRVA;

				DWORD id = GetProcessId(hProcess);
				DebugActiveProcess(id);
				bool finished = FinalBreak(hProcess, addr);
				if (finished)
				{
					DEBUG_EVENT ev;
					while (true)
					{
						WaitForDebugEvent(&ev, INFINITE);

						if (ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT)
						{
							if (ev.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT)
							{
								std::cout << "BREAKPOINT HIT!" << std::endl;
							}
						}
						ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
					}
				}
				else
				{
					std::cout << "Failed to set breakpoint, not entering debug loop" << std::endl;
					return 1;
				}
			}
		}

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