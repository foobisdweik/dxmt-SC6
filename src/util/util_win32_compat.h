#pragma once

#ifndef _WIN32

#include <windows.h>

#include "log/log.hpp"

#define THREAD_PRIORITY_TIME_CRITICAL 15

inline HANDLE GetCurrentProcess() {
  mullion::Logger::warn("GetCurrentProcess not implemented.");
  return nullptr;
}

inline HANDLE GetCurrentThread() {
  mullion::Logger::warn("GetCurrentThread not implemented.");
  return nullptr;
}

inline DWORD GetCurrentProcessId() {
  mullion::Logger::warn("GetCurrentProcessId not implemented.");
  return 0;
}

inline BOOL ProcessIdToSessionId(DWORD pid, DWORD *id) {
  mullion::Logger::warn("ProcessIdToSessionId not implemented.");
  *id = 0;
  return FALSE;
}

inline BOOL SetThreadPriority(HANDLE hThread, int nPriority) {
  mullion::Logger::warn("SetThreadPriority not implemented.");
  return FALSE;
}

inline HANDLE GetModuleHandle(LPCSTR lpModuleName) {
  mullion::Logger::warn("GetModuleHandle not implemented.");
  return nullptr;
}

inline void ExitThread(DWORD dwExitCode) {
  mullion::Logger::warn("ExitThread not implemented.");
}

#define WM_SIZE 0

inline LONG SendMessage(HWND hWnd, UINT Msg, UINT_PTR wParam, LONG_PTR lParam) {
  mullion::Logger::warn("SendMessage not implemented.");
  return 0;
}

inline DWORD GetProcessId(HANDLE Process) {
  mullion::Logger::warn("GetProcessId not implemented.");
  return 0;
}

inline DWORD GetWindowThreadProcessId(HWND hWnd, LPDWORD lpdwProcessId) {
  mullion::Logger::warn("GetWindowThreadProcessId not implemented.");
  *lpdwProcessId = 0;
  return 0;
}

inline DWORD MAKELONG(WORD a, WORD b) {
  return (((DWORD)a) << 16) | ((DWORD)b);
}

typedef SECURITY_ATTRIBUTES *LPSECURITY_ATTRIBUTES;

inline HANDLE CreateSemaphore(LPSECURITY_ATTRIBUTES lpSemaphoreAttributes,
                              LONG                  lInitialCount,
                              LONG                  lMaximumCount,
                              LPCSTR                lpName) {
  mullion::Logger::warn("CreateSemaphore not implemented.");
  return nullptr;
}

inline BOOL ReleaseSemaphore(HANDLE hSemaphore,
                             LONG   lReleaseCount,
                             LPLONG lpPreviousCount) {
  mullion::Logger::warn("ReleaseSemaphore not implemented.");
  return FALSE;
}

inline BOOL CloseHandle(HANDLE hObject) {
  mullion::Logger::warn("CloseHandle not implemented.");
  return FALSE;
}

inline BOOL DuplicateHandle(HANDLE   hSourceProcessHandle,
                            HANDLE   hSourceHandle,
                            HANDLE   hTargetProcessHandle,
                            LPHANDLE lpTargetHandle,
                            DWORD    dwDesiredAccess,
                            BOOL     bInheritHandle,
                            DWORD    dwOptions)
{
  mullion::Logger::warn("DuplicateHandle not implemented.");
  return FALSE;
}


#define ARRAYSIZE(a) (sizeof(a)/sizeof(*(a)))

#endif