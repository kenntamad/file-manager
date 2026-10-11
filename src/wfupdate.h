/********************************************************************

   wfupdate.h

   Automatic updates from GitHub Releases.

   Licensed under the MIT License.

********************************************************************/

#ifndef _WFUPDATE_H
#define _WFUPDATE_H

//
// Messages posted to the frame window
//
#define FS_UPDATECHECKED     (WM_APP + 0x220)  // lParam = PUPDATEINFO (frame frees it)
#define FS_UPDATEDOWNLOADED  (WM_APP + 0x221)  // lParam = PUPDATEINFO (frame frees it)

extern BOOL  bAutoUpdate;
extern WCHAR szAutoUpdate[];

// Call first thing in WinMain. Handles the "/wfupdated:<pid>" switch that a
// freshly installed update is started with, and advances past it.
VOID UpdateHandleCommandLine(LPWSTR* ppszCmdLine);

// Call once after settings are loaded and the frame window exists.
// Reads settings, removes leftovers of a previous update and starts the
// daily background check when automatic updates are on.
VOID AutoUpdateInit(VOID);

// Check now. bInteractive also reports "up to date" and errors.
VOID UpdateCheck(BOOL bInteractive);

// Frame window handlers
VOID UpdateOnChecked(LPARAM lParam);
VOID UpdateOnDownloaded(LPARAM lParam);

#endif // _WFUPDATE_H
