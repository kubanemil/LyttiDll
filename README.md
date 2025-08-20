Compile with:
```bash
cl /nologo /W4 /O2 /EHsc /DUNICODE /D_UNICODE /std:c++17 /MD /LD ExtDll.cpp /Fe:ExtDll.dll /link /DEF:ExtDll.def winhttp.lib
```