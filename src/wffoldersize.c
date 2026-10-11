/********************************************************************

   wffoldersize.c

   Background calculation of total folder sizes.

   When a directory window is filled, a worker thread walks every
   sub-folder shown in the listing and posts its total size back to
   the window, which then shows it in the Size column instead of
   <DIR>, includes it in the status bar totals and in sort-by-size.

   Design notes
   - Only the UI thread touches XDTA entries and the result cache. The
     worker owns private copies of the folder names and talks back only
     through PostMessage.
   - Every job has a generation number. Results from a cancelled or
     superseded job are dropped when they arrive.
   - Junctions and symbolic links are never followed (no double counting,
     no cycles). Folders that cannot be read add 0 and are skipped.
   - A small cache keyed by full path lets a refreshed or revisited
     folder show its last known size immediately while it is recomputed.

   Licensed under the MIT License.

********************************************************************/

#include "winfile.h"
#include "wffoldersize.h"

BOOL  bFolderSizes      = TRUE;
WCHAR szFolderSizes[]   = L"FolderSizes";

static const WCHAR szJobProp[] = L"WinfileFolderSizeJob";

static VOID TreeSizeInvalidate(HWND hwndMDIChild);
static VOID TreeSizeCancelAll(VOID);
static DWORD dwNextGen = 1;

typedef struct _FSJOB {
   LONG          cRef;
   volatile LONG fCancel;
   DWORD         dwGen;
   HWND          hwnd;
   UINT          cNames;
   LPWSTR        pszDir;        // with trailing backslash
   LPWSTR*       apszNames;
} FSJOB, *PFSJOB;

typedef struct _FSRESULT {
   LARGE_INTEGER qSize;
   WCHAR         szName[1];
} FSRESULT, *PFSRESULT;


/////////////////////////////////////////////////////////////////////
//
// Path -> size cache (UI thread only)
//
/////////////////////////////////////////////////////////////////////

#define CACHE_BUCKETS   1024
#define CACHE_MAX       16384

typedef struct _FSCACHE {
   struct _FSCACHE* next;
   LONGLONG qSize;
   DWORD    dwHash;
   WCHAR    szPath[1];
} FSCACHE, *PFSCACHE;

static PFSCACHE aCache[CACHE_BUCKETS];
static UINT cCache;

static DWORD
CacheHash(LPCWSTR psz)
{
   DWORD h = 2166136261u;
   for (; *psz; psz++) {
      WCHAR ch = (WCHAR)(ULONG_PTR)CharUpperW((LPWSTR)(ULONG_PTR)*psz);
      h = (h ^ ch) * 16777619u;
   }
   return h;
}

static VOID
CacheClear(VOID)
{
   UINT i;
   for (i = 0; i < CACHE_BUCKETS; i++) {
      while (aCache[i]) {
         PFSCACHE p = aCache[i];
         aCache[i] = p->next;
         LocalFree(p);
      }
   }
   cCache = 0;
}

static PFSCACHE
CacheFind(LPCWSTR pszPath, DWORD dwHash)
{
   PFSCACHE p;
   for (p = aCache[dwHash % CACHE_BUCKETS]; p; p = p->next) {
      if (p->dwHash == dwHash && !lstrcmpiW(p->szPath, pszPath))
         return p;
   }
   return NULL;
}

static BOOL
CacheLookup(LPCWSTR pszPath, LONGLONG* pqSize)
{
   PFSCACHE p = CacheFind(pszPath, CacheHash(pszPath));
   if (!p || p->qSize < 0)          // missing, or only queued by the tree
      return FALSE;
   *pqSize = p->qSize;
   return TRUE;
}

static VOID
CacheStore(LPCWSTR pszPath, LONGLONG qSize)
{
   DWORD dwHash = CacheHash(pszPath);
   PFSCACHE p = CacheFind(pszPath, dwHash);
   SIZE_T cch;

   if (p) {
      p->qSize = qSize;
      return;
   }

   if (cCache >= CACHE_MAX)
      CacheClear();

   cch = lstrlenW(pszPath);
   p = (PFSCACHE)LocalAlloc(LMEM_FIXED, sizeof(FSCACHE) + cch * sizeof(WCHAR));
   if (!p)
      return;

   p->qSize = qSize;
   p->dwHash = dwHash;
   lstrcpyW(p->szPath, pszPath);
   p->next = aCache[dwHash % CACHE_BUCKETS];
   aCache[dwHash % CACHE_BUCKETS] = p;
   cCache++;
}


/////////////////////////////////////////////////////////////////////
//
// Recursive size calculation (any thread)
//
/////////////////////////////////////////////////////////////////////

typedef struct _PATHSTACK {
   LPWSTR* ap;
   UINT    c;
   UINT    cMax;
} PATHSTACK;

static BOOL
StackPush(PATHSTACK* ps, LPCWSTR pszBase, LPCWSTR pszName)
{
   SIZE_T cchBase = lstrlenW(pszBase);
   SIZE_T cchName = pszName ? lstrlenW(pszName) : 0;
   LPWSTR psz;

   if (ps->c == ps->cMax) {
      UINT cNew = ps->cMax ? ps->cMax * 2 : 64;
      LPWSTR* apNew = (LPWSTR*)LocalAlloc(LMEM_FIXED, cNew * sizeof(LPWSTR));
      if (!apNew)
         return FALSE;
      if (ps->ap) {
         CopyMemory(apNew, ps->ap, ps->c * sizeof(LPWSTR));
         LocalFree(ps->ap);
      }
      ps->ap = apNew;
      ps->cMax = cNew;
   }

   psz = (LPWSTR)LocalAlloc(LMEM_FIXED, (cchBase + cchName + 2) * sizeof(WCHAR));
   if (!psz)
      return FALSE;

   lstrcpyW(psz, pszBase);
   if (pszName) {
      if (cchBase && psz[cchBase - 1] != L'\\')
         psz[cchBase++] = L'\\';
      lstrcpyW(psz + cchBase, pszName);
   }

   ps->ap[ps->c++] = psz;
   return TRUE;
}

static VOID
StackFree(PATHSTACK* ps)
{
   while (ps->c)
      LocalFree(ps->ap[--ps->c]);
   if (ps->ap)
      LocalFree(ps->ap);
   ps->ap = NULL;
   ps->cMax = 0;
}

static HANDLE
FindFirstFast(LPCWSTR pszPattern, WIN32_FIND_DATAW* pfd)
{
   static LONG fBasicUnsupported = FALSE;
   HANDLE h;

   if (!fBasicUnsupported) {
      // FindExInfoBasic (1) and FIND_FIRST_EX_LARGE_FETCH (2) need Windows 7+
      h = FindFirstFileExW(pszPattern, (FINDEX_INFO_LEVELS)1, pfd,
                           FindExSearchNameMatch, NULL, 2);
      if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_INVALID_PARAMETER)
         return h;
      InterlockedExchange(&fBasicUnsupported, TRUE);
   }

   return FindFirstFileW(pszPattern, pfd);
}

BOOL
FolderSizeCompute(LPCWSTR pszPath, PLARGE_INTEGER pqSize, volatile LONG* pfCancel)
{
   PATHSTACK stack = { 0 };
   WIN32_FIND_DATAW fd;
   WCHAR szRoot[MAXPATHLEN + 8];
   LONGLONG qTotal = 0;
   BOOL bCancelled = FALSE;

   pqSize->QuadPart = 0;

   //
   // Use the \\?\ form so paths deeper than MAX_PATH are still counted.
   //
   if (pszPath[0] == L'\\' && pszPath[1] == L'\\') {
      if (pszPath[2] == L'?' || pszPath[2] == L'.')
         StringCchCopyW(szRoot, COUNTOF(szRoot), pszPath);
      else
         StringCchPrintfW(szRoot, COUNTOF(szRoot), L"\\\\?\\UNC\\%s", pszPath + 2);
   } else {
      StringCchPrintfW(szRoot, COUNTOF(szRoot), L"\\\\?\\%s", pszPath);
   }

   // strip a trailing backslash (but keep "X:\")
   {
      SIZE_T cch = lstrlenW(szRoot);
      if (cch > 7 && szRoot[cch - 1] == L'\\' && szRoot[cch - 2] != L':')
         szRoot[cch - 1] = CHAR_NULL;
   }

   if (!StackPush(&stack, szRoot, NULL))
      return TRUE;

   while (stack.c) {
      LPWSTR pszDir = stack.ap[--stack.c];
      LPWSTR pszPattern;
      SIZE_T cchDir = lstrlenW(pszDir);
      HANDLE hFind;

      pszPattern = (LPWSTR)LocalAlloc(LMEM_FIXED, (cchDir + 3) * sizeof(WCHAR));
      if (!pszPattern) {
         LocalFree(pszDir);
         continue;
      }
      lstrcpyW(pszPattern, pszDir);
      if (cchDir && pszPattern[cchDir - 1] != L'\\')
         pszPattern[cchDir++] = L'\\';
      pszPattern[cchDir++] = L'*';
      pszPattern[cchDir] = CHAR_NULL;

      hFind = FindFirstFast(pszPattern, &fd);
      LocalFree(pszPattern);

      if (hFind != INVALID_HANDLE_VALUE) {
         do {
            if (pfCancel && *pfCancel) {
               bCancelled = TRUE;
               break;
            }

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
               if (fd.cFileName[0] == L'.' &&
                   (fd.cFileName[1] == CHAR_NULL ||
                    (fd.cFileName[1] == L'.' && fd.cFileName[2] == CHAR_NULL)))
                  continue;

               if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                  continue;   // junction / directory symlink / mount point

               StackPush(&stack, pszDir, fd.cFileName);
            } else {
               ULARGE_INTEGER q;
               q.LowPart = fd.nFileSizeLow;
               q.HighPart = fd.nFileSizeHigh;
               qTotal += (LONGLONG)q.QuadPart;
            }
         } while (FindNextFileW(hFind, &fd));

         FindClose(hFind);
      }

      LocalFree(pszDir);

      if (bCancelled || (pfCancel && *pfCancel)) {
         bCancelled = TRUE;
         break;
      }
   }

   StackFree(&stack);

   pqSize->QuadPart = qTotal;
   return !bCancelled;
}


/////////////////////////////////////////////////////////////////////
//
// Worker thread
//
/////////////////////////////////////////////////////////////////////

static VOID
JobRelease(PFSJOB pJob)
{
   UINT i;

   if (InterlockedDecrement(&pJob->cRef) != 0)
      return;

   for (i = 0; i < pJob->cNames; i++)
      LocalFree(pJob->apszNames[i]);

   LocalFree(pJob->apszNames);
   LocalFree(pJob->pszDir);
   LocalFree(pJob);
}

static DWORD WINAPI
FolderSizeThread(LPVOID lpParam)
{
   PFSJOB pJob = (PFSJOB)lpParam;
   WCHAR szPath[MAXPATHLEN * 2];
   UINT i;

   SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

   for (i = 0; i < pJob->cNames && !pJob->fCancel; i++) {

      LARGE_INTEGER qSize;
      PFSRESULT pRes;
      SIZE_T cchName;

      if (FAILED(StringCchPrintfW(szPath, COUNTOF(szPath), L"%s%s", pJob->pszDir, pJob->apszNames[i])))
         continue;

      if (!FolderSizeCompute(szPath, &qSize, &pJob->fCancel))
         break;

      cchName = lstrlenW(pJob->apszNames[i]);
      pRes = (PFSRESULT)LocalAlloc(LMEM_FIXED, sizeof(FSRESULT) + cchName * sizeof(WCHAR));
      if (!pRes)
         continue;

      pRes->qSize = qSize;
      lstrcpyW(pRes->szName, pJob->apszNames[i]);

      if (!PostMessage(pJob->hwnd, FS_FOLDERSIZERESULT, (WPARAM)pJob->dwGen, (LPARAM)pRes))
         LocalFree(pRes);
   }

   if (!pJob->fCancel)
      PostMessage(pJob->hwnd, FS_FOLDERSIZERESULT, (WPARAM)pJob->dwGen, 0);

   JobRelease(pJob);
   return 0;
}


/////////////////////////////////////////////////////////////////////
//
// UI thread side
//
/////////////////////////////////////////////////////////////////////

BOOL
FolderSizeIsKnown(LPXDTA lpxdta)
{
   return bFolderSizes &&
          lpxdta &&
          (lpxdta->dwAttrs & ATTR_DIR) &&
          !(lpxdta->dwAttrs & ATTR_PARENT) &&
          lpxdta->bySizeState != FOLDERSIZE_UNKNOWN;
}

LONGLONG
FolderSizeOfItem(LPXDTA lpxdta)
{
   if (!lpxdta)
      return 0;

   if (lpxdta->dwAttrs & ATTR_DIR)
      return FolderSizeIsKnown(lpxdta) ? lpxdta->qFileSize.QuadPart : 0;

   return lpxdta->qFileSize.QuadPart;
}

LONGLONG
FolderSizeListingTotal(LPXDTALINK lpStart)
{
   LPXDTAHEAD lpHead;
   LONGLONG qTotal = 0;
   DWORD i;

   if (!bFolderSizes || !lpStart)
      return 0;

   lpHead = MemLinkToHead(lpStart);
   if (!lpHead->alpxdtaSorted)
      return 0;

   for (i = 0; i < lpHead->dwEntries; i++) {
      LPXDTA lpxdta = lpHead->alpxdtaSorted[i];
      if (FolderSizeIsKnown(lpxdta))
         qTotal += lpxdta->qFileSize.QuadPart;
   }

   return qTotal;
}

static BOOL
IsCountableFolder(LPXDTA lpxdta)
{
   return lpxdta &&
          (lpxdta->dwAttrs & ATTR_DIR) &&
          !(lpxdta->dwAttrs & (ATTR_PARENT | ATTR_JUNCTION | ATTR_SYMBOLIC | ATTR_REPARSE_POINT));
}

VOID
FolderSizeRequest(HWND hwndDir)
{
   PostMessage(hwndDir, FS_FOLDERSIZESTART, 0, 0);
}

VOID
FolderSizeCancel(HWND hwndDir)
{
   PFSJOB pJob = (PFSJOB)RemoveProp(hwndDir, szJobProp);

   if (pJob) {
      InterlockedExchange(&pJob->fCancel, TRUE);
      JobRelease(pJob);
   }
}

VOID
FolderSizeStart(HWND hwndDir)
{
   LPXDTALINK lpStart;
   LPXDTAHEAD lpHead;
   PFSJOB pJob;
   HANDLE hThread;
   WCHAR szDir[MAXPATHLEN];
   WCHAR szPath[MAXPATHLEN * 2];
   DWORD i;
   UINT cNames = 0;
   BOOL bChanged = FALSE;
   HWND hwndLB;

   FolderSizeCancel(hwndDir);

   if (!bFolderSizes)
      return;

   if (GetWindowLongPtr(hwndDir, GWL_IERROR))
      return;

   lpStart = (LPXDTALINK)GetWindowLongPtr(hwndDir, GWL_HDTA);
   if (!lpStart)
      return;

   lpHead = MemLinkToHead(lpStart);
   if (!lpHead->alpxdtaSorted || !lpHead->dwEntries)
      return;

   SendMessage(hwndDir, FS_GETDIRECTORY, COUNTOF(szDir), (LPARAM)szDir);
   if (!szDir[0])
      return;

   pJob = (PFSJOB)LocalAlloc(LPTR, sizeof(FSJOB));
   if (!pJob)
      return;

   pJob->apszNames = (LPWSTR*)LocalAlloc(LPTR, lpHead->dwEntries * sizeof(LPWSTR));
   pJob->pszDir = (LPWSTR)LocalAlloc(LMEM_FIXED, (lstrlenW(szDir) + 1) * sizeof(WCHAR));

   if (!pJob->apszNames || !pJob->pszDir) {
      pJob->cRef = 1;
      JobRelease(pJob);
      return;
   }

   lstrcpyW(pJob->pszDir, szDir);

   for (i = 0; i < lpHead->dwEntries; i++) {

      LPXDTA lpxdta = lpHead->alpxdtaSorted[i];
      LPWSTR pszName;
      LONGLONG qCached;

      if (!IsCountableFolder(lpxdta) || lpxdta->bySizeState == FOLDERSIZE_FINAL)
         continue;

      pszName = MemGetFileName(lpxdta);

      if (lpxdta->bySizeState == FOLDERSIZE_UNKNOWN &&
          SUCCEEDED(StringCchPrintfW(szPath, COUNTOF(szPath), L"%s%s", szDir, pszName)) &&
          CacheLookup(szPath, &qCached)) {

         lpxdta->qFileSize.QuadPart = qCached;
         lpxdta->bySizeState = FOLDERSIZE_CACHED;
         bChanged = TRUE;
      }

      pJob->apszNames[cNames] = (LPWSTR)LocalAlloc(LMEM_FIXED, (lstrlenW(pszName) + 1) * sizeof(WCHAR));
      if (pJob->apszNames[cNames]) {
         lstrcpyW(pJob->apszNames[cNames], pszName);
         cNames++;
      }
   }

   pJob->cNames = cNames;

   if (bChanged) {
      if (hwndLB = GetDlgItem(hwndDir, IDCW_LISTBOX))
         InvalidateRect(hwndLB, NULL, FALSE);
      UpdateStatus(GetParent(hwndDir));
   }

   if (!cNames) {
      pJob->cRef = 1;
      JobRelease(pJob);
      return;
   }

   pJob->hwnd = hwndDir;
   pJob->dwGen = dwNextGen++;
   pJob->cRef = 2;      // one for the window, one for the thread

   SetProp(hwndDir, szJobProp, (HANDLE)pJob);

   hThread = CreateThread(NULL, 0, FolderSizeThread, pJob, 0, NULL);
   if (hThread) {
      CloseHandle(hThread);
   } else {
      JobRelease(pJob);          // thread's reference
      FolderSizeCancel(hwndDir); // window's reference
   }
}

VOID
FolderSizeOnResult(HWND hwndDir, WPARAM wParam, LPARAM lParam)
{
   PFSRESULT pRes = (PFSRESULT)lParam;
   PFSJOB pJob = (PFSJOB)GetProp(hwndDir, szJobProp);
   HWND hwndLB = GetDlgItem(hwndDir, IDCW_LISTBOX);
   HWND hwndParent = GetParent(hwndDir);
   WCHAR szPath[MAXPATHLEN * 2];
   INT i, cItems;

   if (!pJob || pJob->dwGen != (DWORD)wParam) {
      if (pRes)
         LocalFree(pRes);
      return;
   }

   if (!pRes) {

      //
      // Job finished. Re-sort once if the user sorts by size, since the
      // folder sizes changed after the list was sorted.
      //
      FolderSizeCancel(hwndDir);

      if ((DWORD)GetWindowLongPtr(hwndParent, GWL_SORT) == IDD_SIZE)
         SendMessage(hwndDir, FS_CHANGEDISPLAY, CD_SORT, MAKELONG(IDD_SIZE, 0));

      return;
   }

   if (SUCCEEDED(StringCchPrintfW(szPath, COUNTOF(szPath), L"%s%s", pJob->pszDir, pRes->szName)))
      CacheStore(szPath, pRes->qSize.QuadPart);

   if (hwndLB) {

      cItems = (INT)SendMessage(hwndLB, LB_GETCOUNT, 0, 0L);

      for (i = 0; i < cItems; i++) {

         LPXDTA lpxdta = NULL;

         if (SendMessage(hwndLB, LB_GETTEXT, i, (LPARAM)&lpxdta) == LB_ERR || !lpxdta)
            continue;

         if (IsCountableFolder(lpxdta) && !lstrcmpiW(MemGetFileName(lpxdta), pRes->szName)) {

            RECT rc;

            lpxdta->qFileSize = pRes->qSize;
            lpxdta->bySizeState = FOLDERSIZE_FINAL;

            if (SendMessage(hwndLB, LB_GETITEMRECT, i, (LPARAM)&rc) != LB_ERR)
               InvalidateRect(hwndLB, &rc, FALSE);
            break;
         }
      }
   }

   LocalFree(pRes);

   UpdateStatus(hwndParent);
   TreeSizeInvalidate(hwndParent);
}

VOID
FolderSizeInit(VOID)
{
   bFolderSizes = GetPrivateProfileInt(szSettings, szFolderSizes, bFolderSizes, szTheINIFile);
}

VOID
FolderSizeToggle(VOID)
{
   HWND hwnd;

   bFolderSizes = !bFolderSizes;
   WritePrivateProfileBool(szFolderSizes, bFolderSizes);

   TreeSizeCancelAll();

   for (hwnd = GetWindow(hwndMDIClient, GW_CHILD); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {

      HWND hwndDir = HasDirWindow(hwnd);
      HWND hwndLB;

      TreeSizeInvalidate(hwnd);

      if (!hwndDir)
         continue;

      if (bFolderSizes)
         FolderSizeStart(hwndDir);
      else
         FolderSizeCancel(hwndDir);

      if (hwndLB = GetDlgItem(hwndDir, IDCW_LISTBOX))
         InvalidateRect(hwndLB, NULL, FALSE);

      UpdateStatus(hwnd);
   }
}


/////////////////////////////////////////////////////////////////////
//
// Folder sizes in the tree (left pane)
//
// The tree asks for the size of each folder as it draws it. Requests go
// on a LIFO queue (most recently drawn = most likely visible first) that
// one background thread works through. Each walk also reports the sizes
// of sub-folders up to TREE_REPORT_DEPTH levels down, so expanding a
// branch usually shows its sizes immediately. Results land in the same
// path cache the directory window uses.
//
/////////////////////////////////////////////////////////////////////

#define TREE_REPORT_DEPTH    2
#define TREE_MAX_RESULTS     4096
#define PENDING_SIZE         ((LONGLONG)-1)

typedef struct _TREERES {
   struct _TREERES* next;
   LONGLONG qSize;
   WCHAR    szPath[1];
} TREERES, *PTREERES;

typedef struct _TREEBATCH {
   DWORD    dwGen;
   PTREERES pFirst;
} TREEBATCH, *PTREEBATCH;

static CRITICAL_SECTION csTree;
static BOOL     bTreeInit;
static HANDLE   hTreeEvent;
static LPWSTR*  apszTreeQueue;
static UINT     cTreeQueue, cTreeQueueMax;
static volatile LONG dwTreeGen = 1;

static VOID
TreeSizeInvalidate(HWND hwndMDIChild)
{
   HWND hwndTree = HasTreeWindow(hwndMDIChild);
   HWND hwndLB;

   if (hwndTree && (hwndLB = GetDlgItem(hwndTree, IDCW_TREELISTBOX)))
      InvalidateRect(hwndLB, NULL, FALSE);
}

static VOID
TreeSizeInvalidateAll(VOID)
{
   HWND hwnd;
   for (hwnd = GetWindow(hwndMDIClient, GW_CHILD); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT))
      TreeSizeInvalidate(hwnd);
}

// Turn "\\?\C:\x" / "\\?\UNC\srv\share\x" back into the normal form used as cache key.
static VOID
TreeKeyFromLong(LPCWSTR pszLong, LPWSTR pszKey, DWORD cch)
{
   if (!StrCmpNIW(pszLong, L"\\\\?\\UNC\\", 8)) {
      StringCchPrintfW(pszKey, cch, L"\\\\%s", pszLong + 8);
   } else if (!StrCmpNW(pszLong, L"\\\\?\\", 4)) {
      StringCchCopyW(pszKey, cch, pszLong + 4);
   } else {
      StringCchCopyW(pszKey, cch, pszLong);
   }
}

typedef struct _TREEWALK {
   WCHAR    szPath[32800];     // \\?\ long path being walked
   PTREERES pFirst;
   UINT     cResults;
   DWORD    dwGen;
} TREEWALK;

static LONGLONG
TreeWalk(TREEWALK* pw, SIZE_T cch, INT depth)
{
   WIN32_FIND_DATAW fd;
   HANDLE hFind;
   LONGLONG qTotal = 0;

   if (cch + 3 >= COUNTOF(pw->szPath))
      return 0;

   if (pw->szPath[cch - 1] != L'\\')
      pw->szPath[cch++] = L'\\';
   pw->szPath[cch] = L'*';
   pw->szPath[cch + 1] = CHAR_NULL;

   hFind = FindFirstFast(pw->szPath, &fd);
   pw->szPath[cch] = CHAR_NULL;

   if (hFind != INVALID_HANDLE_VALUE) {
      do {
         if ((DWORD)dwTreeGen != pw->dwGen)
            break;

         if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {

            SIZE_T cchName;

            if (fd.cFileName[0] == L'.' &&
                (fd.cFileName[1] == CHAR_NULL ||
                 (fd.cFileName[1] == L'.' && fd.cFileName[2] == CHAR_NULL)))
               continue;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
               continue;

            cchName = lstrlenW(fd.cFileName);
            if (cch + cchName + 2 >= COUNTOF(pw->szPath))
               continue;

            lstrcpyW(pw->szPath + cch, fd.cFileName);
            qTotal += TreeWalk(pw, cch + cchName, depth + 1);
            pw->szPath[cch] = CHAR_NULL;

         } else {
            ULARGE_INTEGER q;
            q.LowPart = fd.nFileSizeLow;
            q.HighPart = fd.nFileSizeHigh;
            qTotal += (LONGLONG)q.QuadPart;
         }
      } while (FindNextFileW(hFind, &fd));

      FindClose(hFind);
   }

   //
   // Report this folder (strip the trailing backslash we added).
   //
   if (depth <= TREE_REPORT_DEPTH && (depth == 0 || pw->cResults < TREE_MAX_RESULTS) &&
       (DWORD)dwTreeGen == pw->dwGen) {

      WCHAR szKey[MAXPATHLEN * 2];
      PTREERES pRes;
      SIZE_T cchKey;

      if (cch && pw->szPath[cch - 1] == L'\\')
         pw->szPath[cch - 1] = CHAR_NULL;

      TreeKeyFromLong(pw->szPath, szKey, COUNTOF(szKey));
      cchKey = lstrlenW(szKey);

      pRes = (PTREERES)LocalAlloc(LMEM_FIXED, sizeof(TREERES) + cchKey * sizeof(WCHAR));
      if (pRes) {
         pRes->qSize = qTotal;
         lstrcpyW(pRes->szPath, szKey);
         pRes->next = pw->pFirst;
         pw->pFirst = pRes;
         pw->cResults++;
      }

      if (cch)
         pw->szPath[cch - 1] = L'\\';
   }

   if (cch && pw->szPath[cch - 1] == L'\\')
      pw->szPath[cch - 1] = CHAR_NULL;

   return qTotal;
}

static VOID
FreeTreeResults(PTREERES p)
{
   while (p) {
      PTREERES pNext = p->next;
      LocalFree(p);
      p = pNext;
   }
}

static DWORD WINAPI
TreeSizeThread(LPVOID lpParam)
{
   TREEWALK* pw = (TREEWALK*)LocalAlloc(LMEM_FIXED, sizeof(TREEWALK));

   UNREFERENCED_PARAMETER(lpParam);

   if (!pw)
      return 0;

   SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

   for (;;) {

      LPWSTR pszPath = NULL;
      PTREEBATCH pBatch;
      SIZE_T cch;

      EnterCriticalSection(&csTree);
      if (cTreeQueue)
         pszPath = apszTreeQueue[--cTreeQueue];
      pw->dwGen = (DWORD)dwTreeGen;
      LeaveCriticalSection(&csTree);

      if (!pszPath) {
         WaitForSingleObject(hTreeEvent, INFINITE);
         continue;
      }

      if (pszPath[0] == L'\\' && pszPath[1] == L'\\')
         StringCchPrintfW(pw->szPath, COUNTOF(pw->szPath), L"\\\\?\\UNC\\%s", pszPath + 2);
      else
         StringCchPrintfW(pw->szPath, COUNTOF(pw->szPath), L"\\\\?\\%s", pszPath);
      LocalFree(pszPath);

      pw->pFirst = NULL;
      pw->cResults = 0;

      cch = lstrlenW(pw->szPath);
      while (cch > 7 && pw->szPath[cch - 1] == L'\\')
         pw->szPath[--cch] = CHAR_NULL;

      TreeWalk(pw, cch, 0);

      if ((DWORD)dwTreeGen != pw->dwGen || !pw->pFirst) {
         FreeTreeResults(pw->pFirst);
         continue;
      }

      pBatch = (PTREEBATCH)LocalAlloc(LMEM_FIXED, sizeof(TREEBATCH));
      if (!pBatch) {
         FreeTreeResults(pw->pFirst);
         continue;
      }
      pBatch->dwGen = pw->dwGen;
      pBatch->pFirst = pw->pFirst;

      if (!PostMessage(hwndFrame, FS_TREESIZERESULT, 0, (LPARAM)pBatch)) {
         FreeTreeResults(pBatch->pFirst);
         LocalFree(pBatch);
      }
   }
}

static BOOL
TreeSizeEnsureThread(VOID)
{
   HANDLE hThread;

   if (bTreeInit)
      return TRUE;

   InitializeCriticalSection(&csTree);
   hTreeEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
   if (!hTreeEvent)
      return FALSE;

   // deep folder trees recurse; reserve a generous stack
   hThread = CreateThread(NULL, 8 * 1024 * 1024, TreeSizeThread, NULL,
                          STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
   if (!hThread)
      return FALSE;

   CloseHandle(hThread);
   bTreeInit = TRUE;
   return TRUE;
}

static VOID
TreeSizeCancelAll(VOID)
{
   UINT i;

   InterlockedIncrement(&dwTreeGen);

   if (!bTreeInit)
      return;

   EnterCriticalSection(&csTree);
   for (i = 0; i < cTreeQueue; i++)
      LocalFree(apszTreeQueue[i]);
   cTreeQueue = 0;
   LeaveCriticalSection(&csTree);

   // forget the "queued" markers so folders get asked for again
   {
      UINT b;
      for (b = 0; b < CACHE_BUCKETS; b++) {
         PFSCACHE* pp = &aCache[b];
         while (*pp) {
            if ((*pp)->qSize == PENDING_SIZE) {
               PFSCACHE p = *pp;
               *pp = p->next;
               LocalFree(p);
               cCache--;
            } else {
               pp = &(*pp)->next;
            }
         }
      }
   }
}

BOOL
TreeSizeGet(LPCWSTR pszPath, PLARGE_INTEGER pqSize)
{
   PFSCACHE p;
   DWORD dwHash;

   if (!bFolderSizes || !pszPath[0])
      return FALSE;

   dwHash = CacheHash(pszPath);
   p = CacheFind(pszPath, dwHash);

   if (p) {
      if (p->qSize == PENDING_SIZE)
         return FALSE;
      pqSize->QuadPart = p->qSize;
      return TRUE;
   }

   //
   // Not known yet: queue it and remember that we did.
   //
   if (!TreeSizeEnsureThread())
      return FALSE;

   {
      LPWSTR psz = (LPWSTR)LocalAlloc(LMEM_FIXED, (lstrlenW(pszPath) + 1) * sizeof(WCHAR));
      if (!psz)
         return FALSE;
      lstrcpyW(psz, pszPath);

      EnterCriticalSection(&csTree);
      if (cTreeQueue == cTreeQueueMax) {
         UINT cNew = cTreeQueueMax ? cTreeQueueMax * 2 : 64;
         LPWSTR* apNew = (LPWSTR*)LocalAlloc(LMEM_FIXED, cNew * sizeof(LPWSTR));
         if (!apNew) {
            LeaveCriticalSection(&csTree);
            LocalFree(psz);
            return FALSE;
         }
         if (apszTreeQueue) {
            CopyMemory(apNew, apszTreeQueue, cTreeQueue * sizeof(LPWSTR));
            LocalFree(apszTreeQueue);
         }
         apszTreeQueue = apNew;
         cTreeQueueMax = cNew;
      }
      apszTreeQueue[cTreeQueue++] = psz;
      LeaveCriticalSection(&csTree);
   }

   CacheStore(pszPath, PENDING_SIZE);
   SetEvent(hTreeEvent);
   return FALSE;
}

VOID
TreeSizeOnResult(LPARAM lParam)
{
   PTREEBATCH pBatch = (PTREEBATCH)lParam;
   PTREERES p;

   if (pBatch->dwGen == (DWORD)dwTreeGen && bFolderSizes) {
      for (p = pBatch->pFirst; p; p = p->next)
         CacheStore(p->szPath, p->qSize);
      TreeSizeInvalidateAll();
   }

   FreeTreeResults(pBatch->pFirst);
   LocalFree(pBatch);
}

VOID
FolderSizeFlush(VOID)
{
   HWND hwnd;

   TreeSizeCancelAll();
   CacheClear();

   for (hwnd = GetWindow(hwndMDIClient, GW_CHILD); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT))
      TreeSizeInvalidate(hwnd);
}
