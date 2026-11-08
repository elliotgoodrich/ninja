// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "disk_interface.h"

#include <algorithm>
#include <string_view>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>  // _mkdir
#include <windows.h>

#include <sstream>
#else
#include <unistd.h>
#endif

#include "metrics.h"
#include "util.h"

using namespace std;

namespace {

/// Return the directory part of |path| without its trailing separators, or an
/// empty view if |path| has no directory part.  The result points into
/// |path|.
string_view DirName(string_view path) {
#ifdef _WIN32
  static const char kPathSeparators[] = "\\/";
#else
  static const char kPathSeparators[] = "/";
#endif
  static const char* const kEnd = kPathSeparators + sizeof(kPathSeparators) - 1;

  string_view::size_type slash_pos = path.find_last_of(kPathSeparators);
  if (slash_pos == string_view::npos)
    return string_view();  // Nothing to do.
  while (slash_pos > 0 &&
         std::find(kPathSeparators, kEnd, path[slash_pos - 1]) != kEnd)
    --slash_pos;
  return path.substr(0, slash_pos);
}

int MakeDir(const string& path) {
#ifdef _WIN32
  return _mkdir(path.c_str());
#else
  return mkdir(path.c_str(), 0777);
#endif
}

#ifdef _WIN32
TimeStamp TimeStampFromFileTime(const FILETIME& filetime) {
  // FILETIME is in 100-nanosecond increments since the Windows epoch.
  // We don't much care about epoch correctness but we do want the
  // resulting value to fit in a 64-bit integer.
  uint64_t mtime = ((uint64_t)filetime.dwHighDateTime << 32) |
    ((uint64_t)filetime.dwLowDateTime);
  // 1600 epoch -> 2000 epoch (subtract 400 years).
  return (TimeStamp)mtime - 12622770400LL * (1000000000LL / 100);
}

TimeStamp StatSingleFile(const string& path, string* err);

/// The FILE_STAT_BASIC_INFORMATION struct filled in by
/// GetFileInformationByName(FileStatBasicByNameInfo).  It is declared here,
/// with the same layout as in the Windows SDK, as older SDKs and builds
/// targeting older versions of Windows don't declare it.
struct FileStatBasicInformation {
  LARGE_INTEGER FileId;
  LARGE_INTEGER CreationTime;
  LARGE_INTEGER LastAccessTime;
  LARGE_INTEGER LastWriteTime;
  LARGE_INTEGER ChangeTime;
  LARGE_INTEGER AllocationSize;
  LARGE_INTEGER EndOfFile;
  DWORD FileAttributes;
  DWORD ReparseTag;
  DWORD NumberOfLinks;
  DWORD DeviceType;
  DWORD DeviceCharacteristics;
  DWORD Reserved;
  LARGE_INTEGER VolumeSerialNumber;
  BYTE FileId128[16];
};

/// FILE_INFO_BY_NAME_CLASS::FileStatBasicByNameInfo.
const int kFileStatBasicByNameInfo = 3;

using GetFileInformationByNameFunc = BOOL(WINAPI*)(PCWSTR, int, PVOID, ULONG);

/// GetFileInformationByName, which was added in Windows 11 24H2, or null if
/// this version of Windows doesn't have it.  This is looked up once at
/// startup, rather than in a function-local static, so that each stat
/// doesn't pay for a thread-safe initialization check.
const GetFileInformationByNameFunc g_get_file_information_by_name =
    reinterpret_cast<GetFileInformationByNameFunc>(
        reinterpret_cast<void*>(::GetProcAddress(
            ::GetModuleHandleA("kernel32.dll"), "GetFileInformationByName")));

/// Try to stat |path| with GetFileInformationByName, which is several times
/// faster than GetFileAttributesEx as it doesn't open the file.  Return true
/// and set |*mtime| if it could, or false if the caller should fall back to
/// StatSingleFileSlow(), such as when the function isn't available, the file
/// system doesn't support it, or |path| is a reparse point.
bool StatSingleFileByName(const string& path, TimeStamp* mtime) {
  const GetFileInformationByNameFunc get_info = g_get_file_information_by_name;
  if (!get_info)
    return false;

  // Convert with the same code page that the "A" functions used elsewhere
  // in ninja use, so that paths are interpreted the same way.
  const int path_len = static_cast<int>(path.size());
  const int wide_len =
      ::MultiByteToWideChar(CP_ACP, 0, path.data(), path_len, NULL, 0);
  if (wide_len == 0 && path_len != 0)
    return false;
  std::wstring wide_path(wide_len, L'\0');
  ::MultiByteToWideChar(CP_ACP, 0, path.data(), path_len, &wide_path[0],
                        wide_len);

  FileStatBasicInformation info;
  if (!get_info(wide_path.c_str(), kFileStatBasicByNameInfo, &info,
                sizeof(info))) {
    const DWORD win_err = ::GetLastError();
    if (win_err == ERROR_FILE_NOT_FOUND || win_err == ERROR_PATH_NOT_FOUND) {
      *mtime = 0;
      return true;
    }
    // Leave unsupported file systems and real errors to the slow path, which
    // produces the error message.
    return false;
  }

  // Symlinks and other reparse points are resolved by the slow path.
  if (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
    return false;

  FILETIME last_write;
  last_write.dwLowDateTime = info.LastWriteTime.LowPart;
  last_write.dwHighDateTime = static_cast<DWORD>(info.LastWriteTime.HighPart);
  *mtime = TimeStampFromFileTime(last_write);
  return true;
}

TimeStamp StatSingleFileSlow(const string& path, string* err) {
  WIN32_FILE_ATTRIBUTE_DATA attrs;
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attrs)) {
    DWORD win_err = GetLastError();
    if (win_err == ERROR_FILE_NOT_FOUND || win_err == ERROR_PATH_NOT_FOUND)
      return 0;
    *err = "GetFileAttributesEx(" + path + "): " + GetLastErrorString();
    return -1;
  }

  if (attrs.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    HANDLE hFile = CreateFileA(path.c_str(), 0, 0, 0, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, 0);
    if (hFile == INVALID_HANDLE_VALUE) {
      DWORD win_err = GetLastError();
      if (win_err == ERROR_FILE_NOT_FOUND || win_err == ERROR_PATH_NOT_FOUND)
        return 0;
      *err = "CreateFileA(" + path + "): " + GetLastErrorString();
      CloseHandle(hFile);
      return -1;
    }

    CHAR pathBuf[MAX_PATH];
    if (GetFinalPathNameByHandleA(hFile, pathBuf, MAX_PATH,
                                  FILE_NAME_NORMALIZED) == 0) {
      DWORD win_err = GetLastError();
      if (win_err == ERROR_FILE_NOT_FOUND || win_err == ERROR_PATH_NOT_FOUND)
        return 0;
      *err = "GetFinalPathNameByHandleA(" + path + "): " + GetLastErrorString();
      CloseHandle(hFile);
      return -1;
    }
    CloseHandle(hFile);
    return StatSingleFile(pathBuf, err);
  }

  return TimeStampFromFileTime(attrs.ftLastWriteTime);
}

TimeStamp StatSingleFile(const string& path, string* err) {
  TimeStamp mtime;
  if (StatSingleFileByName(path, &mtime))
    return mtime;
  return StatSingleFileSlow(path, err);
}

bool IsWindows7OrLater() {
  OSVERSIONINFOEX version_info =
      { sizeof(OSVERSIONINFOEX), 6, 1, 0, 0, {0}, 0, 0, 0, 0, 0};
  DWORDLONG comparison = 0;
  VER_SET_CONDITION(comparison, VER_MAJORVERSION, VER_GREATER_EQUAL);
  VER_SET_CONDITION(comparison, VER_MINORVERSION, VER_GREATER_EQUAL);
  return VerifyVersionInfo(
      &version_info, VER_MAJORVERSION | VER_MINORVERSION, comparison);
}

char ToLowerForCache(char c) {
  return static_cast<char>(::tolower(static_cast<unsigned char>(c)));
}

bool StatAllFilesInDir(string_view dir,
                       map<string, TimeStamp, less<>>* stamps, string* err) {
  // FindExInfoBasic is 30% faster than FindExInfoStandard.
  static bool can_use_basic_info = IsWindows7OrLater();
  // This is not in earlier SDKs.
  const FINDEX_INFO_LEVELS kFindExInfoBasic =
      static_cast<FINDEX_INFO_LEVELS>(1);
  FINDEX_INFO_LEVELS level =
      can_use_basic_info ? kFindExInfoBasic : FindExInfoStandard;

  string pattern;
  pattern.reserve(dir.size() + 2);
  pattern.append(dir);
  pattern.append("\\*");
  WIN32_FIND_DATAA ffd;
  HANDLE find_handle = FindFirstFileExA(pattern.c_str(), level, &ffd,
                                        FindExSearchNameMatch, NULL, 0);

  if (find_handle == INVALID_HANDLE_VALUE) {
    DWORD win_err = GetLastError();
    if (win_err == ERROR_FILE_NOT_FOUND || win_err == ERROR_PATH_NOT_FOUND ||
        win_err == ERROR_DIRECTORY)
      return true;
    *err = "FindFirstFileExA(" + string(dir) + "): " + GetLastErrorString();
    return false;
  }
  do {
    if (strcmp(ffd.cFileName, "..") == 0) {
      // Seems to just copy the timestamp for ".." from ".", which is wrong.
      // This is the case at least on NTFS under Windows 7.
      continue;
    }

    // The key is moved into |stamps|, so this is the only copy of the name.
    string lowername = ffd.cFileName;
    transform(lowername.begin(), lowername.end(), lowername.begin(),
              ToLowerForCache);

    if (ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      // File is a symlink, stat the linked file.
      // |pattern| without its trailing '*' is "<dir>\".
      stamps->emplace(
          std::move(lowername),
          StatSingleFile(pattern.substr(0, pattern.size() - 1) + ffd.cFileName,
                         err));
    } else {
      stamps->emplace(std::move(lowername),
                      TimeStampFromFileTime(ffd.ftLastWriteTime));
    }

  } while (FindNextFileA(find_handle, &ffd));
  FindClose(find_handle);
  return true;
}
#endif  // _WIN32

}  // namespace

// DiskInterface ---------------------------------------------------------------

bool DiskInterface::MakeDirs(const string& path) {
  const string dir(DirName(path));
  if (dir.empty())
    return true;  // Reached root; assume it's there.
  string err;
  TimeStamp mtime = Stat(dir, &err);
  if (mtime < 0) {
    Error("%s", err.c_str());
    return false;
  }
  if (mtime > 0)
    return true;  // Exists already; we're done.

  // Directory doesn't exist.  Try creating its parent first.
  bool success = MakeDirs(dir);
  if (!success)
    return false;
  return MakeDir(dir);
}

// RealDiskInterface -----------------------------------------------------------
RealDiskInterface::RealDiskInterface()
#ifdef _WIN32
: use_cache_(false), long_paths_enabled_(false) {
  // Probe ntdll.dll for RtlAreLongPathsEnabled, and call it if it exists.
  HINSTANCE ntdll_lib = ::GetModuleHandleW(L"ntdll");
  if (ntdll_lib) {
    typedef BOOLEAN(WINAPI FunctionType)();
    auto* func_ptr = FunctionCast<FunctionType*>(
        ::GetProcAddress(ntdll_lib, "RtlAreLongPathsEnabled"));
    if (func_ptr) {
      long_paths_enabled_ = (*func_ptr)();
    }
  }
}
#else
{}
#endif

TimeStamp RealDiskInterface::Stat(const string& path, string* err) const {
  METRIC_RECORD("node stat");
#ifdef _WIN32
  // MSDN: "Naming Files, Paths, and Namespaces"
  // http://msdn.microsoft.com/en-us/library/windows/desktop/aa365247(v=vs.85).aspx
  if (!path.empty() && !AreLongPathsEnabled() && path[0] != '\\' &&
      path.size() > MAX_PATH) {
    ostringstream err_stream;
    err_stream << "Stat(" << path << "): Filename longer than " << MAX_PATH
               << " characters";
    *err = err_stream.str();
    return -1;
  }
  if (!use_cache_)
    return StatSingleFile(path, err);

  string_view dir = DirName(path);
  string_view base = string_view(path).substr(dir.size() ? dir.size() + 1 : 0);
  if (base == "..") {
    // StatAllFilesInDir does not report any information for base = "..".
    base = ".";
    dir = path;
  }

  // Lowercase |dir| and |base| into one buffer for the cache lookups.  It is
  // on the stack unless the path is too long to fit, which only happens with
  // long paths enabled.
  char stack_buffer[2 * MAX_PATH];
  string heap_buffer;
  char* lower = stack_buffer;
  if (dir.size() + base.size() > sizeof(stack_buffer)) {
    heap_buffer.resize(dir.size() + base.size());
    lower = &heap_buffer[0];
  }
  transform(dir.begin(), dir.end(), lower, ToLowerForCache);
  transform(base.begin(), base.end(), lower + dir.size(), ToLowerForCache);
  const string_view dir_lowercase(lower, dir.size());
  const string_view base_lowercase(lower + dir.size(), base.size());

  Cache::iterator ci = cache_.find(dir_lowercase);
  if (ci == cache_.end()) {
    ci = cache_.try_emplace(string(dir_lowercase)).first;
    if (!StatAllFilesInDir(dir.empty() ? string_view(".") : dir, &ci->second,
                           err)) {
      cache_.erase(ci);
      return -1;
    }
  }
  DirCache::iterator di = ci->second.find(base_lowercase);
  return di != ci->second.end() ? di->second : 0;
#else
#ifdef __USE_LARGEFILE64
  struct stat64 st;
  if (stat64(path.c_str(), &st) < 0) {
#else
  struct stat st;
  if (stat(path.c_str(), &st) < 0) {
#endif
    if (errno == ENOENT || errno == ENOTDIR)
      return 0;
    *err = "stat(" + path + "): " + strerror(errno);
    return -1;
  }
  // Some users (Flatpak) set mtime to 0, this should be harmless
  // and avoids conflicting with our return value of 0 meaning
  // that it doesn't exist.
  if (st.st_mtime == 0)
    return 1;
#if defined(_AIX)
  return (int64_t)st.st_mtime * 1000000000LL + st.st_mtime_n;
#elif defined(__APPLE__)
  return ((int64_t)st.st_mtimespec.tv_sec * 1000000000LL +
          st.st_mtimespec.tv_nsec);
#elif defined(st_mtime) // A macro, so we're likely on modern POSIX.
  return (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#else
  return (int64_t)st.st_mtime * 1000000000LL + st.st_mtimensec;
#endif
#endif
}

bool RealDiskInterface::WriteFile(const string& path, const string& contents,
                                  bool crlf_on_windows) {
  FILE* fp = fopen(path.c_str(),
#ifdef _WIN32
                   crlf_on_windows ? "w" : "wb");
#else
                   "wb");
  (void)crlf_on_windows;
#endif
  if (fp == NULL) {
    Error("WriteFile(%s): Unable to create file. %s",
          path.c_str(), strerror(errno));
    return false;
  }

  if (fwrite(contents.data(), 1, contents.length(), fp) < contents.length())  {
    Error("WriteFile(%s): Unable to write to the file. %s",
          path.c_str(), strerror(errno));
    fclose(fp);
    return false;
  }

  if (fclose(fp) == EOF) {
    Error("WriteFile(%s): Unable to close the file. %s",
          path.c_str(), strerror(errno));
    return false;
  }

  return true;
}

bool RealDiskInterface::MakeDir(const string& path) {
  if (::MakeDir(path) < 0) {
    if (errno == EEXIST) {
      return true;
    }
    Error("mkdir(%s): %s", path.c_str(), strerror(errno));
    return false;
  }
  return true;
}

FileReader::Status RealDiskInterface::ReadFile(const string& path,
                                               string* contents,
                                               string* err) {
  switch (::ReadFile(path, contents, err)) {
  case 0:       return Okay;
  case -ENOENT: return NotFound;
  default:      return OtherError;
  }
}

int RealDiskInterface::RemoveFile(const string& path) {
#ifdef _WIN32
  DWORD attributes = GetFileAttributesA(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    DWORD win_err = GetLastError();
    if (win_err == ERROR_FILE_NOT_FOUND || win_err == ERROR_PATH_NOT_FOUND) {
      return 1;
    }
  } else if (attributes & FILE_ATTRIBUTE_READONLY) {
    // On non-Windows systems, remove() will happily delete read-only files.
    // On Windows Ninja should behave the same:
    //   https://github.com/ninja-build/ninja/issues/1886
    // Skip error checking.  If this fails, accept whatever happens below.
    SetFileAttributesA(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
  }
  if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
    // remove() deletes both files and directories. On Windows we have to
    // select the correct function (DeleteFile will yield Permission Denied when
    // used on a directory)
    // This fixes the behavior of ninja -t clean in some cases
    // https://github.com/ninja-build/ninja/issues/828
    if (!RemoveDirectoryA(path.c_str())) {
      DWORD win_err = GetLastError();
      if (win_err == ERROR_FILE_NOT_FOUND || win_err == ERROR_PATH_NOT_FOUND) {
        return 1;
      }
      // Report remove(), not RemoveDirectory(), for cross-platform consistency.
      Error("remove(%s): %s", path.c_str(), GetLastErrorString().c_str());
      return -1;
    }
  } else {
    if (!DeleteFileA(path.c_str())) {
      DWORD win_err = GetLastError();
      if (win_err == ERROR_FILE_NOT_FOUND || win_err == ERROR_PATH_NOT_FOUND) {
        return 1;
      }
      // Report as remove(), not DeleteFile(), for cross-platform consistency.
      Error("remove(%s): %s", path.c_str(), GetLastErrorString().c_str());
      return -1;
    }
  }
#else
  if (remove(path.c_str()) < 0) {
    switch (errno) {
      case ENOENT:
        return 1;
      default:
        Error("remove(%s): %s", path.c_str(), strerror(errno));
        return -1;
    }
  }
#endif
  return 0;
}

void RealDiskInterface::AllowStatCache(bool allow) {
#ifdef _WIN32
  use_cache_ = allow;
  if (!use_cache_)
    cache_.clear();
#endif
}

#ifdef _WIN32
bool RealDiskInterface::AreLongPathsEnabled(void) const {
  return long_paths_enabled_;
}
#endif
