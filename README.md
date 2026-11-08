# Folder Size Explorer

A small standalone Windows C application that lists files and folders like Explorer while calculating folder sizes in the background.

## Features

- Pick a folder or type/paste a path.
- Navigate into folders and back up to a parent folder.
- See logical sizes for both files and folders in one sortable list.
- Refresh on demand without locking the interface during recursive scans.
- Avoids following junctions and symbolic-link directories, preventing cycles and accidental scans outside the selected tree.
- Uses the current Windows user's normal file permissions and requires no installation or administrator access.

## Build

Run from PowerShell:

```powershell
.\build.ps1
```

The standalone executable is written to `build\FolderSizeExplorer.exe`. The build uses the installed MSYS2 UCRT64 toolchain and only links Windows system libraries.

## Use

1. Launch `FolderSizeExplorer.exe`.
2. Click **Browse...** to grant access by selecting a directory, or enter a path in the address box and press Enter.
3. Double-click a folder to enter it. Click **Up** to return to its parent.

Folder sizes represent the sum of logical file lengths below each folder. Inaccessible files are skipped and marked in the Type column when a scan is incomplete.
