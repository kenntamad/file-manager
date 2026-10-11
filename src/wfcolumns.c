/********************************************************************

   wfcolumns.c

   Resizable column header for the directory window (right pane),
   similar to File Explorer's details view:

   - drag a divider to resize a column (the list follows live)
   - double-click a divider to go back to the automatic width
   - click a column title to sort by it (Name, Size, Date/Time)

   Widths are shared by all directory windows and saved in winfile.ini
   as ColumnWidths=name,shortname,size,date,time,attributes (0 = auto).

   The header sits above the listbox and moves with its horizontal
   scroll position. Columns map onto the existing tab stop array that
   FixTabsAndThings() fills: each tab stop is the right edge of a
   column, and slot TAB_NAMEWIDTH holds the width of the Name column.

   Licensed under the MIT License.

********************************************************************/

#include "winfile.h"
#include "wfcolumns.h"
#include <commctrl.h>

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#endif

static INT aColWidth[COL_MAX];          // 0 = automatic
static WCHAR szColumnWidths[] = L"ColumnWidths";
static const WCHAR szTrackingProp[] = L"WinfileHeaderTracking";

static const LPCWSTR aszColTitle[COL_MAX] = {
   L"Name", L"Short Name", L"Size", L"Date", L"Time", L"Attributes"
};

#define MIN_NAME_WIDTH   40
#define MIN_COL_WIDTH    16

// The header's own visible flag (IsWindowVisible is FALSE while the
// directory window itself is still being created)
#define HeaderShown(h)   ((h) && (GetWindowLongPtr((h), GWL_STYLE) & WS_VISIBLE))

// x offset of the name text from the left of a listbox line (see DrawItem)
static INT
NameTextOffset(VOID)
{
   return 1 + dxFolder + dyBorderx2 + dyBorder;
}


/////////////////////////////////////////////////////////////////////
//
// Settings
//
/////////////////////////////////////////////////////////////////////

VOID
ColumnsInit(VOID)
{
   WCHAR szBuf[128];
   LPWSTR p = szBuf;
   INT i;

   GetPrivateProfileString(szSettings, szColumnWidths, L"", szBuf, COUNTOF(szBuf), szTheINIFile);

   for (i = 0; i < COL_MAX && *p; i++) {
      aColWidth[i] = max(0, StrToInt(p));
      while (*p && *p != L',')
         p++;
      if (*p == L',')
         p++;
   }
}

static VOID
ColumnsSave(VOID)
{
   WCHAR szBuf[128];

   StringCchPrintf(szBuf, COUNTOF(szBuf), L"%d,%d,%d,%d,%d,%d",
                   aColWidth[0], aColWidth[1], aColWidth[2],
                   aColWidth[3], aColWidth[4], aColWidth[5]);
   WritePrivateProfileString(szSettings, szColumnWidths, szBuf, szTheINIFile);
}

INT
ColumnsWidth(INT iCol, INT iAuto)
{
   return aColWidth[iCol] > 0 ? aColWidth[iCol] : iAuto;
}

BOOL
ColumnsNameIsAuto(VOID)
{
   return aColWidth[COL_NAME] <= 0;
}

BOOL
ColumnsIsDirWindow(HWND hwnd)
{
   WCHAR szClass[32];

   return hwnd &&
          GetClassName(hwnd, szClass, COUNTOF(szClass)) &&
          !lstrcmpi(szClass, szDirClass);
}

BOOL
ColumnsIsDirListbox(HWND hwndLB)
{
   return hwndLB && ColumnsIsDirWindow(GetParent(hwndLB));
}


/////////////////////////////////////////////////////////////////////
//
// Which columns are showing
//
/////////////////////////////////////////////////////////////////////

static INT
VisibleColumns(DWORD dwView, INT aiCol[COL_MAX])
{
   INT c = 0;

   aiCol[c++] = COL_NAME;
   if (dwView & VIEW_DOSNAMES) aiCol[c++] = COL_DOSNAME;
   if (dwView & VIEW_SIZE)     aiCol[c++] = COL_SIZE;
   if (dwView & VIEW_DATE)     aiCol[c++] = COL_DATE;
   if (dwView & VIEW_TIME)     aiCol[c++] = COL_TIME;
   if (dwView & VIEW_FLAGS)    aiCol[c++] = COL_ATTR;

   return c;
}


/////////////////////////////////////////////////////////////////////
//
// Drawing a line
//
/////////////////////////////////////////////////////////////////////

VOID
DirDrawColumns(HDC hdc, const RECT* prcItem, INT x, LPWSTR pszLine,
               const WORD* pwTabs, DWORD dwView)
{
   INT aiCol[COL_MAX];
   INT cCols = VisibleColumns(dwView, aiCol);
   INT iField;
   INT xLeft = x;
   INT gap = max(dxText / 2, 3);
   LPWSTR p = pszLine;

   for (iField = 0; iField < cCols && p; iField++) {

      LPWSTR pEnd = StrChrW(p, CHAR_TAB);
      INT cch = pEnd ? (INT)(pEnd - p) : lstrlen(p);
      INT xRight;
      UINT uFormat = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS;
      RECT rc;

      if (iField == 0)
         xRight = x + pwTabs[TAB_NAMEWIDTH];
      else
         xRight = x + pwTabs[iField - 1];

      rc.top = prcItem->top;
      rc.bottom = prcItem->bottom;

      if (iField == 0) {
         rc.left = xLeft;
         rc.right = xRight - gap;
         uFormat |= DT_LEFT;
      } else if (aiCol[iField] == COL_DOSNAME) {
         rc.left = xLeft + gap;
         rc.right = xRight - gap;
         uFormat |= DT_LEFT;
      } else {
         rc.left = xLeft + gap;
         rc.right = xRight;
         uFormat |= DT_RIGHT;
      }

      if (rc.right > rc.left && cch)
         DrawText(hdc, p, cch, &rc, uFormat);

      xLeft = xRight;
      p = pEnd ? pEnd + 1 : NULL;
   }
}


/////////////////////////////////////////////////////////////////////
//
// Header control
//
/////////////////////////////////////////////////////////////////////

static VOID
PositionHeader(HWND hwndDir, HWND hwndHdr, HWND hwndLB, INT* pcyHeader)
{
   RECT rc;
   HDLAYOUT hdl;
   WINDOWPOS wp;
   RECT rcLayout;
   INT xScroll = hwndLB ? GetScrollPos(hwndLB, SB_HORZ) : 0;

   GetClientRect(hwndDir, &rc);

   rcLayout = rc;
   hdl.prc = &rcLayout;
   hdl.pwpos = &wp;
   if (!SendMessage(hwndHdr, HDM_LAYOUT, 0, (LPARAM)&hdl))
      wp.cy = dyFileName + 2 * dyBorderx2;

   SetWindowPos(hwndHdr, NULL, -xScroll, 0, rc.right + xScroll, wp.cy,
                SWP_NOZORDER | SWP_NOACTIVATE);

   if (pcyHeader)
      *pcyHeader = wp.cy;
}

VOID
DirLayout(HWND hwndDir)
{
   HWND hwndLB = GetDlgItem(hwndDir, IDCW_LISTBOX);
   HWND hwndHdr = GetDlgItem(hwndDir, IDCW_HEADER);
   RECT rc;
   INT cyHeader = 0;

   GetClientRect(hwndDir, &rc);

   if (HeaderShown(hwndHdr))
      PositionHeader(hwndDir, hwndHdr, hwndLB, &cyHeader);

   if (hwndLB)
      MoveWindow(hwndLB, 0, cyHeader, rc.right, max(0, rc.bottom - cyHeader), TRUE);
}

//
// Keeps the header lined up with the list when it scrolls sideways.
//
static LRESULT CALLBACK
DirListSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
   LRESULT lRet = DefSubclassProc(hwnd, uMsg, wParam, lParam);

   if (uMsg == WM_NCDESTROY) {
      RemoveWindowSubclass(hwnd, DirListSubclassProc, uIdSubclass);
      return lRet;
   }

   {
      INT xScroll = GetScrollPos(hwnd, SB_HORZ);

      if (xScroll != (INT)dwRefData) {

         HWND hwndDir = GetParent(hwnd);
         HWND hwndHdr = GetDlgItem(hwndDir, IDCW_HEADER);

         SetWindowSubclass(hwnd, DirListSubclassProc, uIdSubclass, (DWORD_PTR)xScroll);

         if (HeaderShown(hwndHdr))
            PositionHeader(hwndDir, hwndHdr, hwnd, NULL);
      }
   }

   return lRet;
}

VOID
DirHeaderSync(HWND hwndDir)
{
   HWND hwndListParms = (HWND)GetWindowLongPtr(hwndDir, GWL_LISTPARMS);
   HWND hwndLB = GetDlgItem(hwndDir, IDCW_LISTBOX);
   HWND hwndHdr = GetDlgItem(hwndDir, IDCW_HEADER);
   WORD* pwTabs = (WORD*)GetWindowLongPtr(hwndDir, GWL_TABARRAY);
   DWORD dwView, dwSort;
   INT aiCol[COL_MAX];
   INT cCols, i, xPrev;

   if (!hwndListParms || !pwTabs)
      return;

   dwView = (DWORD)GetWindowLongPtr(hwndListParms, GWL_VIEW);
   dwSort = (DWORD)GetWindowLongPtr(hwndListParms, GWL_SORT);

   if (!(dwView & VIEW_EVERYTHING) || !hwndLB) {
      if (hwndHdr)
         ShowWindow(hwndHdr, SW_HIDE);
      DirLayout(hwndDir);
      return;
   }

   if (!hwndHdr) {
      hwndHdr = CreateWindowEx(0, WC_HEADER, NULL,
                               WS_CHILD | HDS_HORZ | HDS_BUTTONS | HDS_FULLDRAG,
                               0, 0, 0, 0,
                               hwndDir, (HMENU)IDCW_HEADER, hAppInstance, NULL);
      if (!hwndHdr) {
         DirLayout(hwndDir);
         return;
      }
   }

   SendMessage(hwndHdr, WM_SETFONT, (WPARAM)hFont, FALSE);

   SetWindowSubclass(hwndLB, DirListSubclassProc, 1, (DWORD_PTR)-1);

   //
   // Don't rebuild the items while the user drags a divider.
   //
   if (!GetProp(hwndDir, szTrackingProp)) {

      SendMessage(hwndHdr, WM_SETREDRAW, FALSE, 0);

      while (SendMessage(hwndHdr, HDM_GETITEMCOUNT, 0, 0) > 0)
         SendMessage(hwndHdr, HDM_DELETEITEM, 0, 0);

      cCols = VisibleColumns(dwView, aiCol);
      xPrev = pwTabs[TAB_NAMEWIDTH];

      for (i = 0; i < cCols; i++) {

         HDITEM hdi;
         INT iCol = aiCol[i];

         ZeroMemory(&hdi, sizeof(hdi));
         hdi.mask = HDI_TEXT | HDI_WIDTH | HDI_FORMAT | HDI_LPARAM;
         hdi.pszText = (LPWSTR)aszColTitle[iCol];
         hdi.lParam = iCol;

         if (i == 0) {
            hdi.cxy = NameTextOffset() + pwTabs[TAB_NAMEWIDTH];
            hdi.fmt = HDF_LEFT | HDF_STRING;
         } else {
            hdi.cxy = pwTabs[i - 1] - xPrev;
            xPrev = pwTabs[i - 1];
            hdi.fmt = (iCol == COL_DOSNAME ? HDF_LEFT : HDF_RIGHT) | HDF_STRING;
         }

         if ((iCol == COL_NAME && dwSort == IDD_NAME))
            hdi.fmt |= HDF_SORTUP;
         else if ((iCol == COL_SIZE && dwSort == IDD_SIZE) ||
                  (iCol == COL_DATE && dwSort == IDD_DATE))
            hdi.fmt |= HDF_SORTDOWN;

         SendMessage(hwndHdr, HDM_INSERTITEM, i, (LPARAM)&hdi);
      }

      SendMessage(hwndHdr, WM_SETREDRAW, TRUE, 0);
      InvalidateRect(hwndHdr, NULL, TRUE);
   }

   ShowWindow(hwndHdr, SW_SHOWNA);
   DirLayout(hwndDir);
}

static INT
ColumnOfItem(HWND hwndHdr, INT iItem)
{
   HDITEM hdi;

   hdi.mask = HDI_LPARAM;
   if (iItem < 0 || !SendMessage(hwndHdr, HDM_GETITEM, iItem, (LPARAM)&hdi))
      return -1;
   return (INT)hdi.lParam;
}

static VOID
RefreshAllDirWindows(HWND hwndExcept)
{
   HWND hwnd;

   for (hwnd = GetWindow(hwndMDIClient, GW_CHILD); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
      HWND hwndDir = HasDirWindow(hwnd);
      if (hwndDir && hwndDir != hwndExcept)
         DirRecalcColumns(hwndDir);
   }
}

LRESULT
DirHeaderNotify(HWND hwndDir, LPNMHDR pnm)
{
   LPNMHEADER pnmh = (LPNMHEADER)pnm;
   HWND hwndHdr = pnm->hwndFrom;
   INT iCol;

   switch (pnm->code) {

   case HDN_BEGINTRACK:
      SetProp(hwndDir, szTrackingProp, (HANDLE)1);
      break;

   case HDN_ITEMCHANGED:

      if (!pnmh->pitem || !(pnmh->pitem->mask & HDI_WIDTH))
         break;

      if (!GetProp(hwndDir, szTrackingProp))
         break;          // our own rebuild, not the user

      iCol = ColumnOfItem(hwndHdr, pnmh->iItem);
      if (iCol < 0)
         break;

      if (iCol == COL_NAME)
         aColWidth[COL_NAME] = max(MIN_NAME_WIDTH, pnmh->pitem->cxy - NameTextOffset());
      else
         aColWidth[iCol] = max(MIN_COL_WIDTH, pnmh->pitem->cxy);

      DirRecalcColumns(hwndDir);
      break;

   case HDN_ENDTRACK:
      RemoveProp(hwndDir, szTrackingProp);
      ColumnsSave();
      DirRecalcColumns(hwndDir);
      RefreshAllDirWindows(hwndDir);
      break;

   case HDN_DIVIDERDBLCLICK:

      iCol = ColumnOfItem(hwndHdr, pnmh->iItem);
      if (iCol < 0)
         break;

      aColWidth[iCol] = 0;          // back to automatic
      ColumnsSave();
      DirRecalcColumns(hwndDir);
      RefreshAllDirWindows(hwndDir);
      break;

   case HDN_ITEMCLICK:
   {
      HWND hwndListParms = (HWND)GetWindowLongPtr(hwndDir, GWL_LISTPARMS);
      UINT idSort = 0;

      switch (ColumnOfItem(hwndHdr, pnmh->iItem)) {
      case COL_NAME:
      case COL_DOSNAME: idSort = IDM_BYNAME; break;
      case COL_SIZE:    idSort = IDM_BYSIZE; break;
      case COL_DATE:
      case COL_TIME:    idSort = IDM_BYDATE; break;
      }

      if (idSort && hwndListParms) {
         // sort commands act on the active window
         if ((HWND)SendMessage(hwndMDIClient, WM_MDIGETACTIVE, 0, 0) != hwndListParms)
            SendMessage(hwndMDIClient, WM_MDIACTIVATE, (WPARAM)hwndListParms, 0);
         SendMessage(hwndFrame, WM_COMMAND, MAKEWPARAM(idSort, 0), 0);
      }
      break;
   }
   }

   return 0;
}
