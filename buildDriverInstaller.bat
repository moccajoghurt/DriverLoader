@ECHO OFF
cl.exe /EHsc DriverInstaller.cpp /link user32.lib ntdll.lib Advapi32.lib Shlwapi.lib