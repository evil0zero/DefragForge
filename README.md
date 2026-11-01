# DefragForge

**Source-only, native Win32 NTFS analysis and experimental HDD relocation.**

DefragForge is a Win32 C++ workbench for examining fragmentation, ranking candidate files,
inspecting an NTFS allocation-map sample, exporting a CSV report, and optionally performing
small, filesystem-managed cluster relocation passes on mechanical hard drives. It does not
ship executables, drivers, or an installer.

> **Status: experimental prototype.** The Windows GUI and NTFS relocation engine have not been
> compiled or exercised against real Windows disks in this release. **Do not run Optimize on
> any drive holding irreplaceable data.** The default Analyze operation is read-only.

## Features

- Visual Studio `.sln`, native `.vcxproj` (Win32/x64, Debug/Release), resource manifest,
  build script, unit tests and GitHub Actions workflow (CI builds aren't distributed).
- Classic Win32 Unicode interface: drive selector, 512-cell allocation view, ranked file
  table, threaded scans, cancellation, live activity log, preview and UTF-8 CSV export.
- Reads file extents with `FSCTL_GET_RETRIEVAL_POINTERS`; accesses the NTFS volume allocation
  map with `FSCTL_GET_VOLUME_BITMAP` when running elevated.
- Limited experimental HDD moves via documented `FSCTL_MOVE_FILE`. Conservative selection:
  only contiguous-move-eligible regular files, skipping system/reparse/compressed/sparse/
  encrypted files; max 64 MiB per file and 16 file attempts per pass.
- SSDs that can be positively identified are blocked from relocation. **Unknown media may
  still be an SSD**; the user must explicitly verify the physical drive type.

## Building on the target Windows machine

1. Install Visual Studio C++/MSBuild and the Windows SDK matching the host OS.
2. Open **Developer Command Prompt** (recommended) in the repository directory.
3. Execute `build.cmd` (or `build.cmd Debug`).
4. Start `out\<platform>\Release\DefragForge.exe`.
5. For unit tests, run `out\<platform>\Release\PlannerTests.exe`.

The batch script identifies the host's Windows version using `ver`, chooses the native
architecture and a *default* matching toolset. Toolset detection is a best-effort mapping:
**it cannot install missing compilers or SDKs**. Override using:

```bat
set DEFRAGFORGE_TOOLSET=v141_xp
set DEFRAGFORGE_TARGET=0x0501
set DEFRAGFORGE_PLATFORM=Win32
build.cmd
```

Versions and toolsets are not interchangeable. `WINVER` / `_WIN32_WINNT` change the
**compile-time API surface**, not the actual machine. Running `build.cmd` on an XP machine
requires XP-compatible build tools *installed and working on that machine* (typically
VS2010/v100 and Windows 7 SDK compatible headers); newer Visual Studio releases do not
run on XP. For cross-targeting XP from a newer host, use the deprecated `v141_xp` toolset.

| Native build host | Suggested toolset | API macro | Notes |
| --- | --- | --- | --- |
| Windows XP SP3 / Server 2003 SP2 | v100 | `0x0501` | Untested; original vendor SDK needed |
| Windows Vista | v100 | `0x0600` | Untested; legacy toolchains only |
| Windows 7 | v141 or available older toolset | `0x0601` | Untested |
| Windows 8 / 8.1 | v141 or supported host toolset | `0x0602` / `0x0603` | Untested |
| Windows 10 / 11 | v143 | `0x0A00` | Untested with Win32 GUI on Windows |

**Editions** (Home/Pro/Enterprise) share the same Windows API level. Native CPU, compiler
and SDK support determine builds, not the marketing edition.

## Safety model and limitations

- Analyze uses only read operations, though inspecting the full allocation map may need
  administrator privileges; normal-user analysis can be incomplete.
- Optimize is disabled unless a successful analysis exists, the device isn't identified
  as an SSD, and a user ticks the backup/mechanical-HDD acknowledgment. A second modal
  confirmation is required before calling `FSCTL_MOVE_FILE`.
- Volume handle creation for relocation requires administrator privileges; do **not**
  indiscriminately elevate the entire GUI.
- Moves use the NTFS driver; the program never writes raw sectors. The allocation bitmap
  is a snapshot and can become stale; failed moves are tracked, not forced.
- Unknown/virtual/RAID storage characteristics are **not** fully detectable; user confirmation
  isn't proof. Never run a relocation pass on flash media, storage pools, critical volumes,
  BitLocker volumes that have not been separately validated, or valuable production systems.
- Relocation of the currently running Windows system volume is disabled. Use a disposable,
  non-system NTFS HDD volume for early validation.
- The first engine **does not guarantee a lower fragmentation score**. A move can fail after
  an earlier chunk succeeds; immediately rerun analysis to measure the result.
- The basic score is a deterministic heuristic, not trained machine learning. There are
  no claims of universal performance gains over Windows' built-in optimizer.
- VSS, transactional rollback, filesystem repair, data-integrity hashes, compression-stream
  relocation, offline MFT optimization, and granular throttling are **not implemented**.
- Extensive tests on expendable NTFS test volumes (including abrupt process termination,
  concurrent writes, sparse files, BitLocker, volume full, and power failure simulation)
  are required before production use.

## Source map

```
include/BuildTarget.h      Windows compile-time feature defines
include/Planner.h          Planner scoring/eligibility contracts
include/Engine.h           Native engine interface and data types
src/Planner.cpp            Scoring and conservative eligibility rules
src/Engine.cpp             Windows NTFS reader, bitmap, CSV and experimental moves
src/Main.cpp               Threaded native Win32 GUI and GDI visualization
resources/                 Common Controls v6 / application manifest
tests/                     Standalone deterministic planner tests
```

### Microsoft Windows references

- [Defragmenting Files](https://learn.microsoft.com/en-us/windows/win32/fileio/defragmenting-files)
- [FSCTL_GET_VOLUME_BITMAP](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_volume_bitmap)
- [FSCTL_GET_RETRIEVAL_POINTERS](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_retrieval_pointers)
- [FSCTL_MOVE_FILE](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_move_file)
- [Targeting Windows XP with MSVC](https://learn.microsoft.com/en-us/cpp/build/configuring-programs-for-windows-xp)

MIT-licensed. All releases are source-only.
