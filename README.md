# DefragForge

**Source-only classic-style NTFS defragmentation workbench with a Win32 UI.**

This experimental application combines an Explorer-like file/folder fragmentation browser,
a detailed allocation map, ranked candidate files, and selectable HDD/SSD maintenance actions.
Windows' optimizer handles full-volume HDD defragmentation, free-space consolidation, and
SSD ReTrim; a separate **bounded, experimental** `FSCTL_MOVE_FILE` path is available for
small, eligible files on non-system NTFS HDDs. No custom disk driver or raw-sector writes.

> **Status: pre-production.** GitHub CI compiles and tests the source, but real disks have
> not undergone independent power-failure, concurrent-writer, storage-pool, BitLocker,
> and crash-recovery testing. For now, use **Analyze** on important disks; test manual
> file relocation only on expendable non-system HDD volumes with verified backups.

## Classic defragmenter features

- **Physical allocation map:** 2,048 color-coded samples of approximate allocation density.
  This is **occupied/free space**, not a direct visualization of fragment boundaries;
  hover over a cell for its approximate occupancy percentage.
- **Two sortable, full-length lists:** fragmented files (path, size, extent count,
  excess extents, priority) and folder hotspots (fragmented file count, excess
  extents, size). Virtual Win32 list controls avoid the former 1,500-row display cap.
- **Search and drilldown:** live case-insensitive path filter, sortable columns,
  right-click Copy path/Open folder, double-click a hotspot to display its files.
- **Native optimization strategies:**
  - `Analyze` — read-only NTFS metadata and fragmentation report.
  - `Quick targeted` — bounded experimental `FSCTL_MOVE_FILE`, maximum 16 attempts
    and 64 MiB per file, optionally limited to files selected in the list.
  - `Full HDD defragmentation` — delegates to the Windows `defrag.exe /D` operation.
  - `Consolidate HDD free space` — delegates to the Windows `defrag.exe /X` operation.
  - `SSD ReTrim` — delegates to the Windows `defrag.exe /L` operation.
- **Operator controls:** operation preview, confirmation/backup acknowledgment,
  cooperative pause/resume for scans and quick targeted moves, cancellation,
  scrollable activity log, percent progress where available, UTF-8 CSV export,
  and File/Actions/View/Help menus.
- **Reporting:** overall counts for scanned files, fragmented files, bytes, excess
  extents, and inaccessible files; plus per-folder rollups.
- **Source-based builds:** Visual Studio 2026 (`v145`) or VS 2022 (`v143`), Win32/x64,
  Debug/Release; GitHub CI builds all four configurations and runs planner tests.

### Optimization safety policy

| Mode | HDD | SSD | Unknown media | Windows version |
| --- | --- | --- | --- | --- |
| Analyze | Yes | Yes | Yes (NTFS only) | XP+ intended (legacy unverified) |
| Quick targeted | Non-system HDD only | No | No | NTFS API supported |
| Full defragmentation `/D` | Yes | No | No | Windows 8+ |
| Consolidate free space `/X` | Yes | No | No | Windows 8+ |
| SSD ReTrim `/L` | No | Confirmed TRIM support only | No | Windows 8+ |

All write actions require positive media detection and confirmation. The Windows optimizer
runs under your existing privileges, usually requiring an elevated process. The full-volume
modes are delegated to Windows, **not** unique DefragForge defragmentation algorithms.

## Building on the target Windows machine

1. Install **Desktop development with C++**, MSBuild, and a compatible Windows SDK.
2. Open **Developer Command Prompt** (recommended) in the repository directory.
3. Execute `build.cmd` (or `build.cmd Debug`). The toolset is selected from Visual Studio's version on modern Windows; set `DEFRAGFORGE_TOOLSET` to override.
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
| Windows 10 / 11 | v145 (VS 2026) / v143 (VS 2022) | `0x0A00` | Current CI uses v143 |

**Editions** (Home/Pro/Enterprise) share the same Windows API level. Native CPU, compiler
and SDK support determine builds, not the marketing edition.

## Safety model and limitations

- Analyze uses only read operations, though inspecting the full allocation map may need
  administrator privileges; normal-user analysis can be incomplete.
- HDD relocation is disabled unless a successful analysis exists, **HDD is positively
  identified**, the drive is not the Windows system drive, and a user confirms the backup.
  A second modal confirmation is required before calling `FSCTL_MOVE_FILE`.
- SSD ReTrim requires positive SSD and TRIM capability probes, NTFS, Windows 8+,
  explicit acknowledgment, and a separate confirmation. It does not require a fragment scan.
  Windows' native optimizer runs with the current user's privileges (administrative rights
  are generally required). Cancelling stops the helper; already queued storage maintenance
  may continue after the helper exits.
- Volume handle creation for relocation requires administrator privileges; do **not**
  indiscriminately elevate the entire GUI.
- Moves use the NTFS driver; the program never writes raw sectors. The allocation bitmap
  is a snapshot and can become stale; failed moves are tracked, not forced.
- Unknown/virtual/RAID storage characteristics are **not** fully detectable; user confirmation
  isn't proof. Never run a relocation pass on flash media, storage pools, critical volumes,
  BitLocker volumes that have not been separately validated, or valuable production systems.
- **Manual** relocation of the currently running Windows system volume is disabled.
  Windows-managed `/D` and `/X` operations may handle system HDD volumes. Use a disposable,
  non-system NTFS HDD volume for early manual-relocation validation.
- The experimental manual engine **does not guarantee a lower fragmentation score**. A move can fail after
  an earlier chunk succeeds; immediately rerun analysis to measure the result.
- The basic score is a deterministic heuristic, not trained machine learning. There are
  no claims of universal performance gains over Windows' built-in optimizer.
- Built-in scheduling, service-based background optimization, offline MFT optimization,
  raw-drive balancing, VSS integration, transactional rollback, filesystem repair,
  integrity hashes, and I/O-aware throttling are **not implemented**.
- Pausing is cooperative only for read-only scans and Quick targeted moves, never
  for the OS optimizer child process. Cancellation may leave earlier file moves completed.
- Extensive tests on expendable NTFS test volumes (including abrupt process termination,
  concurrent writes, sparse files, BitLocker, volume full, and power failure simulation)
  are required before production use.

## Source map

```
include/BuildTarget.h      Windows compile-time feature defines
include/Analytics.h        File and folder aggregation contracts
src/Analytics.cpp          Excess-extent and folder hotspot analytics
include/Planner.h          Planner scoring/eligibility contracts
include/Engine.h           Native engine interface and data types
src/Planner.cpp            Scoring and conservative eligibility rules
src/Engine.cpp             NTFS analysis, HDD moves, capability checks and Windows optimizer dispatch
include/SsdPolicy.h        Testable no-write/SSD eligibility rules
src/SsdPolicy.cpp          Conservative cross-version optimization policy
src/Main.cpp               Menus, file/folder browser, threaded Win32 GUI, GDI map
resources/                 Common Controls v6 / application manifest
tests/                     Standalone deterministic planner tests
```

### Microsoft Windows references

- [Defragmenting Files](https://learn.microsoft.com/en-us/windows/win32/fileio/defragmenting-files)
- [FSCTL_GET_VOLUME_BITMAP](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_volume_bitmap)
- [FSCTL_GET_RETRIEVAL_POINTERS](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_retrieval_pointers)
- [FSCTL_MOVE_FILE](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_move_file)
- [Windows defrag /L ReTrim](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/defrag)
- [DEVICE_SEEK_PENALTY_DESCRIPTOR](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-device_seek_penalty_descriptor)
- [DEVICE_TRIM_DESCRIPTOR](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-device_trim_descriptor)
- [Targeting Windows XP with MSVC](https://learn.microsoft.com/en-us/cpp/build/configuring-programs-for-windows-xp)

MIT-licensed. All releases are source-only.
