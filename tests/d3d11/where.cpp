// says where every thread of the processes of one program is: where.exe <the program's file name>. first a line a
// process, `threads: ` and the process and its threads; then a line a thread, `where: ` and the process, the thread and
// its frames innermost first, each the module and the offset in it, which the build's symbols turn into functions
// and lines. the runner starts it when a test outlives its time (tests/run.py).
// nothing here knows what a thread may wait for: the stacks say it
#include <windows.h>

#include <dbghelp.h>
#include <tlhelp32.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

// dbghelp serves one caller at a time (Microsoft Learn, DbgHelp Functions: "all DbgHelp functions ... are single
// threaded")
static std::mutex dbghelp;

struct Thread {
  HANDLE process;
  DWORD process_id, id;
  const std::vector<MODULEENTRY32> *modules;
};

// a thread stands still while its stack is read. one that never gives its context (it is in a call that the system
// cannot interrupt) keeps this reader, and its line never comes: the other threads have readers of their own. one
// the system refuses says which call refused it and why
static DWORD WINAPI
Read(void *argument) {
  Thread *read = static_cast<Thread *>(argument);
  HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, read->id);
  CONTEXT context = {};
  context.ContextFlags = CONTEXT_FULL;
  const char *refused = !thread ? "OpenThread" : SuspendThread(thread) == DWORD(-1) ? "SuspendThread"
                                             : !GetThreadContext(thread, &context) ? "GetThreadContext" : nullptr;
  if (refused) {
    printf("refused: %04lx %04lx: %s, error %lu\n", read->process_id, read->id, refused, GetLastError());
    fflush(stdout);
    return 1;
  }
  STACKFRAME64 frame = {};
  frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
#ifdef __i386__
  frame.AddrPC.Offset = context.Eip, frame.AddrFrame.Offset = context.Ebp, frame.AddrStack.Offset = context.Esp;
#elif defined(__aarch64__) && !defined(__arm64ec__)
  frame.AddrPC.Offset = context.Pc, frame.AddrFrame.Offset = context.Fp, frame.AddrStack.Offset = context.Sp;
#else
  frame.AddrPC.Offset = context.Rip, frame.AddrFrame.Offset = context.Rbp, frame.AddrStack.Offset = context.Rsp;
#endif
  char text[32];
  snprintf(text, sizeof(text), "where: %04lx %04lx:", read->process_id, read->id);
  std::string line = text;
  std::lock_guard lock(dbghelp);
  // a caller's frame is above its callee's on the stack: a walk that does not climb has lost the stack
  for (DWORD64 below = 0;
       StackWalk64(
           ImageNtHeader(GetModuleHandleW(nullptr))->FileHeader.Machine, read->process, thread, &frame, &context,
           nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr
       ) &&
       frame.AddrPC.Offset && frame.AddrStack.Offset > below;
       below = frame.AddrStack.Offset) {
    DWORD64 base = 0;
    const char *name = "";
    for (const MODULEENTRY32 &module : *read->modules)
      if (frame.AddrPC.Offset - DWORD64(module.modBaseAddr) < module.modBaseSize)
        base = DWORD64(module.modBaseAddr), name = module.szModule;
    snprintf(text, sizeof(text), "+0x%llx", frame.AddrPC.Offset - base);
    line += std::string(" ") + name + text;
  }
  ResumeThread(thread);
  puts(line.c_str());
  fflush(stdout);
  return 0;
}

int
main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  std::vector<DWORD> processes;
  HANDLE all = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS | TH32CS_SNAPTHREAD, 0);
  PROCESSENTRY32 process = {};
  process.dwSize = sizeof(process);
  for (BOOL more = Process32First(all, &process); more; more = Process32Next(all, &process))
    if (!_stricmp(process.szExeFile, argv[1]) && process.th32ProcessID != GetCurrentProcessId())
      processes.push_back(process.th32ProcessID);
  std::vector<HANDLE> readers;
  // a walk wants where the modules are and their unwind tables: their symbols are for the runner to read
  SymSetOptions(SYMOPT_DEFERRED_LOADS);
  for (DWORD id : processes) {
    HANDLE opened = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, id);
    if (!opened || !SymInitialize(opened, nullptr, TRUE)) {
      printf("refused: %04lx process: %s, error %lu\n", id, opened ? "SymInitialize" : "OpenProcess", GetLastError());
      continue;
    }
    // the process's own list of its modules names them: dbghelp names none whose symbols it has not read
    auto modules = new std::vector<MODULEENTRY32>;
    HANDLE images = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, id);
    MODULEENTRY32 module = {};
    module.dwSize = sizeof(module);
    for (BOOL more = Module32First(images, &module); more; more = Module32Next(images, &module))
      modules->push_back(module);
    THREADENTRY32 thread = {};
    thread.dwSize = sizeof(thread);
    printf("threads: %04lx:", id);
    for (BOOL more = Thread32First(all, &thread); more; more = Thread32Next(all, &thread))
      if (thread.th32OwnerProcessID == id) {
        printf(" %04lx", thread.th32ThreadID);
        readers.push_back(CreateThread(
            nullptr, 0, Read, new Thread{opened, id, thread.th32ThreadID, modules}, CREATE_SUSPENDED, nullptr
        ));
      }
    puts("");
  }
  fflush(stdout);
  for (HANDLE reader : readers)
    ResumeThread(reader);
  for (HANDLE reader : readers)
    WaitForSingleObject(reader, INFINITE);
  puts("where: end");
  return processes.empty();
}
