del cores.exe
del cores.pdb
cl /W4 /nologo cores.c /I.\ /Ox /Qpar /O2 /Oi /Ob2 /EHac /Zi /Gy /link ntdll.lib user32.lib gdi32.lib /OPT:REF /subsystem:windows


