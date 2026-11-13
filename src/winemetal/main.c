#include "windef.h"
#include "winbase.h"
#include "wineunixlib.h"

void wmt_flight_main(DWORD reason, BOOL exiting);

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  // the flight recorder takes each ending thread's ring back for the next (WMTFlightRecord)
  if (reason == DLL_PROCESS_ATTACH && __wine_init_unix_call())
    return FALSE;
  wmt_flight_main(reason, reserved != NULL);
  return TRUE;
}

extern BOOL WINAPI DllMainCRTStartup(HANDLE hDllHandle, DWORD dwReason,
                                       LPVOID lpreserved);
