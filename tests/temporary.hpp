// a directory of the test's own in `temporary`. GetTempFileNameA makes a file of a name no one else holds, which the
// directory takes once the file is gone; a test running beside this one can take the name in between, and then this
// one takes another
#pragma once
#include <windows.h>

inline bool
temporary_directory(const char *temporary, const char *prefix, char *directory) {
  while (GetTempFileNameA(temporary, prefix, 0, directory) && DeleteFileA(directory))
    if (CreateDirectoryA(directory, nullptr))
      return true;
    else if (GetLastError() != ERROR_ALREADY_EXISTS)
      return false;
  return false;
}
