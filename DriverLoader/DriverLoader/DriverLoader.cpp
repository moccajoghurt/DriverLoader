#pragma once
#include <windows.h>
#include <Shlwapi.h>
// #include <Subauth.h>
#include <string>
#include <filesystem>
#include <fstream>
#include <iostream>
#include "NtDefines.h"

#define STATUS_SUCCESS 0

using namespace std;

wstring GetDriverPath() {
    wchar_t systemDirectory[2048];
    GetSystemDirectoryW(systemDirectory, 2048);

    wstring driverPath = systemDirectory;
    driverPath += L"\\drivers\\";

    return driverPath;
}

BOOL MoveFileToDriversFolder(const wchar_t* fileName, const wchar_t* filePath) {
    wstring driverDestinationPath = GetDriverPath() + fileName;
    DeleteFileW(driverDestinationPath.c_str()); // delete existing
    ifstream src(filePath, ios::binary);
    ofstream dest(driverDestinationPath, ios::binary | ios::out);
    dest << src.rdbuf();
    if (!dest.good()) {
        return FALSE;
    }
    src.close();
    dest.close();
    return TRUE;
}

NTSTATUS RemoveDriverFromRegistry(const wchar_t* driverName) {
    NTSTATUS status = STATUS_SUCCESS;

    wstring RegistryPath = wstring(L"System\\CurrentControlSet\\Services\\") + driverName;

    status = RegDeleteKeyW(HKEY_LOCAL_MACHINE, RegistryPath.c_str());
    if (!status || status == ERROR_FILE_NOT_FOUND) {
        return STATUS_SUCCESS;
    }

    status = SHDeleteKeyW(HKEY_LOCAL_MACHINE, RegistryPath.c_str());
    if (!status || status == ERROR_FILE_NOT_FOUND) {
        return STATUS_SUCCESS;
    }

    status = RegDeleteKeyW(HKEY_LOCAL_MACHINE, RegistryPath.c_str());
    if (!status || status == ERROR_FILE_NOT_FOUND) {
        return STATUS_SUCCESS;
    }

    return status;
}

NTSTATUS AddServiceToRegistry(const wchar_t* driverName) {
    NTSTATUS status = STATUS_SUCCESS;

    wstring registryPath = wstring(L"System\\CurrentControlSet\\Services\\") + driverName;

    RemoveDriverFromRegistry(driverName);

    HKEY key;
    status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, registryPath.c_str(), 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, 0);

    if (status != ERROR_SUCCESS) {
        return status;
    }
    wstring driverPath = wstring(L"\\SystemRoot\\System32\\drivers\\") + driverName + L".sys";
    DWORD value = 1;
    status |= RegSetValueExW(key, L"ImagePath", 0, REG_EXPAND_SZ, (PBYTE)driverPath.c_str(), driverPath.size() * sizeof(wchar_t));
    status |= RegSetValueExW(key, L"Type", 0, REG_DWORD, (PBYTE)&value, sizeof(DWORD));
    status |= RegSetValueExW(key, L"ErrorControl", 0, REG_DWORD, (PBYTE)&value, sizeof(DWORD));
    value = 3;
    status |= RegSetValueExW(key, L"Start", 0, REG_DWORD, (PBYTE)&value, sizeof(DWORD));

    if (status != ERROR_SUCCESS) {
        RegCloseKey(key);
        RemoveDriverFromRegistry(driverName);
        return status;
    }

    RegCloseKey(key);
    return STATUS_SUCCESS;
}

NTSTATUS TryOpenServiceKey(const wchar_t* driverName) {
    wstring registryPath = wstring(L"System\\CurrentControlSet\\Services\\") + driverName;
    HKEY key;
    NTSTATUS result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, registryPath.c_str(), 0, KEY_ALL_ACCESS, &key);
    RegCloseKey(key);
    return result;
}

NTSTATUS UnloadDriver(const wchar_t* driverName) {
    BOOLEAN alreadyEnabled = FALSE;
    if (RtlAdjustPrivilege(SeLoadDriverPrivilege, 1ull, AdjustCurrentProcess, &alreadyEnabled) != STATUS_SUCCESS && !alreadyEnabled) {
        return FALSE;
    }

    if (TryOpenServiceKey(driverName) == 2) {
        AddServiceToRegistry(driverName);
    }

    wstring sourceRegistry = wstring(L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\") + driverName;

    UNICODE_STRING sourceRegistryUnicode = { 0 };
    sourceRegistryUnicode.Buffer = (wchar_t*)sourceRegistry.c_str();
    sourceRegistryUnicode.Length = (USHORT)(sourceRegistry.size()) * 2;
    sourceRegistryUnicode.MaximumLength = (USHORT)(sourceRegistry.size() + 1) * 2;

    NTSTATUS status;
    status = NtUnloadDriver(&sourceRegistryUnicode);

    printf("[+] NtUnloadDriver(%ls) returned %08x\n", sourceRegistry.c_str(), status);

    RemoveDriverFromRegistry(driverName);

    return status;

}


BOOL LoadDriver(wstring driverName) {

    BOOLEAN alreadyEnabled = FALSE;
    if (RtlAdjustPrivilege(SeLoadDriverPrivilege, 1ull, AdjustCurrentProcess, &alreadyEnabled) != STATUS_SUCCESS && !alreadyEnabled) {
        cout << "[-] LoadDriver::RtlAdjustPrivilege failed" << endl;
        return FALSE;
    }

    if (AddServiceToRegistry(driverName.c_str()) != STATUS_SUCCESS) {
        cout << "[-] LoadDriver::AddServiceToRegistry failed" << endl;
        return FALSE;
    }

    wstring sourceRegistry = wstring(L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\") + driverName;

    UNICODE_STRING sourceRegistryUnicode = { 0 };
    sourceRegistryUnicode.Buffer = (wchar_t*)sourceRegistry.c_str();
    sourceRegistryUnicode.Length = (USHORT)(sourceRegistry.size()) * 2;
    sourceRegistryUnicode.MaximumLength = (USHORT)(sourceRegistry.size() + 1) * 2;

    NTSTATUS status = NtLoadDriver(&sourceRegistryUnicode);
    if (status != STATUS_SUCCESS) {
        cout << "[-] NtLoadDriver() failed." << endl;
        printf("[-] NtLoadDriver(%ls) returned %08x\n", sourceRegistry.c_str(), status);
        UnloadDriver(driverName.c_str());
        RemoveDriverFromRegistry(driverName.c_str());
        return FALSE;
    }
    return TRUE;
}

HANDLE OpenDevice(string driverName) {
    char completeDeviceName[128];
    sprintf_s(completeDeviceName, "\\\\.\\%s", driverName.data());

    HANDLE deviceHandle = CreateFileA(
        completeDeviceName,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (deviceHandle == INVALID_HANDLE_VALUE) {
        deviceHandle = 0;
    }
    // printf("[+] CreateFileA(%s) returned %p\n", completeDeviceName, deviceHandle);
    return deviceHandle;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        cout << "[+] Usage DriverInstaller.exe DriverName" << endl;
        wcout << L"[+] Driver will be copied to: " << GetDriverPath() << endl;
        system("PAUSE");
        return 0;
    }

    if (!PathFileExistsA(argv[1])) {
        cout << "[-] File does not exist: " << argv[1] << endl;
        system("PAUSE");
        return 0;
    }
    
    char drive[_MAX_PATH];
    char directory[_MAX_DIR];
    char filename[_MAX_FNAME];
    char extension[_MAX_EXT];

    _splitpath_s(argv[1], drive, directory, filename, extension);

    if (string(extension).find(".sys") == string::npos) {
        cout << "[-] File is not a .sys file" << endl;
        system("PAUSE");
        return 0;
    }

    string driverName = string(filename) + string(extension);
    wstring driverNameW(driverName.begin(), driverName.end());

    wstring driverNameWithoutFileEnding = driverNameW.substr(0, driverNameW.size() - 4);
    UnloadDriver(driverNameWithoutFileEnding.c_str());

    string driverPath(argv[1]);
    wstring driverPathW(driverPath.begin(), driverPath.end());
    if (!MoveFileToDriversFolder(driverNameW.c_str(), driverPathW.c_str())) {
        cout << "[-] Failed to copy driver file to drivers folder" << endl;
        system("PAUSE");
        return 0;
    }

    wstring pdbName = driverNameWithoutFileEnding + wstring(L".pdb");
    wstring pdbPath = driverPathW.substr(0, driverPathW.size() - 4) + wstring(L".pdb");
    if (!MoveFileToDriversFolder(pdbName.c_str(), pdbPath.c_str())) {
        cout << "[-] Failed to copy pdb file to drivers folder. No pdb provided? Pdb must reside in same folder as .sys file." << endl;
    }

    wcout << "[+] Registering Driver: " << driverNameW << endl;
    if (LoadDriver(driverNameWithoutFileEnding)) {
        cout << "[+] Successfully loaded driver" << endl;
    }
    else {
        cout << "[-] Loading driver failed" << endl;
    }

    system("PAUSE");
    return 1;
}