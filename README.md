# ![icon](winfile.png) File Manager

A personal fork of [Microsoft's Windows File Manager (WinFile)](https://github.com/microsoft/winfile)
with folder sizes, resizable columns and automatic updates.

## Folder sizes

Folders show the **total size of everything inside them** in the Size column
(View > All File Details or Partial Details) instead of `<DIR>`.

- Sizes are calculated in the background, so browsing stays responsive; a folder
  shows `<DIR>` until its total is ready.
- The status bar totals and *Sort by Size* include folder totals.
- File > Properties on a folder shows its total size.
- Junctions and symbolic links are not followed, so nothing is counted twice.
- The folder tree on the left shows each folder's total in grey next to its name
  (drive roots are skipped; the status bar shows drive usage).
- **Window > Refresh** (F5) recalculates all sizes.
- Turn it on or off with **View > Show Folder Sizes** (on by default; saved as
  `FolderSizes=` in winfile.ini).

## Resizable columns

In the details views the file list has a column header like File Explorer's:

- Drag a divider to resize a column; double-click it to go back to the automatic width.
- Click Name, Size or Date to sort by that column.
- Long names are cut off with "..." instead of running into the next column.
- Widths are remembered (`ColumnWidths=` in winfile.ini).
- New windows open in **View > All File Details**.

## Automatic updates

File Manager checks this repository's GitHub Releases once a day at startup. When a
newer version is published it asks before downloading, verifies the download
(size, executable header and GitHub's SHA-256 digest), swaps it in and restarts.

- **Options > Check for Updates Automatically** turns the daily check on or off.
- **Help > Check for Updates...** checks right away.
- Declining an automatic prompt skips that version until you check manually.

### Publishing an update

Push a version tag that is higher than the current one:

```
git tag v10.5.0
git push origin v10.5.0
```

The `Release` workflow (.github/workflows/release.yml) builds x64, x86 and ARM64,
stamps the version from the tag, points the build at this repository for
updates, and publishes `winfile-x64.exe`, `winfile-x86.exe` and
`winfile-arm64.exe` to a new release. Installed copies pick it up on their next
daily check.

Builds made outside the workflow don't know where to look for updates. Set the
repository in `src/wfupdate_repo.h`, or add `UpdateRepo=owner/repository` under
`[Settings]` in winfile.ini.

## Building

- Visual Studio: open `Winfile.sln` and build.
- MinGW / llvm-mingw: `make` (see `GNUmakefile`).

---

## Original README

# ![icon](winfile.png) Windows File Manager (WinFile)

The Windows File Manager lives again and runs as a native x86, x64 and arm64 desktop app
on all currently supported version of Windows, including Windows 11. I welcome your thoughts, comments and suggestions.

There are two primary versions of the source code in the master branch:

1. original_plus tag: refers to the source for WinFile as of Windows NT4 with minimal changes
so that it compiles with Visual Studio and runs on current Windows.

2. current master: actively developed branch that consists of enhancements and additions to WinFile.

I will consider bugs fixes and suggestions for minor changes to the master branch. Feel free to create a pull request or post issues as you see fit.

I will not be changing the original_plus sources nor creating other branches for other purposes.
You are welcome to do that on your own.

## Download The App

If you are running Windows 10, you can download the app from the store

<a href="https://www.microsoft.com/store/apps/9p7vbbbc49rb?ocid=badge"><img src="https://developer.microsoft.com/store/badges/images/English_get-it-from-MS.png" alt="Get it from Microsoft" width="150px"></a>

If you just want to download the WinFile application without worrying about compiling from the source code, we have a number of pre-compiled versions available.

Please select the version which you would like to download.

- [Chocolatey release](https://chocolatey.org/packages/winfile)

- Winget release: `winget install "Windows File Manager"`

- [MSYS2 release](https://packages.msys2.org/base/mingw-w64-winfile)

- [Latest Release on Github (v10.3.0.0)](https://github.com/microsoft/winfile/releases/latest)

To see more release binaries, including of older versions, [see the releases page](https://github.com/Microsoft/winfile/releases).

## History

The Windows File manager was originally released with Windows 3.0 in the early 1990s.  You
can read more about the history at https://en.wikipedia.org/wiki/Windows_File_Manager .

## Change history

Highlights of changes in each release are in [CHANGES.md](CHANGES.md).

## What it looks like
# ![Winfile](winfilescreenshot.png)

## Contributing

### Contributor License Agreement
As mentioned above, this project welcomes contributions and suggestions.  Most contributions require you to agree to a
Contributor License Agreement (CLA) declaring that you have the right to, and actually do, grant us
the rights to use your contribution. For details, visit https://cla.microsoft.com.

When you submit a pull request, a CLA-bot will automatically determine whether you need to provide
a CLA and decorate the PR appropriately (e.g., label, comment). Simply follow the instructions
provided by the bot. You will only need to do this once across all repos using our CLA.

This project has adopted the [Microsoft Open Source Code of Conduct](https://opensource.microsoft.com/codeofconduct/).
For more information see the [Code of Conduct FAQ](https://opensource.microsoft.com/codeofconduct/faq/) or
contact [opencode@microsoft.com](mailto:opencode@microsoft.com) with any additional questions or comments.

### What Makes a Good Pull Request for WinFile?
If you are interested in contributing and/or suggesting changes to the actual application, you might find it helpful to [read this post first](https://github.com/Microsoft/winfile/issues/88).

## License

Copyright (c) Microsoft Corporation. All rights reserved.

Licensed under the [MIT](LICENSE) License.
