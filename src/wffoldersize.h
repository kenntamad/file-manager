/********************************************************************

   wffoldersize.h

   Background calculation of total folder sizes for the directory
   window (Size column, status bar and Properties dialog).

   Licensed under the MIT License.

********************************************************************/

#ifndef _WFFOLDERSIZE_H
#define _WFFOLDERSIZE_H

//
// Messages sent to a directory window (IDCW_DIR)
//
#define FS_FOLDERSIZESTART   (WM_APP + 0x210)  // begin (re)computing sizes for the current listing
#define FS_FOLDERSIZERESULT  (WM_APP + 0x211)  // wParam = job generation, lParam = PFSRESULT or NULL when done

//
// Values for XDTA.bySizeState (directories only)
//
#define FOLDERSIZE_UNKNOWN   0   // not computed yet, shows <DIR>
#define FOLDERSIZE_CACHED    1   // value from a recent calculation, being refreshed
#define FOLDERSIZE_FINAL     2   // value computed for this listing

extern BOOL  bFolderSizes;
extern WCHAR szFolderSizes[];

VOID     FolderSizeInit(VOID);
VOID     FolderSizeToggle(VOID);

// Directory window hooks
VOID     FolderSizeRequest(HWND hwndDir);          // thread safe, posts FS_FOLDERSIZESTART
VOID     FolderSizeStart(HWND hwndDir);
VOID     FolderSizeCancel(HWND hwndDir);
VOID     FolderSizeOnResult(HWND hwndDir, WPARAM wParam, LPARAM lParam);

// Size of a listing entry as it should be shown/summed. For a folder this
// is the total size of its contents when known, 0 otherwise.
BOOL     FolderSizeIsKnown(LPXDTA lpxdta);
LONGLONG FolderSizeOfItem(LPXDTA lpxdta);

// Total of all known folder sizes in a listing (files are not included).
LONGLONG FolderSizeListingTotal(LPXDTALINK lpStart);

// Synchronous recursive size of a folder. pfCancel may be NULL.
// Returns FALSE if cancelled. Reparse points (junctions, symlinks) are not
// followed, so nothing is counted twice and link cycles are impossible.
BOOL     FolderSizeCompute(LPCWSTR pszPath, PLARGE_INTEGER pqSize, volatile LONG* pfCancel);

#endif // _WFFOLDERSIZE_H
