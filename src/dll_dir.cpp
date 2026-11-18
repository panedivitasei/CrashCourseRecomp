// dll_dir.cpp: delay-load hook that loads the runtime's libraries from bin\ under the exe, falling through to the
// usual search in a build tree, which has no bin\ folder.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <delayimp.h>

#include <string>

namespace {

FARPROC WINAPI FromBin(unsigned reason, DelayLoadInfo* info) {
  if (reason != dliNotePreLoadLibrary || !info || !info->szDll) return nullptr;
  wchar_t exe[MAX_PATH];
  std::wstring path(exe, GetModuleFileNameW(nullptr, exe, MAX_PATH));
  path.resize(path.find_last_of(L"\\/") + 1);
  path += L"bin\\";
  for (const char* c = info->szDll; *c; ++c) path += wchar_t(*c);
  // The altered search path resolves the library's own imports from bin\ as well.
  return reinterpret_cast<FARPROC>(LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
}

}  // namespace

// Constant-initialized: in place before any static initializer reaches into the runtime.
extern "C" const PfnDliHook __pfnDliNotifyHook2 = FromBin;

#endif
