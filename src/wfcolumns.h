/********************************************************************

   wfcolumns.h

   Resizable column header for the directory window (right pane).

   Licensed under the MIT License.

********************************************************************/

#ifndef _WFCOLUMNS_H
#define _WFCOLUMNS_H

#define IDCW_HEADER          7                 // header control in the directory window
#define FS_HEADERSYNC        (WM_APP + 0x230)  // rebuild header items / layout (posted to the dir window)

// Slot in the GWL_TABARRAY array that holds the width of the Name column
// (directory windows only).
#define TAB_NAMEWIDTH        (MAX_TAB_COLUMNS - 1)

#ifndef RC_INVOKED

enum {
   COL_NAME = 0,
   COL_DOSNAME,
   COL_SIZE,
   COL_DATE,
   COL_TIME,
   COL_ATTR,
   COL_MAX
};

VOID  ColumnsInit(VOID);

// TRUE when hwndLB is the listbox of a directory window (not search results)
BOOL  ColumnsIsDirListbox(HWND hwndLB);
BOOL  ColumnsIsDirWindow(HWND hwnd);

// Width the user gave a column, or iAuto when it was never resized
INT   ColumnsWidth(INT iCol, INT iAuto);
BOOL  ColumnsNameIsAuto(VOID);

// Directory window hooks
VOID    DirHeaderSync(HWND hwndDir);
VOID    DirLayout(HWND hwndDir);
LRESULT DirHeaderNotify(HWND hwndDir, LPNMHDR pnm);

// Draws one listbox line (CreateLBLine format) column by column, each
// clipped to its column. x is where the Name column text starts.
VOID  DirDrawColumns(HDC hdc, const RECT* prcItem, INT x, LPWSTR pszLine,
                     const WORD* pwTabs, DWORD dwView);

// in wfdir.c
VOID  DirRecalcColumns(HWND hwndDir);

#endif // RC_INVOKED

#endif // _WFCOLUMNS_H
