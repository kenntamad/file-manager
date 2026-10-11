/********************************************************************

   wfupdate_repo.h

   GitHub repository ("owner/name") whose Releases are checked for
   updates. The release workflow (.github/workflows/release.yml)
   overwrites this file with the repository it builds from, so
   published builds update from the right place automatically.

   For local builds, either set it here or add
       UpdateRepo=owner/name
   under [Settings] in winfile.ini.

********************************************************************/

#define WF_UPDATE_REPO L"kenntamad/file-manager"
