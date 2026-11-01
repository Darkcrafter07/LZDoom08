/*
** i_specialpaths.cpp - xp faulty
** Gets special system folders where data should be stored. (Windows version)
**
**---------------------------------------------------------------------------
** Copyright 2013-2016 Randy Heit
** Copyright 2016 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#ifdef IKNOW_UNKNOWN
typedef struct IUnknown IUnknown; // XP patch
#endif

#include <windows.h>
#include <lmcons.h>
#include <shlobj.h>
#include <Shlwapi.h>

#include "i_specialpaths.h"
#include "printf.h"
#include "cmdlib.h"
#include "findfile.h"
#include "version.h" // for GAMENAME
#include "gstrings.h"
#include "i_mainwindow.h"
#include "engineerrors.h"

// Define modern folder flags manually if missing in old SDK headers
#ifndef KF_FLAG_CREATE
#define KF_FLAG_CREATE 0x00008000
#endif

static int isportable = -1;
extern bool winXPcompat; // XP patch: declared in win32, i_main.cpp

// Stub replacement for missing VersionHelpers on Windows XP
static bool IsWindows8OrGreaterXP()
{
	OSVERSIONINFOEXW osvi = { sizeof(osvi), 0, 0, 0, 0, {0}, 0, 0 };
	DWORDLONG const dwlConditionMask = VerSetConditionMask(
		VerSetConditionMask(
			VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL),
			VER_MINORVERSION, VER_GREATER_EQUAL),
		VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL);

	osvi.dwMajorVersion = 6;
	osvi.dwMinorVersion = 2;
	return VerifyVersionInfoW(&osvi, VER_MAJORVERSION | VER_MINORVERSION, dwlConditionMask) != FALSE;
}

//==========================================================================
//
// IsPortable
//
//==========================================================================
bool IsPortable()
{
	// XP Compatibility: block the system folders completely and enforce root directory execution
	if (winXPcompat)
	{
		isportable = true;
		return true;
	}

	// Original modern OS detection fallback
	if (isportable >= 0)
	{
		return !!isportable;
	}

	bool found = false;
	for (auto p : { L"ProgramFiles", L"ProgramFiles(x86)" })
	{
		wchar_t buffer1[256];
		if (GetEnvironmentVariable(p, buffer1, 256))
		{
			FString envpath(buffer1);
			if (winXPcompat) envpath.ReplaceChars('/', '\\');
			else envpath.ReplaceChars('\\', '/');

			if (progdir.MakeLower().IndexOf(envpath.MakeLower()) == 0)
			{
				isportable = false;
				return false;
			}
		}
	}

	FString pathf("%s" GAMENAMELOWERCASE "_portable.ini", progdir.GetChars());
	if (FileExists(pathf))
	{
		HANDLE file = CreateFileW(pathf.WideString().c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file != INVALID_HANDLE_VALUE)
		{
			CloseHandle(file);
			if (!batchrun) Printf("Using portable configuration\n");
			isportable = true;
			return true;
		}
	}

	isportable = false;
	return false;
}

//==========================================================================
//
// GetKnownFolder
//
// Backported to use SHGetFolderPathA for Windows XP compatibility
//
//==========================================================================
FString GetKnownFolder(int shell_folder, const void* dummy_guid, bool create)
{
	char path[MAX_PATH];
	int flags = shell_folder;
	if (create) flags |= CSIDL_FLAG_CREATE;

	if (winXPcompat)
	{
		// Force standard Windows XP token and type 0 initialization
		if (FAILED(SHGetFolderPathA(NULL, flags, NULL, 0, path)))
		{
			I_FatalError("Unable to retrieve known folder.");
		}
	}
	else
	{
		if (FAILED(SHGetFolderPathA(NULL, flags, NULL, SHGFP_TYPE_CURRENT, path)))
		{
			if (FAILED(SHGetFolderPathA(NULL, flags, NULL, SHGFP_TYPE_DEFAULT, path)))
			{
				I_FatalError("Unable to retrieve known folder.");
			}
		}
	}

	FString fpath = FString(path);

	// Normalize slashes safely depending on active compatibility mode
	if (winXPcompat)
	{
		fpath.ReplaceChars('/', '\\');
	}
	else
	{
		fpath.ReplaceChars('\\', '/');
	}

	return fpath;
}

//==========================================================================
//
// M_GetAppDataPath
//
//==========================================================================
FString M_GetAppDataPath(bool create)
{
	FString path = GetKnownFolder(CSIDL_LOCAL_APPDATA, NULL, create);
	path += "/" GAMENAMELOWERCASE;
	if (create)
	{
		CreatePath(path.GetChars());
	}
	return path;
}

//==========================================================================
//
// M_GetCachePath
//
//==========================================================================
FString M_GetCachePath(bool create)
{
	FString path = GetKnownFolder(CSIDL_LOCAL_APPDATA, NULL, create);
	path += "/zdoom/cache";
	if (create)
	{
		CreatePath(path.GetChars());
	}
	return path;
}

//==========================================================================
//
// M_GetAutoexecPath
//
//==========================================================================
FString M_GetAutoexecPath()
{
	return "$PROGDIR/autoexec.cfg";
}

//==========================================================================
//
// M_GetOldConfigPath
//
//==========================================================================
FString M_GetOldConfigPath(int& type)
{
	FString path;
	HRESULT hr;
	WCHAR uname[UNLEN + 1];
	DWORD unamelen = UNLEN;
	path = progdir;
	hr = GetUserNameW(uname, &unamelen);
	if (SUCCEEDED(hr) && uname[0] != 0)
	{
		auto probe = uname;
		while (*probe != 0)
		{
			if (*probe == '\\' || *probe == '/')
				*probe = '-';
			++probe;
		}
		type = 0;
		path << GAMENAMELOWERCASE "-" << FString(uname) << ".ini";
		if (FileExists(path))
			return path;
	}

	path = GetKnownFolder(CSIDL_APPDATA, NULL, true);
	path += "/" GAME_DIR "/" GAMENAMELOWERCASE ".ini";
	type = 1;
	if (FileExists(path))
		return path;

	return "";
}

//==========================================================================
//
// M_MigrateOldConfig
//
// Disabled on Windows XP due to missing TaskDialog API
//
//==========================================================================
int M_MigrateOldConfig()
{
	// TaskDialog API does not exist on Windows XP, skip migration choices cleanly
	return IDCANCEL;
}

//==========================================================================
//
// M_GetConfigPath
//
//==========================================================================
FString M_GetConfigPath(bool for_reading)
{
	if (IsPortable())
	{
		return FStringf("%s" GAMENAMELOWERCASE "_portable.ini", progdir.GetChars());
	}

	FString path = GetKnownFolder(CSIDL_APPDATA, NULL, true);
	path += "/My Games/" GAME_DIR;
	CreatePath(path.GetChars());
	path += "/" GAMENAMELOWERCASE ".ini";
	if (!for_reading || FileExists(path))
		return path;

	int type = 0;
	FString oldpath = M_GetOldConfigPath(type);
	if (!oldpath.IsEmpty())
	{
		if (type == 0)
		{
			int action = M_MigrateOldConfig();
			if (action == IDNO)
			{
				path.Format("%s" GAMENAMELOWERCASE "_portable.ini", progdir.GetChars());
				isportable = true;
			}
		}

		// MoveFileExW works correctly on XP
		bool res = MoveFileExW(WideString(oldpath.GetChars()).c_str(), WideString(path.GetChars()).c_str(), MOVEFILE_COPY_ALLOWED);
		if (res) return path;
		else return oldpath;
	}

	if (for_reading)
	{
		if (!FileExists(path))
		{
			path = progdir;
			path << GAMENAMELOWERCASE ".ini";
		}
	}
	return path;
}

//==========================================================================
//
// M_GetScreenshotsPath
//
//==========================================================================
FString M_GetScreenshotsPath()
{
	FString path;
	if (IsPortable())
	{
		path << progdir << "Screenshots/";
	}
	else if (IsWindows8OrGreaterXP())
	{
		// Modern Windows path fallback
		path = GetKnownFolder(CSIDL_MYPICTURES, NULL, true);
		path << "/" GAMENAME "/";
	}
	else
	{
		// Classic Windows XP Pictures location
		path = GetKnownFolder(CSIDL_MYPICTURES, NULL, true);
		path << "/Screenshots/" GAMENAME "/";
	}
	CreatePath(path.GetChars());
	return path;
}

//==========================================================================
//
// M_GetSavegamesPath
//
//==========================================================================
FString M_GetSavegamesPath()
{
	FString path;
	if (IsPortable())
	{
		path << progdir << "Save/";
	}
	else
	{
		// Windows XP saves into My Documents/My Games/GAMENAME instead of FOLDERID_SavedGames
		path = GetKnownFolder(CSIDL_PERSONAL, NULL, true);
		path << "/My Games/" GAMENAME "/";
	}
	return path;
}

//==========================================================================
//
// M_GetDocumentsPath
//
//==========================================================================
FString M_GetDocumentsPath()
{
	FString path;
	if (IsPortable())
	{
		return progdir;
	}
	else
	{
		path = GetKnownFolder(CSIDL_PERSONAL, NULL, true);
		path << "/My Games/" GAMENAME "/";
		CreatePath(path.GetChars());
	}
	return path;
}

//==========================================================================
//
// M_GetDemoPath
//
//==========================================================================
FString M_GetDemoPath()
{
	FString path;
	if (IsPortable())
	{
		path << progdir << "Demos/";
	}
	else
	{
		path = GetKnownFolder(CSIDL_PERSONAL, NULL, true);
		path << "/My Games/" GAMENAME "/";
	}
	return path;
}

//==========================================================================
//
// M_GetNormalizedPath
//
//==========================================================================
FString M_GetNormalizedPath(const char* path)
{
	std::wstring wpath = WideString(path);
	wchar_t buffer[MAX_PATH];
	GetFullPathNameW(wpath.c_str(), MAX_PATH, buffer, nullptr);
	FString result(buffer);
	FixPathSeperator(result);
	return result;
}
