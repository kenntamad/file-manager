/********************************************************************

   wfupdate.c

   Automatic updates from GitHub Releases.

   Flow
   1. AutoUpdateInit() runs at startup. If automatic updates are on and no
      check happened today, a background thread asks the GitHub API for
      the latest release of WF_UPDATE_REPO (or UpdateRepo= in winfile.ini).
   2. If the release tag is a newer version than this exe, the user is
      asked whether to install it. Declining an automatic prompt skips
      that version until the next manual check (Help > Check for Updates).
   3. The matching .exe asset (winfile-x64.exe / winfile-x86.exe /
      winfile-arm64.exe, else any .exe) is downloaded next to the running
      exe as winfile.exe.new and verified: size, "MZ" header and, when
      GitHub publishes one, its SHA-256 digest.
   4. On confirmation the running exe is renamed to *.old (Windows allows
      renaming a running exe), the new file takes its place, the new exe
      is started with /wfupdated:<pid> so it waits for this process to
      exit, and this instance closes normally. The new instance deletes
      the *.old file.

   Licensed under the MIT License.

********************************************************************/

#include "winfile.h"
#include "wfupdate.h"
#include "wfupdate_repo.h"
#include <winhttp.h>
#include <bcrypt.h>

#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "version.lib")
#endif

BOOL  bAutoUpdate           = TRUE;
WCHAR szAutoUpdate[]        = L"AutoUpdate";

static WCHAR szUpdateRepoKey[]   = L"UpdateRepo";
static WCHAR szLastCheckKey[]    = L"LastUpdateCheck";
static WCHAR szSkipVersionKey[]  = L"SkipUpdateVersion";

static const WCHAR szUpdatedSwitch[] = L"/wfupdated:";

#if defined(_M_ARM64) || defined(__aarch64__)
static const char szAssetArch[] = "winfile-arm64.exe";
#elif defined(_M_X64) || defined(__x86_64__)
static const char szAssetArch[] = "winfile-x64.exe";
#else
static const char szAssetArch[] = "winfile-x86.exe";
#endif

static const WCHAR szUpdTitle[] = L"File Manager Update";

static volatile LONG fUpdateBusy = FALSE;

enum {
   UPD_UPTODATE = 0,
   UPD_NEWER,
   UPD_NOTCONFIGURED,
   UPD_NORELEASE,
   UPD_NOASSET,
   UPD_NETERROR,
   UPD_BADDATA,
   UPD_DOWNLOADED,
   UPD_NOTWRITABLE,
   UPD_VERIFYFAILED,
};

typedef struct _UPDATEINFO {
   BOOL     bInteractive;
   INT      iStatus;
   DWORD    dwError;
   WORD     awCurrent[4];
   WORD     awLatest[4];
   WCHAR    szRepo[256];
   WCHAR    szTag[64];
   WCHAR    szAssetUrl[2048];
   WCHAR    szReleaseUrl[1024];
   LONGLONG cbAsset;
   BOOL     bHasDigest;
   BYTE     abSha256[32];
   WCHAR    szNewFile[MAXPATHLEN];
} UPDATEINFO, *PUPDATEINFO;


/////////////////////////////////////////////////////////////////////
//
// Small helpers
//
/////////////////////////////////////////////////////////////////////

static VOID
GetRepo(LPWSTR pszRepo, DWORD cch)
{
   GetPrivateProfileString(szSettings, szUpdateRepoKey, WF_UPDATE_REPO, pszRepo, cch, szTheINIFile);

   // Accept a full URL too: https://github.com/owner/name(.git)
   {
      static const WCHAR szGitHub[] = L"github.com/";
      LPWSTR p = StrStrIW(pszRepo, szGitHub);
      SIZE_T cchRepo;

      if (p)
         MoveMemory(pszRepo, p + COUNTOF(szGitHub) - 1, (lstrlen(p + COUNTOF(szGitHub) - 1) + 1) * sizeof(WCHAR));

      cchRepo = lstrlen(pszRepo);
      while (cchRepo && (pszRepo[cchRepo - 1] == L'/' || pszRepo[cchRepo - 1] == L' '))
         pszRepo[--cchRepo] = CHAR_NULL;
      if (cchRepo > 4 && !lstrcmpi(pszRepo + cchRepo - 4, L".git"))
         pszRepo[cchRepo - 4] = CHAR_NULL;
   }
}

static DWORD
TodayNumber(VOID)
{
   FILETIME ft;
   ULARGE_INTEGER u;

   GetSystemTimeAsFileTime(&ft);
   u.LowPart = ft.dwLowDateTime;
   u.HighPart = ft.dwHighDateTime;
   return (DWORD)(u.QuadPart / (10000000ULL * 60 * 60 * 24));
}

static BOOL
GetOwnVersion(WORD aw[4])
{
   WCHAR szExe[MAXPATHLEN];
   DWORD dwHandle, cb;
   LPVOID pv;
   VS_FIXEDFILEINFO* pffi;
   UINT cbffi;
   BOOL bRet = FALSE;

   ZeroMemory(aw, 4 * sizeof(WORD));

   if (!GetModuleFileName(NULL, szExe, COUNTOF(szExe)))
      return FALSE;

   cb = GetFileVersionInfoSize(szExe, &dwHandle);
   if (!cb)
      return FALSE;

   pv = LocalAlloc(LMEM_FIXED, cb);
   if (!pv)
      return FALSE;

   if (GetFileVersionInfo(szExe, 0, cb, pv) &&
       VerQueryValue(pv, L"\\", (LPVOID*)&pffi, &cbffi) && cbffi >= sizeof(*pffi)) {

      aw[0] = HIWORD(pffi->dwFileVersionMS);
      aw[1] = LOWORD(pffi->dwFileVersionMS);
      aw[2] = HIWORD(pffi->dwFileVersionLS);
      aw[3] = LOWORD(pffi->dwFileVersionLS);
      bRet = TRUE;
   }

   LocalFree(pv);
   return bRet;
}

// "v10.4.1", "10.4.1.0", "release-10.5" -> {10,4,1,0}
static BOOL
ParseVersion(LPCWSTR psz, WORD aw[4])
{
   INT i = 0;

   ZeroMemory(aw, 4 * sizeof(WORD));

   while (*psz && (*psz < L'0' || *psz > L'9'))
      psz++;

   if (!*psz)
      return FALSE;

   while (*psz && i < 4) {
      DWORD n = 0;
      if (*psz < L'0' || *psz > L'9')
         break;
      while (*psz >= L'0' && *psz <= L'9') {
         n = n * 10 + (*psz - L'0');
         if (n > 0xFFFF)
            n = 0xFFFF;
         psz++;
      }
      aw[i++] = (WORD)n;
      if (*psz != L'.')
         break;
      psz++;
   }

   return i > 0;
}

static INT
CompareVersion(const WORD a[4], const WORD b[4])
{
   INT i;
   for (i = 0; i < 4; i++) {
      if (a[i] != b[i])
         return a[i] < b[i] ? -1 : 1;
   }
   return 0;
}

static VOID
FormatVersion(const WORD aw[4], LPWSTR psz, DWORD cch)
{
   StringCchPrintf(psz, cch, L"%u.%u.%u.%u", aw[0], aw[1], aw[2], aw[3]);
}


/////////////////////////////////////////////////////////////////////
//
// Minimal JSON reading (just enough for the GitHub release object)
//
/////////////////////////////////////////////////////////////////////

static const char*
JsonWs(const char* p)
{
   while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
      p++;
   return p;
}

static const char*
JsonSkipString(const char* p)
{
   if (*p != '"')
      return NULL;
   for (p++; *p; p++) {
      if (*p == '\\') {
         if (!p[1])
            return NULL;
         p++;
      } else if (*p == '"') {
         return p + 1;
      }
   }
   return NULL;
}

static const char*
JsonSkipValue(const char* p)
{
   p = JsonWs(p);

   if (*p == '"')
      return JsonSkipString(p);

   if (*p == '{' || *p == '[') {
      INT depth = 0;
      while (*p) {
         if (*p == '"') {
            p = JsonSkipString(p);
            if (!p)
               return NULL;
            continue;
         }
         if (*p == '{' || *p == '[')
            depth++;
         else if (*p == '}' || *p == ']') {
            if (--depth == 0)
               return p + 1;
         }
         p++;
      }
      return NULL;
   }

   while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\r' && *p != '\n' && *p != '\t')
      p++;
   return p;
}

// pObj points at '{'. Returns the value of a top level key, or NULL.
static const char*
JsonFind(const char* pObj, const char* pszKey)
{
   const char* p;
   SIZE_T cchKey = lstrlenA(pszKey);

   if (!pObj || *pObj != '{')
      return NULL;

   p = pObj + 1;

   for (;;) {
      const char* pKey;
      const char* pEnd;

      p = JsonWs(p);
      if (*p != '"')
         return NULL;

      pKey = p + 1;
      pEnd = JsonSkipString(p);
      if (!pEnd)
         return NULL;

      p = JsonWs(pEnd);
      if (*p != ':')
         return NULL;
      p = JsonWs(p + 1);

      if ((SIZE_T)(pEnd - 1 - pKey) == cchKey && !memcmp(pKey, pszKey, cchKey))
         return p;

      p = JsonSkipValue(p);
      if (!p)
         return NULL;
      p = JsonWs(p);
      if (*p != ',')
         return NULL;
      p++;
   }
}

static BOOL
JsonString(const char* pVal, LPWSTR pszOut, DWORD cchOut)
{
   char sz[4096];
   DWORD i = 0;

   if (!pVal || *pVal != '"' || !cchOut)
      return FALSE;

   for (pVal++; *pVal && *pVal != '"' && i < sizeof(sz) - 1; pVal++) {
      char ch = *pVal;
      if (ch == '\\') {
         pVal++;
         switch (*pVal) {
         case 'n': ch = '\n'; break;
         case 't': ch = '\t'; break;
         case 'r': ch = '\r'; break;
         case 'b': ch = '\b'; break;
         case 'f': ch = '\f'; break;
         case 'u':
            // not needed for tags/URLs; keep a placeholder
            ch = '?';
            if (pVal[1] && pVal[2] && pVal[3] && pVal[4])
               pVal += 4;
            break;
         case 0:
            return FALSE;
         default:
            ch = *pVal;        // \" \\ \/
            break;
         }
      }
      sz[i++] = ch;
   }
   sz[i] = 0;

   return MultiByteToWideChar(CP_UTF8, 0, sz, -1, pszOut, cchOut) > 0;
}

static LONGLONG
JsonInt(const char* pVal)
{
   LONGLONG n = 0;
   if (!pVal)
      return -1;
   if (*pVal < '0' || *pVal > '9')
      return -1;
   while (*pVal >= '0' && *pVal <= '9')
      n = n * 10 + (*pVal++ - '0');
   return n;
}

static INT
HexNibble(WCHAR ch)
{
   if (ch >= L'0' && ch <= L'9') return ch - L'0';
   if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
   if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
   return -1;
}

static BOOL
ParseSha256Digest(LPCWSTR psz, BYTE ab[32])
{
   INT i;

   if (StrCmpNIW(psz, L"sha256:", 7))
      return FALSE;
   psz += 7;

   for (i = 0; i < 32; i++) {
      INT hi = HexNibble(psz[2 * i]);
      INT lo = hi < 0 ? -1 : HexNibble(psz[2 * i + 1]);
      if (lo < 0)
         return FALSE;
      ab[i] = (BYTE)((hi << 4) | lo);
   }
   return psz[64] == CHAR_NULL;
}


/////////////////////////////////////////////////////////////////////
//
// HTTP
//
/////////////////////////////////////////////////////////////////////

static HINTERNET
HttpOpenSession(VOID)
{
   HINTERNET hSession;
   DWORD dwProtocols;
   WCHAR szAgent[64];
   WORD aw[4];

   GetOwnVersion(aw);
   StringCchPrintf(szAgent, COUNTOF(szAgent), L"WinfileUpdater/%u.%u.%u.%u", aw[0], aw[1], aw[2], aw[3]);

   // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY (Windows 8.1+), else the classic default
   hSession = WinHttpOpen(szAgent, 4, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
   if (!hSession)
      hSession = WinHttpOpen(szAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
   if (!hSession)
      return NULL;

   // Make sure TLS 1.2 (and 1.3 where available) is on, needed for GitHub on Windows 7/8.
   dwProtocols = 0x00000800 | 0x00002000;   // TLS1_2 | TLS1_3
   if (!WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &dwProtocols, sizeof(dwProtocols))) {
      dwProtocols = 0x00000800;
      WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &dwProtocols, sizeof(dwProtocols));
   }

   WinHttpSetTimeouts(hSession, 15000, 15000, 30000, 60000);

   return hSession;
}

//
// GET pszUrl. The body goes to hFile when given, otherwise into a
// NUL terminated buffer (*ppBody, at most cbMax bytes). Returns the HTTP
// status code, or 0 on a network error (GetLastError()).
//
static DWORD
HttpGet(HINTERNET hSession, LPCWSTR pszUrl, LPCWSTR pszHeaders,
        HANDLE hFile, char** ppBody, DWORD cbMax, LONGLONG* pcbTotal)
{
   URL_COMPONENTS uc;
   WCHAR szHost[256];
   WCHAR szPath[2048];
   HINTERNET hConnect = NULL, hRequest = NULL;
   DWORD dwStatus = 0, cbStatus = sizeof(dwStatus);
   DWORD dwErr = ERROR_SUCCESS;
   char* pBody = NULL;
   DWORD cbBody = 0;
   LONGLONG cbTotal = 0;

   if (ppBody)
      *ppBody = NULL;

   ZeroMemory(&uc, sizeof(uc));
   uc.dwStructSize = sizeof(uc);
   uc.lpszHostName = szHost;
   uc.dwHostNameLength = COUNTOF(szHost);
   uc.lpszUrlPath = szPath;
   uc.dwUrlPathLength = COUNTOF(szPath);
   uc.dwExtraInfoLength = (DWORD)-1;

   if (!WinHttpCrackUrl(pszUrl, 0, 0, &uc))
      return 0;

   if (uc.nScheme != INTERNET_SCHEME_HTTPS) {
      SetLastError(ERROR_ACCESS_DENIED);      // never download over plain HTTP
      return 0;
   }

   if (uc.dwExtraInfoLength && uc.lpszExtraInfo)
      StringCchCatN(szPath, COUNTOF(szPath), uc.lpszExtraInfo, uc.dwExtraInfoLength);

   hConnect = WinHttpConnect(hSession, szHost, uc.nPort, 0);
   if (!hConnect)
      goto Fail;

   hRequest = WinHttpOpenRequest(hConnect, L"GET", szPath, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
   if (!hRequest)
      goto Fail;

   if (!WinHttpSendRequest(hRequest,
                           pszHeaders ? pszHeaders : WINHTTP_NO_ADDITIONAL_HEADERS,
                           pszHeaders ? (DWORD)-1L : 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
       !WinHttpReceiveResponse(hRequest, NULL))
      goto Fail;

   if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &dwStatus, &cbStatus, WINHTTP_NO_HEADER_INDEX))
      goto Fail;

   if (dwStatus == 200) {

      for (;;) {
         BYTE buf[16384];
         DWORD cbRead = 0;

         if (!WinHttpReadData(hRequest, buf, sizeof(buf), &cbRead)) {
            dwStatus = 0;
            goto Fail;
         }
         if (!cbRead)
            break;

         cbTotal += cbRead;

         if (hFile) {
            DWORD cbWritten;
            if (!WriteFile(hFile, buf, cbRead, &cbWritten, NULL) || cbWritten != cbRead) {
               dwStatus = 0;
               goto Fail;
            }
         } else if (ppBody) {
            char* pNew;
            if (cbBody + cbRead > cbMax) {
               SetLastError(ERROR_INVALID_DATA);
               dwStatus = 0;
               goto Fail;
            }
            pNew = (char*)LocalAlloc(LMEM_FIXED, cbBody + cbRead + 1);
            if (!pNew) {
               dwStatus = 0;
               goto Fail;
            }
            if (pBody) {
               CopyMemory(pNew, pBody, cbBody);
               LocalFree(pBody);
            }
            CopyMemory(pNew + cbBody, buf, cbRead);
            pBody = pNew;
            cbBody += cbRead;
            pBody[cbBody] = 0;
         }
      }
   }

   WinHttpCloseHandle(hRequest);
   WinHttpCloseHandle(hConnect);

   if (ppBody)
      *ppBody = pBody;
   else if (pBody)
      LocalFree(pBody);

   if (pcbTotal)
      *pcbTotal = cbTotal;

   return dwStatus;

Fail:
   dwErr = GetLastError();
   if (pBody)
      LocalFree(pBody);
   if (hRequest)
      WinHttpCloseHandle(hRequest);
   if (hConnect)
      WinHttpCloseHandle(hConnect);
   SetLastError(dwErr ? dwErr : ERROR_WINHTTP_CONNECTION_ERROR);
   return 0;
}


/////////////////////////////////////////////////////////////////////
//
// Check thread
//
/////////////////////////////////////////////////////////////////////

static VOID
PickAsset(const char* pRelease, PUPDATEINFO pInfo)
{
   const char* pAssets = JsonFind(pRelease, "assets");
   const char* p;
   const char* pFallback = NULL;
   const char* pChosen = NULL;

   if (!pAssets || *pAssets != '[')
      return;

   p = JsonWs(pAssets + 1);

   while (*p == '{') {
      WCHAR szName[MAXPATHLEN];
      const char* pNext;

      if (JsonString(JsonFind(p, "name"), szName, COUNTOF(szName))) {
         WCHAR szArch[64];
         SIZE_T cch = lstrlen(szName);

         MultiByteToWideChar(CP_ACP, 0, szAssetArch, -1, szArch, COUNTOF(szArch));

         if (!lstrcmpi(szName, szArch)) {
            pChosen = p;
            break;
         }
         if (!pFallback && (!lstrcmpi(szName, L"winfile.exe") ||
                            (cch > 4 && !lstrcmpi(szName + cch - 4, L".exe") && !StrStrIW(szName, L"winfile-"))))
            pFallback = p;
      }

      pNext = JsonSkipValue(p);
      if (!pNext)
         break;
      p = JsonWs(pNext);
      if (*p != ',')
         break;
      p = JsonWs(p + 1);
   }

   if (!pChosen)
      pChosen = pFallback;

   if (!pChosen)
      return;

   JsonString(JsonFind(pChosen, "browser_download_url"), pInfo->szAssetUrl, COUNTOF(pInfo->szAssetUrl));
   pInfo->cbAsset = JsonInt(JsonFind(pChosen, "size"));

   {
      WCHAR szDigest[128];
      if (JsonString(JsonFind(pChosen, "digest"), szDigest, COUNTOF(szDigest)))
         pInfo->bHasDigest = ParseSha256Digest(szDigest, pInfo->abSha256);
   }
}

//
// GET pszUrl without following redirects. Returns the HTTP status (0 on a
// network error) and the Location header, if any.
//
static DWORD
HttpGetLocation(HINTERNET hSession, LPCWSTR pszUrl, LPWSTR pszLocation, DWORD cchLocation)
{
   URL_COMPONENTS uc;
   WCHAR szHost[256];
   WCHAR szPath[2048];
   HINTERNET hConnect = NULL, hRequest = NULL;
   DWORD dwStatus = 0, cbStatus = sizeof(dwStatus);
   DWORD dwOption = WINHTTP_DISABLE_REDIRECTS;
   DWORD cbLocation = cchLocation * sizeof(WCHAR);

   pszLocation[0] = CHAR_NULL;

   ZeroMemory(&uc, sizeof(uc));
   uc.dwStructSize = sizeof(uc);
   uc.lpszHostName = szHost;
   uc.dwHostNameLength = COUNTOF(szHost);
   uc.lpszUrlPath = szPath;
   uc.dwUrlPathLength = COUNTOF(szPath);

   if (!WinHttpCrackUrl(pszUrl, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS)
      return 0;

   hConnect = WinHttpConnect(hSession, szHost, uc.nPort, 0);
   if (hConnect)
      hRequest = WinHttpOpenRequest(hConnect, L"GET", szPath, NULL, WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

   if (hRequest &&
       WinHttpSetOption(hRequest, WINHTTP_OPTION_DISABLE_FEATURE, &dwOption, sizeof(dwOption)) &&
       WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
       WinHttpReceiveResponse(hRequest, NULL) &&
       WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &dwStatus, &cbStatus, WINHTTP_NO_HEADER_INDEX)) {

      if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                               pszLocation, &cbLocation, WINHTTP_NO_HEADER_INDEX))
         pszLocation[0] = CHAR_NULL;
   }

   if (hRequest)
      WinHttpCloseHandle(hRequest);
   if (hConnect)
      WinHttpCloseHandle(hConnect);

   return dwStatus;
}

//
// Fallback when the GitHub API refuses us (it allows 60 anonymous requests
// an hour per IP address, which a shared/carrier-grade NAT address can use
// up). The release web pages have no such limit:
//   https://github.com/<repo>/releases/latest -> 302 .../releases/tag/<tag>
//   https://github.com/<repo>/releases/download/<tag>/<asset>(.sha256)
// Returns FALSE if the site could not be reached either.
//
static BOOL
WebCheck(HINTERNET hSession, PUPDATEINFO pInfo)
{
   WCHAR szUrl[512];
   WCHAR szLocation[1024];
   WCHAR szAsset[64];
   LPWSTR pTag;
   DWORD dwStatus;
   static const WCHAR szTagPart[] = L"/releases/tag/";

   StringCchPrintf(szUrl, COUNTOF(szUrl), L"https://github.com/%s/releases/latest", pInfo->szRepo);
   dwStatus = HttpGetLocation(hSession, szUrl, szLocation, COUNTOF(szLocation));

   if (dwStatus == 404) {
      pInfo->iStatus = UPD_NORELEASE;     // repository not found / private
      return TRUE;
   }

   if (dwStatus < 300 || dwStatus >= 400 || !szLocation[0])
      return FALSE;

   pTag = StrStrIW(szLocation, szTagPart);
   if (!pTag) {
      pInfo->iStatus = UPD_NORELEASE;     // redirected to .../releases: none published
      return TRUE;
   }
   pTag += COUNTOF(szTagPart) - 1;

   StringCchCopy(pInfo->szTag, COUNTOF(pInfo->szTag), pTag);
   StringCchCopy(pInfo->szReleaseUrl, COUNTOF(pInfo->szReleaseUrl), szLocation);

   if (!ParseVersion(pInfo->szTag, pInfo->awLatest)) {
      pInfo->iStatus = UPD_BADDATA;
      return TRUE;
   }

   if (CompareVersion(pInfo->awLatest, pInfo->awCurrent) <= 0) {
      pInfo->iStatus = UPD_UPTODATE;
      return TRUE;
   }

   MultiByteToWideChar(CP_ACP, 0, szAssetArch, -1, szAsset, COUNTOF(szAsset));
   StringCchPrintf(pInfo->szAssetUrl, COUNTOF(pInfo->szAssetUrl),
                   L"https://github.com/%s/releases/download/%s/%s",
                   pInfo->szRepo, pInfo->szTag, szAsset);
   pInfo->cbAsset = 0;                    // not known without the API

   //
   // The release workflow publishes <asset>.sha256 next to each program.
   // A release without one can't be verified, so don't offer it.
   //
   {
      char* pBody = NULL;
      WCHAR szHashUrl[2100];
      WCHAR szHash[80];
      INT i = 0;

      StringCchPrintf(szHashUrl, COUNTOF(szHashUrl), L"%s.sha256", pInfo->szAssetUrl);
      dwStatus = HttpGet(hSession, szHashUrl, NULL, NULL, &pBody, 4096, NULL);

      if (dwStatus == 200 && pBody) {
         const char* p = pBody;
         while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
         lstrcpyW(szHash, L"sha256:");
         for (i = 0; i < 64 && p[i]; i++)
            szHash[7 + i] = (WCHAR)p[i];
         szHash[7 + i] = CHAR_NULL;
         pInfo->bHasDigest = ParseSha256Digest(szHash, pInfo->abSha256);
      }

      if (pBody)
         LocalFree(pBody);

      if (dwStatus == 404) {
         pInfo->iStatus = UPD_NOASSET;    // no program for this computer
         return TRUE;
      }
      if (!pInfo->bHasDigest)
         return FALSE;
   }

   pInfo->iStatus = UPD_NEWER;
   return TRUE;
}

static DWORD WINAPI
UpdateCheckThread(LPVOID lpParam)
{
   PUPDATEINFO pInfo = (PUPDATEINFO)lpParam;
   HINTERNET hSession;
   WCHAR szUrl[512];
   char* pBody = NULL;
   DWORD dwStatus;

   if (!pInfo->bInteractive)
      Sleep(5000);         // let File Manager finish starting first

   GetOwnVersion(pInfo->awCurrent);

   if (!pInfo->szRepo[0]) {
      pInfo->iStatus = UPD_NOTCONFIGURED;
      goto Done;
   }

   hSession = HttpOpenSession();
   if (!hSession) {
      pInfo->iStatus = UPD_NETERROR;
      pInfo->dwError = GetLastError();
      goto Done;
   }

   StringCchPrintf(szUrl, COUNTOF(szUrl), L"https://api.github.com/repos/%s/releases/latest", pInfo->szRepo);

   dwStatus = HttpGet(hSession, szUrl,
                      L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28",
                      NULL, &pBody, 4 * 1024 * 1024, NULL);
   pInfo->dwError = GetLastError();

   if (dwStatus == 404) {
      pInfo->iStatus = UPD_NORELEASE;
   } else if (dwStatus != 200 || !pBody) {
      //
      // Rate limited (403/429), GitHub API trouble or blocked: try the
      // release web pages instead.
      //
      if (!WebCheck(hSession, pInfo)) {
         pInfo->iStatus = UPD_NETERROR;
         if (dwStatus)
            pInfo->dwError = dwStatus;     // HTTP status
      }
   } else {
      const char* pRelease = JsonWs(pBody);

      if (!JsonString(JsonFind(pRelease, "tag_name"), pInfo->szTag, COUNTOF(pInfo->szTag)) ||
          !ParseVersion(pInfo->szTag, pInfo->awLatest)) {

         pInfo->iStatus = UPD_BADDATA;

      } else {

         JsonString(JsonFind(pRelease, "html_url"), pInfo->szReleaseUrl, COUNTOF(pInfo->szReleaseUrl));

         if (CompareVersion(pInfo->awLatest, pInfo->awCurrent) <= 0) {
            pInfo->iStatus = UPD_UPTODATE;
         } else {
            PickAsset(pRelease, pInfo);
            pInfo->iStatus = pInfo->szAssetUrl[0] ? UPD_NEWER : UPD_NOASSET;
         }
      }
   }

   WinHttpCloseHandle(hSession);

Done:
   if (pBody)
      LocalFree(pBody);

   if (!PostMessage(hwndFrame, FS_UPDATECHECKED, 0, (LPARAM)pInfo)) {
      LocalFree(pInfo);
      InterlockedExchange(&fUpdateBusy, FALSE);
   }
   return 0;
}


/////////////////////////////////////////////////////////////////////
//
// Download thread
//
/////////////////////////////////////////////////////////////////////

static BOOL
FileSha256(LPCWSTR pszFile, BYTE abHash[32])
{
   BCRYPT_ALG_HANDLE hAlg = NULL;
   BCRYPT_HASH_HANDLE hHash = NULL;
   HANDLE hFile;
   BOOL bRet = FALSE;
   BYTE buf[65536];
   DWORD cbRead;

   hFile = CreateFile(pszFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
   if (hFile == INVALID_HANDLE_VALUE)
      return FALSE;

   if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0)
      goto Exit;
   if (BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0) < 0)
      goto Exit;

   while (ReadFile(hFile, buf, sizeof(buf), &cbRead, NULL) && cbRead) {
      if (BCryptHashData(hHash, buf, cbRead, 0) < 0)
         goto Exit;
   }

   bRet = BCryptFinishHash(hHash, abHash, 32, 0) >= 0;

Exit:
   if (hHash)
      BCryptDestroyHash(hHash);
   if (hAlg)
      BCryptCloseAlgorithmProvider(hAlg, 0);
   CloseHandle(hFile);
   return bRet;
}

static BOOL
LooksLikeExe(LPCWSTR pszFile)
{
   HANDLE hFile = CreateFile(pszFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
   IMAGE_DOS_HEADER dos;
   DWORD cbRead = 0;
   BOOL bRet = FALSE;

   if (hFile == INVALID_HANDLE_VALUE)
      return FALSE;

   if (ReadFile(hFile, &dos, sizeof(dos), &cbRead, NULL) && cbRead == sizeof(dos) &&
       dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0) {

      DWORD dwSig = 0;
      if (SetFilePointer(hFile, dos.e_lfanew, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
          ReadFile(hFile, &dwSig, sizeof(dwSig), &cbRead, NULL) && cbRead == sizeof(dwSig))
         bRet = (dwSig == IMAGE_NT_SIGNATURE);
   }

   CloseHandle(hFile);
   return bRet;
}

static DWORD WINAPI
UpdateDownloadThread(LPVOID lpParam)
{
   PUPDATEINFO pInfo = (PUPDATEINFO)lpParam;
   HINTERNET hSession = NULL;
   HANDLE hFile;
   DWORD dwStatus;
   LONGLONG cbGot = 0;

   if (!GetModuleFileName(NULL, pInfo->szNewFile, COUNTOF(pInfo->szNewFile)) ||
       FAILED(StringCchCat(pInfo->szNewFile, COUNTOF(pInfo->szNewFile), L".new"))) {
      pInfo->iStatus = UPD_NOTWRITABLE;
      goto Done;
   }

   hFile = CreateFile(pInfo->szNewFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   if (hFile == INVALID_HANDLE_VALUE) {
      pInfo->dwError = GetLastError();
      pInfo->iStatus = UPD_NOTWRITABLE;
      goto Done;
   }

   hSession = HttpOpenSession();
   dwStatus = hSession ? HttpGet(hSession, pInfo->szAssetUrl, NULL, hFile, NULL, 0, &cbGot) : 0;
   pInfo->dwError = GetLastError();
   if (dwStatus && dwStatus != 200)
      pInfo->dwError = dwStatus;

   CloseHandle(hFile);
   if (hSession)
      WinHttpCloseHandle(hSession);

   if (dwStatus != 200) {
      pInfo->iStatus = UPD_NETERROR;
   } else if ((pInfo->cbAsset > 0 && cbGot != pInfo->cbAsset) || !LooksLikeExe(pInfo->szNewFile)) {
      pInfo->iStatus = UPD_VERIFYFAILED;
   } else if (pInfo->bHasDigest) {
      BYTE abHash[32];
      pInfo->iStatus = (FileSha256(pInfo->szNewFile, abHash) && !memcmp(abHash, pInfo->abSha256, 32))
                          ? UPD_DOWNLOADED : UPD_VERIFYFAILED;
   } else {
      pInfo->iStatus = UPD_DOWNLOADED;
   }

   if (pInfo->iStatus != UPD_DOWNLOADED)
      DeleteFile(pInfo->szNewFile);

Done:
   if (!PostMessage(hwndFrame, FS_UPDATEDOWNLOADED, 0, (LPARAM)pInfo)) {
      LocalFree(pInfo);
      InterlockedExchange(&fUpdateBusy, FALSE);
   }
   return 0;
}


/////////////////////////////////////////////////////////////////////
//
// UI thread
//
/////////////////////////////////////////////////////////////////////

static VOID
ShowError(PUPDATEINFO pInfo, LPCWSTR pszWhat)
{
   WCHAR szMsg[1024];
   WCHAR szErr[256] = L"";

   if (pInfo->dwError >= 100 && pInfo->dwError < 600)
      StringCchPrintf(szErr, COUNTOF(szErr), L"The server answered with HTTP status %u.", pInfo->dwError);
   else if (pInfo->dwError)
      FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
                    FORMAT_MESSAGE_FROM_HMODULE,
                    GetModuleHandle(L"winhttp.dll"), pInfo->dwError, 0, szErr, COUNTOF(szErr), NULL);

   StringCchPrintf(szMsg, COUNTOF(szMsg), L"%s\n\n%s", pszWhat, szErr);
   MessageBox(hwndFrame, szMsg, szUpdTitle, MB_OK | MB_ICONEXCLAMATION);
}

static VOID
OfferReleasePage(PUPDATEINFO pInfo, LPCWSTR pszWhy)
{
   WCHAR szMsg[1024];

   if (!pInfo->szReleaseUrl[0]) {
      MessageBox(hwndFrame, pszWhy, szUpdTitle, MB_OK | MB_ICONEXCLAMATION);
      return;
   }

   StringCchPrintf(szMsg, COUNTOF(szMsg), L"%s\n\nOpen the release page to download it manually?", pszWhy);
   if (MessageBox(hwndFrame, szMsg, szUpdTitle, MB_YESNO | MB_ICONEXCLAMATION) == IDYES)
      ShellExecute(hwndFrame, L"open", pInfo->szReleaseUrl, NULL, NULL, SW_SHOWNORMAL);
}

VOID
UpdateOnChecked(LPARAM lParam)
{
   PUPDATEINFO pInfo = (PUPDATEINFO)lParam;
   WCHAR szMsg[1024];
   WCHAR szCur[32], szNew[32];

   FormatVersion(pInfo->awCurrent, szCur, COUNTOF(szCur));
   FormatVersion(pInfo->awLatest, szNew, COUNTOF(szNew));

   switch (pInfo->iStatus) {

   case UPD_NEWER:
   {
      WCHAR szSkip[64];
      HANDLE hThread;

      GetPrivateProfileString(szSettings, szSkipVersionKey, L"", szSkip, COUNTOF(szSkip), szTheINIFile);

      if (!pInfo->bInteractive && !lstrcmpi(szSkip, pInfo->szTag))
         break;

      StringCchPrintf(szMsg, COUNTOF(szMsg),
                      L"A new version of File Manager is available.\n\n"
                      L"Installed version:\t%s\nNew version:\t%s\n\n"
                      L"Download and install it now?",
                      szCur, szNew);

      if (MessageBox(hwndFrame, szMsg, szUpdTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) {
         if (!pInfo->bInteractive)
            WritePrivateProfileString(szSettings, szSkipVersionKey, pInfo->szTag, szTheINIFile);
         break;
      }

      hThread = CreateThread(NULL, 0, UpdateDownloadThread, pInfo, 0, NULL);
      if (hThread) {
         CloseHandle(hThread);
         return;          // the download thread owns pInfo now; still busy
      }

      pInfo->dwError = GetLastError();
      ShowError(pInfo, L"Could not start the download.");
      break;
   }

   case UPD_UPTODATE:
      if (pInfo->bInteractive) {
         StringCchPrintf(szMsg, COUNTOF(szMsg),
                         L"File Manager is up to date.\n\nInstalled version:\t%s\nLatest release:\t%s",
                         szCur, szNew);
         MessageBox(hwndFrame, szMsg, szUpdTitle, MB_OK | MB_ICONINFORMATION);
      }
      break;

   case UPD_NOTCONFIGURED:
      if (pInfo->bInteractive)
         MessageBox(hwndFrame,
                    L"No update source is set for this build.\n\n"
                    L"Builds made by the release workflow check their own GitHub repository automatically. "
                    L"For other builds, add\n\n    UpdateRepo=owner/repository\n\n"
                    L"under [Settings] in winfile.ini.",
                    szUpdTitle, MB_OK | MB_ICONINFORMATION);
      break;

   case UPD_NORELEASE:
      if (pInfo->bInteractive) {
         StringCchPrintf(szMsg, COUNTOF(szMsg),
                         L"No published release was found for %s.\n\nInstalled version:\t%s",
                         pInfo->szRepo, szCur);
         MessageBox(hwndFrame, szMsg, szUpdTitle, MB_OK | MB_ICONINFORMATION);
      }
      break;

   case UPD_NOASSET:
      if (pInfo->bInteractive) {
         StringCchPrintf(szMsg, COUNTOF(szMsg),
                         L"Version %s is available, but the release has no program file for this computer.",
                         szNew);
         OfferReleasePage(pInfo, szMsg);
      }
      break;

   case UPD_BADDATA:
      if (pInfo->bInteractive)
         MessageBox(hwndFrame, L"The update information could not be read.", szUpdTitle, MB_OK | MB_ICONEXCLAMATION);
      break;

   default:
      if (pInfo->bInteractive)
         ShowError(pInfo, L"Could not check for updates.");
      break;
   }

   LocalFree(pInfo);
   InterlockedExchange(&fUpdateBusy, FALSE);
}

static BOOL
InstallAndRestart(PUPDATEINFO pInfo)
{
   WCHAR szExe[MAXPATHLEN];
   WCHAR szOld[MAXPATHLEN];
   WCHAR szCmd[MAXPATHLEN * 2];
   STARTUPINFO si;
   PROCESS_INFORMATION pi;

   if (!GetModuleFileName(NULL, szExe, COUNTOF(szExe)))
      return FALSE;

   StringCchPrintf(szOld, COUNTOF(szOld), L"%s.old", szExe);
   DeleteFile(szOld);

   if (!MoveFileEx(szExe, szOld, MOVEFILE_REPLACE_EXISTING))
      return FALSE;

   if (!MoveFileEx(pInfo->szNewFile, szExe, MOVEFILE_REPLACE_EXISTING)) {
      DWORD dwErr = GetLastError();
      MoveFileEx(szOld, szExe, MOVEFILE_REPLACE_EXISTING);   // put the old one back
      SetLastError(dwErr);
      return FALSE;
   }

   StringCchPrintf(szCmd, COUNTOF(szCmd), L"\"%s\" %s%u", szExe, szUpdatedSwitch, GetCurrentProcessId());

   ZeroMemory(&si, sizeof(si));
   si.cb = sizeof(si);

   if (!CreateProcess(szExe, szCmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
      // The update is in place; it will be used on the next start.
      return TRUE;
   }

   CloseHandle(pi.hThread);
   CloseHandle(pi.hProcess);

   PostMessage(hwndFrame, WM_CLOSE, 0, 0);
   return TRUE;
}

VOID
UpdateOnDownloaded(LPARAM lParam)
{
   PUPDATEINFO pInfo = (PUPDATEINFO)lParam;
   WCHAR szMsg[512];
   WCHAR szNew[32];

   FormatVersion(pInfo->awLatest, szNew, COUNTOF(szNew));

   switch (pInfo->iStatus) {

   case UPD_DOWNLOADED:
      StringCchPrintf(szMsg, COUNTOF(szMsg),
                      L"Version %s has been downloaded.\n\nRestart File Manager now to finish installing it?",
                      szNew);

      if (MessageBox(hwndFrame, szMsg, szUpdTitle, MB_YESNO | MB_ICONQUESTION) == IDYES) {
         if (!InstallAndRestart(pInfo)) {
            pInfo->dwError = GetLastError();
            DeleteFile(pInfo->szNewFile);
            ShowError(pInfo, L"The update could not be installed.");
         }
      } else {
         DeleteFile(pInfo->szNewFile);
      }
      break;

   case UPD_NOTWRITABLE:
      OfferReleasePage(pInfo,
                       L"File Manager can't write to the folder it is installed in "
                       L"(try running it as administrator once).");
      break;

   case UPD_VERIFYFAILED:
      OfferReleasePage(pInfo, L"The downloaded update failed verification and was discarded.");
      break;

   default:
      ShowError(pInfo, L"The update could not be downloaded.");
      break;
   }

   LocalFree(pInfo);
   InterlockedExchange(&fUpdateBusy, FALSE);
}

VOID
UpdateCheck(BOOL bInteractive)
{
   PUPDATEINFO pInfo;
   HANDLE hThread;

   if (InterlockedExchange(&fUpdateBusy, TRUE)) {
      if (bInteractive)
         MessageBox(hwndFrame, L"An update check is already in progress.", szUpdTitle, MB_OK | MB_ICONINFORMATION);
      return;
   }

   pInfo = (PUPDATEINFO)LocalAlloc(LPTR, sizeof(UPDATEINFO));
   if (!pInfo) {
      InterlockedExchange(&fUpdateBusy, FALSE);
      return;
   }

   pInfo->bInteractive = bInteractive;
   GetRepo(pInfo->szRepo, COUNTOF(pInfo->szRepo));

   if (bInteractive)
      WritePrivateProfileString(szSettings, szSkipVersionKey, NULL, szTheINIFile);

   {
      WCHAR szDay[16];
      StringCchPrintf(szDay, COUNTOF(szDay), L"%u", TodayNumber());
      WritePrivateProfileString(szSettings, szLastCheckKey, szDay, szTheINIFile);
   }

   hThread = CreateThread(NULL, 0, UpdateCheckThread, pInfo, 0, NULL);
   if (hThread) {
      CloseHandle(hThread);
   } else {
      LocalFree(pInfo);
      InterlockedExchange(&fUpdateBusy, FALSE);
   }
}

VOID
AutoUpdateInit(VOID)
{
   WCHAR szExe[MAXPATHLEN];
   WCHAR szOld[MAXPATHLEN];

   if (GetModuleFileName(NULL, szExe, COUNTOF(szExe))) {
      StringCchPrintf(szOld, COUNTOF(szOld), L"%s.old", szExe);
      DeleteFile(szOld);
      StringCchPrintf(szOld, COUNTOF(szOld), L"%s.new", szExe);
      DeleteFile(szOld);
   }

   if (bAutoUpdate &&
       (DWORD)GetPrivateProfileInt(szSettings, szLastCheckKey, 0, szTheINIFile) != TodayNumber()) {

      WCHAR szRepo[256];
      GetRepo(szRepo, COUNTOF(szRepo));

      if (szRepo[0])
         UpdateCheck(FALSE);
   }
}

VOID
UpdateHandleCommandLine(LPWSTR* ppszCmdLine)
{
   LPWSTR p = *ppszCmdLine;
   SIZE_T cchSwitch = lstrlen(szUpdatedSwitch);

   while (*p == L' ' || *p == L'\t')
      p++;

   if (StrCmpNIW(p, szUpdatedSwitch, (int)cchSwitch))
      return;

   {
      DWORD dwPid = StrToInt(p + cchSwitch);
      HANDLE hProcess;

      p += cchSwitch;
      while (*p && *p != L' ' && *p != L'\t')
         p++;
      while (*p == L' ' || *p == L'\t')
         p++;
      *ppszCmdLine = p;

      //
      // Wait for the previous version to finish closing (it saves settings
      // on exit) before starting up.
      //
      hProcess = OpenProcess(SYNCHRONIZE, FALSE, dwPid);
      if (hProcess) {
         WaitForSingleObject(hProcess, 15000);
         CloseHandle(hProcess);
      }
   }
}
