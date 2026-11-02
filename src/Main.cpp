#include "BuildTarget.h"
#include "Engine.h"
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>
#include <process.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

enum ControlId {
    IdDrive = 100, IdRefresh, IdAnalyze, IdStrategy, IdPreview, IdOptimize,
    IdPause, IdCancel, IdExport, IdConfirm, IdSearch, IdSelectedOnly,
    IdTabs, IdFiles, IdFolders, IdLog, IdStatus, IdMap, IdHeadline,
    IdStatistics, IdMapTitle, IdLegend, IdClearFilter,
    IdAbout = 200, IdExit, IdCopyPath, IdOpenFolder, IdFolderToFilter,
    IdTabFiles, IdTabFolders
};

enum WorkMode { ModeAnalyze, ModeQuick, ModeHddDefrag, ModeHddConsolidate, ModeRetrim };
const UINT MsgLog = WM_APP + 1;
const UINT MsgProgress = WM_APP + 2;
const UINT MsgFinished = WM_APP + 3;

struct WorkRequest {
    HWND window;
    std::wstring root;
    WorkMode mode;
    nd::ScanResult result;
    std::vector<std::wstring> selectedPaths;
    volatile LONG cancel;
    volatile LONG paused;
    WorkRequest() : window(NULL), mode(ModeAnalyze), cancel(0), paused(0) {}
};

struct Application {
    HWND window, drives, refresh, analyze, strategy, preview, optimize, pause, cancel;
    HWND exportButton, confirm, filter, clearFilter, selectedOnly, tabs;
    HWND files, folders, log, status, progress, map, headline, statistics;
    HWND mapTitle, legend;
    HFONT font;
    HANDLE thread;
    WorkRequest* active;
    nd::ScanResult result;
    std::vector<nd::VolumeInfo> volumes;
    std::vector<size_t> fileRows;
    std::vector<size_t> folderRows;
    bool hasScan;
    bool closing;
    int fileSortColumn, folderSortColumn;
    bool fileSortDescending, folderSortDescending;
    Application() : window(NULL), drives(NULL), refresh(NULL), analyze(NULL),
        strategy(NULL), preview(NULL), optimize(NULL), pause(NULL), cancel(NULL),
        exportButton(NULL), confirm(NULL), filter(NULL), clearFilter(NULL), selectedOnly(NULL),
        tabs(NULL), files(NULL), folders(NULL), log(NULL), status(NULL), progress(NULL),
        map(NULL), headline(NULL), statistics(NULL), mapTitle(NULL), legend(NULL),
        font(NULL), thread(NULL), active(NULL), hasScan(false), closing(false),
        fileSortColumn(4), folderSortColumn(2), fileSortDescending(true), folderSortDescending(true) {}
};

Application app;

std::wstring Number(unsigned long long value) {
    wchar_t buffer[64] = {};
    swprintf_s(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%I64u", value);
    return buffer;
}

std::wstring FloatNumber(double value) {
    wchar_t buffer[64] = {};
    swprintf_s(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%.2f", value);
    return buffer;
}

std::wstring Lower(const std::wstring& value) {
    std::wstring result(value);
    for (size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<wchar_t>(towlower(result[i]));
    return result;
}

std::wstring WindowText(HWND control) {
    if (!control) return L"";
    const int length = GetWindowTextLengthW(control);
    std::vector<wchar_t> data(static_cast<size_t>(length) + 2, 0);
    GetWindowTextW(control, &data[0], length + 1);
    return &data[0];
}

void ApplyFont(HWND control) {
    if (control && app.font)
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
}

HWND Add(const wchar_t* kind, const wchar_t* title, DWORD style, int id, DWORD extra = 0) {
    HWND control = CreateWindowExW(extra, kind, title, WS_CHILD | WS_VISIBLE | style,
        0, 0, 10, 10, app.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(NULL), NULL);
    ApplyFont(control);
    return control;
}

void LogLine(const std::wstring& text) {
    if (!app.log) return;
    const int length = GetWindowTextLengthW(app.log);
    SendMessageW(app.log, EM_SETSEL, length, length);
    const std::wstring line = text + L"\r\n";
    SendMessageW(app.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(app.log, EM_SCROLLCARET, 0, 0);
    if (GetWindowTextLengthW(app.log) > 50000) {
        SendMessageW(app.log, EM_SETSEL, 0, 12000);
        SendMessageW(app.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
    }
}

nd::VolumeInfo* SelectedVolume() {
    if (!app.drives) return NULL;
    const int index = static_cast<int>(SendMessageW(app.drives, CB_GETCURSEL, 0, 0));
    return index >= 0 && static_cast<size_t>(index) < app.volumes.size() ?
        &app.volumes[static_cast<size_t>(index)] : NULL;
}

WorkMode SelectedMode() {
    const int index = static_cast<int>(SendMessageW(app.strategy, CB_GETCURSEL, 0, 0));
    if (index < 0) return ModeAnalyze;
    return static_cast<WorkMode>(SendMessageW(app.strategy, CB_GETITEMDATA, index, 0));
}

bool IsSystemVolume(const nd::VolumeInfo* volume) {
    wchar_t directory[MAX_PATH] = {};
    if (!volume || volume->root.empty() || !GetWindowsDirectoryW(directory, MAX_PATH)) return true;
    return towupper(directory[0]) == towupper(volume->root[0]);
}

bool Checked(HWND checkbox) {
    return checkbox && SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void RefreshButtons() {
    const bool busy = app.active != NULL;
    nd::VolumeInfo* volume = SelectedVolume();
    const bool ntfs = volume && volume->ntfs;
    const WorkMode mode = SelectedMode();
    const bool ack = Checked(app.confirm);
    bool eligible = ntfs;
    if (mode == ModeQuick) eligible = ntfs && volume->media == nd::MediaHdd &&
        !IsSystemVolume(volume) && app.hasScan;
    if (mode == ModeHddDefrag || mode == ModeHddConsolidate)
        eligible = ntfs && volume->nativeHddSupported;
    if (mode == ModeRetrim) eligible = ntfs && volume->retrimSupported;
    if (mode == ModeAnalyze) eligible = false;
    EnableWindow(app.drives, !busy);
    EnableWindow(app.refresh, !busy);
    EnableWindow(app.analyze, !busy && ntfs);
    EnableWindow(app.strategy, !busy && ntfs);
    EnableWindow(app.preview, !busy && ntfs);
    EnableWindow(app.optimize, !busy && eligible && ack);
    EnableWindow(app.confirm, !busy && ntfs);
    EnableWindow(app.selectedOnly, !busy && mode == ModeQuick && app.hasScan);
    EnableWindow(app.exportButton, !busy && app.hasScan);
    EnableWindow(app.cancel, busy);
    const bool pauseable = busy && (app.active->mode == ModeAnalyze || app.active->mode == ModeQuick);
    EnableWindow(app.pause, pauseable);
    SetWindowTextW(app.pause, pauseable && InterlockedCompareExchange(&app.active->paused, 0, 0) != 0 ?
        L"Resume" : L"Pause");
}

void AddStrategy(const wchar_t* title, WorkMode mode) {
    const int index = static_cast<int>(SendMessageW(app.strategy, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(title)));
    SendMessageW(app.strategy, CB_SETITEMDATA, index, mode);
}

void PopulateStrategies() {
    SendMessageW(app.strategy, CB_RESETCONTENT, 0, 0);
    nd::VolumeInfo* info = SelectedVolume();
    if (info && info->ntfs && info->media == nd::MediaHdd) {
        AddStrategy(L"Quick: targeted NTFS files", ModeQuick);
        if (info->nativeHddSupported) {
            AddStrategy(L"Full HDD defragmentation", ModeHddDefrag);
            AddStrategy(L"Consolidate HDD free space", ModeHddConsolidate);
        }
    } else if (info && info->retrimSupported) {
        AddStrategy(L"SSD maintenance: ReTrim", ModeRetrim);
    } else {
        AddStrategy(L"Read-only analysis only", ModeAnalyze);
    }
    SendMessageW(app.strategy, CB_SETCURSEL, 0, 0);
}

void SetHeadline() {
    nd::VolumeInfo* info = SelectedVolume();
    if (!info) {
        SetWindowTextW(app.headline, L"No local volumes detected");
        return;
    }
    const wchar_t* kind = info->media == nd::MediaHdd ? L"Mechanical HDD" :
        (info->media == nd::MediaSsd ? L"SSD" : L"Unidentified media");
    std::wstring line = info->root + L" " + info->filesystem + L"   |   " + kind +
        L"   |   Total " + nd::FormatSize(info->bytesTotal) +
        L"   |   Free " + nd::FormatSize(info->bytesFree);
    if (info->media == nd::MediaSsd) {
        line += info->trimKnown ? (info->trimEnabled ? L"   |   TRIM enabled" :
            L"   |   TRIM disabled") : L"   |   TRIM unknown";
    }
    SetWindowTextW(app.headline, line.c_str());
    PopulateStrategies();
    SetWindowTextW(app.confirm, info->media == nd::MediaSsd ?
        L"I understand ReTrim is SSD maintenance, not file defragmentation." :
        L"I have a verified backup and understand that optimization changes disk allocation.");
    RefreshButtons();
}

void PopulateVolumes() {
    const nd::VolumeInfo* old = SelectedVolume();
    const std::wstring previous = old ? old->root : L"";
    app.volumes = nd::GetLocalVolumes();
    SendMessageW(app.drives, CB_RESETCONTENT, 0, 0);
    int select = 0;
    for (size_t i = 0; i < app.volumes.size(); ++i) {
        const nd::VolumeInfo& volume = app.volumes[i];
        std::wstring label = volume.root + L"  " +
            (volume.label.empty() ? L"Local disk" : volume.label);
        SendMessageW(app.drives, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (_wcsicmp(volume.root.c_str(), previous.c_str()) == 0) select = static_cast<int>(i);
    }
    if (!app.volumes.empty()) SendMessageW(app.drives, CB_SETCURSEL, select, 0);
    SetHeadline();
}

struct SortFile {
    bool operator()(size_t a, size_t b) const {
        const nd::FileRecord& left = app.result.files[a];
        const nd::FileRecord& right = app.result.files[b];
        int comparison = 0;
        switch (app.fileSortColumn) {
        case 1: comparison = left.bytes < right.bytes ? -1 : left.bytes > right.bytes ? 1 : 0; break;
        case 2: comparison = left.extents < right.extents ? -1 : left.extents > right.extents ? 1 : 0; break;
        case 3: comparison = left.extents < right.extents ? -1 : left.extents > right.extents ? 1 : 0; break;
        case 4: comparison = left.score < right.score ? -1 : left.score > right.score ? 1 : 0; break;
        default: comparison = _wcsicmp(left.path.c_str(), right.path.c_str()); break;
        }
        if (!comparison) comparison = _wcsicmp(left.path.c_str(), right.path.c_str());
        return app.fileSortDescending ? comparison > 0 : comparison < 0;
    }
};

struct SortFolder {
    bool operator()(size_t a, size_t b) const {
        const nd::FolderStat& left = app.result.folders[a];
        const nd::FolderStat& right = app.result.folders[b];
        int comparison = 0;
        switch (app.folderSortColumn) {
        case 1: comparison = left.fragmentedFiles < right.fragmentedFiles ? -1 :
            left.fragmentedFiles > right.fragmentedFiles ? 1 : 0; break;
        case 2: comparison = left.excessExtents < right.excessExtents ? -1 :
            left.excessExtents > right.excessExtents ? 1 : 0; break;
        case 3: comparison = left.bytes < right.bytes ? -1 : left.bytes > right.bytes ? 1 : 0; break;
        default: comparison = _wcsicmp(left.path.c_str(), right.path.c_str()); break;
        }
        if (!comparison) comparison = _wcsicmp(left.path.c_str(), right.path.c_str());
        return app.folderSortDescending ? comparison > 0 : comparison < 0;
    }
};

void RebuildVisibleRows() {
    app.fileRows.clear();
    app.folderRows.clear();
    const std::wstring filter = Lower(WindowText(app.filter));
    if (app.hasScan) {
        for (size_t i = 0; i < app.result.files.size(); ++i) {
            if (filter.empty() || Lower(app.result.files[i].path).find(filter) != std::wstring::npos)
                app.fileRows.push_back(i);
        }
        for (size_t i = 0; i < app.result.folders.size(); ++i) {
            if (filter.empty() || Lower(app.result.folders[i].path).find(filter) != std::wstring::npos)
                app.folderRows.push_back(i);
        }
        std::sort(app.fileRows.begin(), app.fileRows.end(), SortFile());
        std::sort(app.folderRows.begin(), app.folderRows.end(), SortFolder());
    }
    ListView_SetItemCountEx(app.files, static_cast<int>(app.fileRows.size()), LVSICF_NOINVALIDATEALL);
    ListView_SetItemCountEx(app.folders, static_cast<int>(app.folderRows.size()), LVSICF_NOINVALIDATEALL);
    InvalidateRect(app.files, NULL, TRUE);
    InvalidateRect(app.folders, NULL, TRUE);
}

void SetStatistics() {
    if (!app.hasScan) {
        SetWindowTextW(app.statistics, L"Read-only analysis provides file, folder and cluster-map statistics.");
        return;
    }
    const std::wstring summary = L"Scanned " + Number(app.result.scanned) +
        L" files   |   Fragmented " + Number(app.result.fragmented) +
        L"   |   Excess extents " + Number(app.result.excessExtents) +
        L"   |   Fragmented data " + nd::FormatSize(app.result.fragmentedBytes) +
        L"   |   Inaccessible " + Number(app.result.inaccessible);
    SetWindowTextW(app.statistics, summary.c_str());
    SendMessageW(app.status, SB_SETTEXTW, 0,
        reinterpret_cast<LPARAM>(app.result.volumeBitmapAvailable ?
            L"Analysis complete: allocation map available" :
            L"Analysis complete: cluster map unavailable without sufficient privileges"));
}

void ShowCurrentTab() {
    const bool fileTab = TabCtrl_GetCurSel(app.tabs) == 0;
    ShowWindow(app.files, fileTab ? SW_SHOW : SW_HIDE);
    ShowWindow(app.folders, fileTab ? SW_HIDE : SW_SHOW);
}

void PopulateResults() {
    SetStatistics();
    RebuildVisibleRows();
    ShowCurrentTab();
    InvalidateRect(app.map, NULL, TRUE);
    LogLine(L"Analysis: " + Number(app.result.fragmented) + L" fragmented files, " +
        Number(app.result.excessExtents) + L" excess extents in " +
        Number(app.result.folders.size()) + L" folders.");
}

class WindowObserver : public nd::Observer {
public:
    explicit WindowObserver(WorkRequest* work) : work_(work) {}
    virtual bool Cancelled() const {
        return InterlockedCompareExchange(&work_->cancel, 0, 0) != 0;
    }
    virtual bool WaitWhilePaused() const {
        while (InterlockedCompareExchange(&work_->paused, 0, 0) != 0) {
            if (Cancelled()) return false;
            Sleep(70);
        }
        return !Cancelled();
    }
    virtual void Message(const std::wstring& message) {
        PostMessageW(work_->window, MsgLog, 0, reinterpret_cast<LPARAM>(new std::wstring(message)));
    }
    virtual void Progress(unsigned int percent) {
        PostMessageW(work_->window, MsgProgress, percent, 0);
    }
private:
    WorkRequest* work_;
};

unsigned __stdcall Worker(void* data) {
    WorkRequest* request = static_cast<WorkRequest*>(data);
    WindowObserver observer(request);
    switch (request->mode) {
    case ModeAnalyze: nd::Analyze(request->root, observer, request->result); break;
    case ModeQuick: nd::Optimize(request->root, observer, request->result, request->selectedPaths); break;
    case ModeHddDefrag: nd::OptimizeWindowsHdd(request->root, nd::NativeDefragment, observer, request->result); break;
    case ModeHddConsolidate: nd::OptimizeWindowsHdd(request->root, nd::NativeConsolidate, observer, request->result); break;
    case ModeRetrim: nd::Retrim(request->root, observer, request->result); break;
    }
    PostMessageW(request->window, MsgFinished, reinterpret_cast<WPARAM>(request), 0);
    return 0;
}

std::vector<std::wstring> SelectedFilePaths() {
    std::vector<std::wstring> paths;
    int selection = -1;
    while ((selection = ListView_GetNextItem(app.files, selection, LVNI_SELECTED)) >= 0) {
        if (static_cast<size_t>(selection) < app.fileRows.size())
            paths.push_back(app.result.files[app.fileRows[selection]].path);
    }
    return paths;
}

bool StartWorker(WorkMode mode) {
    if (app.active) return false;
    nd::VolumeInfo* info = SelectedVolume();
    if (!info || !info->ntfs) return false;
    WorkRequest* request = new WorkRequest();
    request->window = app.window;
    request->root = info->root;
    request->mode = mode;
    if (mode == ModeQuick) {
        request->result = app.result;
        if (Checked(app.selectedOnly)) {
            request->selectedPaths = SelectedFilePaths();
            if (request->selectedPaths.empty()) {
                delete request;
                MessageBoxW(app.window, L"Select one or more files in the Files tab, or uncheck Selected only.",
                    L"Nothing selected", MB_OK | MB_ICONINFORMATION);
                return false;
            }
        }
    }
    unsigned threadId = 0;
    uintptr_t thread = _beginthreadex(NULL, 0, Worker, request, 0, &threadId);
    if (!thread) {
        delete request;
        MessageBoxW(app.window, L"Could not start the disk worker.", L"DefragForge", MB_ICONERROR);
        return false;
    }
    app.active = request;
    app.thread = reinterpret_cast<HANDLE>(thread);
    SendMessageW(app.progress, PBM_SETPOS, 0, 0);
    LogLine(mode == ModeAnalyze ? L"Reading NTFS metadata (read-only)..." :
        mode == ModeQuick ? L"Starting bounded experimental HDD relocation..." :
        mode == ModeRetrim ? L"Starting SSD ReTrim through Windows..." :
        mode == ModeHddConsolidate ? L"Starting Windows HDD free-space consolidation..." :
        L"Starting Windows HDD defragmentation...");
    RefreshButtons();
    return true;
}

void PreviewPlan() {
    nd::VolumeInfo* volume = SelectedVolume();
    if (!volume) return;
    const WorkMode mode = SelectedMode();
    std::wstring message;
    if (mode == ModeRetrim) {
        message = L"Windows SSD ReTrim (/L) tells supported hardware which logical blocks are unused.\n\n"
            L"No file clusters are manually moved. Windows often performs ReTrim automatically.";
    } else if (mode == ModeHddDefrag) {
        message = L"Full HDD defragmentation (/D) is delegated to the Windows optimizer.\n\n"
            L"Unlike Quick targeted, this can process a system volume. Windows decides which files can move.";
    } else if (mode == ModeHddConsolidate) {
        message = L"Free-space consolidation (/X) is delegated to the Windows optimizer.\n\n"
            L"This aims to make free space more contiguous, reducing future fragmentation.";
    } else if (mode == ModeQuick) {
        const nd::PlanLimits limits = nd::DefaultLimits();
        unsigned long long bytes = 0;
        unsigned int eligible = 0;
        const std::vector<std::wstring> chosen = Checked(app.selectedOnly) ?
            SelectedFilePaths() : std::vector<std::wstring>();
        for (size_t i = 0; i < app.result.files.size(); ++i) {
            const nd::FileRecord& file = app.result.files[i];
            if (Checked(app.selectedOnly) &&
                std::find(chosen.begin(), chosen.end(), file.path) == chosen.end()) continue;
            nd::Candidate candidate = {};
            candidate.fileBytes = file.bytes;
            candidate.clusterCount = file.clusters;
            candidate.extents = file.extents;
            candidate.sparseOrCompressed = (file.attributes &
                (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_SPARSE_FILE)) != 0;
            candidate.systemFile = (file.attributes & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT)) != 0;
            if (nd::IsEligible(candidate, limits)) { ++eligible; bytes += file.bytes; }
        }
        message = L"Quick targeted (experimental)\n\nPreliminary candidate count: " + Number(eligible) +
            L"\nCandidate file data: " + nd::FormatSize(bytes) +
            L"\nMaximum attempts: " + Number(limits.maximumMoves) +
            L"\nMaximum file size: " + nd::FormatSize(limits.maximumFileBytes) +
            L"\n\nOnly positively identified, non-system NTFS HDDs are eligible. "
            L"Additional safety rules are applied immediately before each move.";
    } else message = L"Read-only analysis is the only supported operation for this media.";
    MessageBoxW(app.window, message.c_str(), L"DefragForge - Operation preview",
        MB_OK | MB_ICONINFORMATION);
}

void StartOptimization() {
    if (!Checked(app.confirm) || app.active) return;
    nd::VolumeInfo* volume = SelectedVolume();
    if (!volume || !volume->ntfs) return;
    const WorkMode mode = SelectedMode();
    std::wstring message;
    if (mode == ModeQuick) {
        if (volume->media != nd::MediaHdd || IsSystemVolume(volume) || !app.hasScan) return;
        message = L"EXPERIMENTAL FILE RELOCATION\n\nThis uses FSCTL_MOVE_FILE to relocate eligible NTFS HDD files. "
            L"Do not use on production data. The selected drive must have a verified backup.\n\n"
            L"A power failure or storage defect could cause data loss. Continue?";
    } else if (mode == ModeHddDefrag || mode == ModeHddConsolidate) {
        if (!volume->nativeHddSupported) return;
        message = mode == ModeHddDefrag ?
            L"FULL HDD DEFRAGMENTATION\n\nRun Windows' built-in defrag.exe /D on this NTFS HDD?\n\n"
            L"This changes disk allocations and may take significant time. Keep a verified backup." :
            L"CONSOLIDATE FREE SPACE\n\nRun Windows' built-in defrag.exe /X on this NTFS HDD?\n\n"
            L"This changes disk allocations and may take significant time. Keep a verified backup.";
    } else if (mode == ModeRetrim) {
        if (!volume->retrimSupported) return;
        message = L"SSD RETRIM\n\nAsk Windows to inform the SSD of unused logical blocks?\n\n"
            L"This is not file defragmentation. Windows normally schedules maintenance automatically.";
    } else return;
    if (MessageBoxW(app.window, message.c_str(), L"Confirm disk operation",
        MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) == IDYES) StartWorker(mode);
}

void ExportCsv() {
    if (!app.hasScan) return;
    wchar_t file[MAX_PATH] = L"DefragForge-report.csv";
    const wchar_t filter[] = L"CSV report (*.csv)\0*.csv\0All files (*.*)\0*.*\0\0";
    OPENFILENAMEW chooser = {};
    chooser.lStructSize = sizeof(chooser);
    chooser.hwndOwner = app.window;
    chooser.lpstrFilter = filter;
    chooser.lpstrFile = file;
    chooser.nMaxFile = MAX_PATH;
    chooser.lpstrDefExt = L"csv";
    chooser.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (GetSaveFileNameW(&chooser)) {
        if (nd::WriteCsvReport(file, app.result)) LogLine(L"Exported CSV: " + std::wstring(file));
        else MessageBoxW(app.window, L"Unable to write CSV.", L"DefragForge", MB_ICONERROR);
    }
}

void CopyText(const std::wstring& text) {
    if (!OpenClipboard(app.window)) return;
    EmptyClipboard();
    const size_t size = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (memory) {
        void* data = GlobalLock(memory);
        if (data) {
            memcpy(data, text.c_str(), size);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        } else GlobalFree(memory);
    }
    CloseClipboard();
}

std::wstring SelectedPathFromList(HWND list) {
    const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (selected < 0) return L"";
    if (list == app.files && static_cast<size_t>(selected) < app.fileRows.size())
        return app.result.files[app.fileRows[selected]].path;
    if (list == app.folders && static_cast<size_t>(selected) < app.folderRows.size())
        return app.result.folders[app.folderRows[selected]].path;
    return L"";
}

void ChangeFolderFilter() {
    const std::wstring folder = SelectedPathFromList(app.folders);
    if (folder.empty()) return;
    SetWindowTextW(app.filter, folder.c_str());
    TabCtrl_SetCurSel(app.tabs, 0);
    ShowCurrentTab();
}

void ContextMenu(HWND list, LPARAM position) {
    const std::wstring path = SelectedPathFromList(list);
    if (path.empty()) return;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IdCopyPath, L"Copy path");
    AppendMenuW(menu, MF_STRING, IdOpenFolder, L"Open containing folder");
    if (list == app.folders) AppendMenuW(menu, MF_STRING, IdFolderToFilter, L"Show files in this folder");
    POINT cursor = {};
    if (position == -1) GetCursorPos(&cursor);
    else { cursor.x = GET_X_LPARAM(position); cursor.y = GET_Y_LPARAM(position); }
    const int action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON,
        cursor.x, cursor.y, 0, app.window, NULL);
    DestroyMenu(menu);
    if (action == IdCopyPath) CopyText(path);
    else if (action == IdOpenFolder) {
        const std::wstring folder = list == app.files ? nd::ParentFolder(path) : path;
        ShellExecuteW(app.window, L"open", folder.c_str(), NULL, NULL, SW_SHOWNORMAL);
    } else if (action == IdFolderToFilter) ChangeFolderFilter();
}

void Layout(int width, int height) {
    if (!app.drives) return;
    const int margin = 12;
    const int top = 10;
    const int usable = width - 2 * margin;
    int driveWidth = width >= 1220 ? 210 : 160;
    int strategyWidth = width >= 1220 ? 270 : 210;
    int x = margin;
    struct Item { HWND control; int width; } topControls[] = {
        {app.drives, driveWidth}, {app.refresh, 70}, {app.analyze, 80},
        {app.strategy, strategyWidth}, {app.preview, 80}, {app.optimize, 86},
        {app.pause, 70}, {app.cancel, 70}, {app.exportButton, 80}
    };
    const int totalFixed = driveWidth + strategyWidth + 70 + 80 + 80 + 86 + 70 + 70 + 80;
    const int gap = (usable - totalFixed) / 8 >= 3 ? (usable - totalFixed) / 8 : 3;
    for (size_t i = 0; i < sizeof(topControls) / sizeof(topControls[0]); ++i) {
        MoveWindow(topControls[i].control, x, top, topControls[i].width, 27, TRUE);
        x += topControls[i].width + gap;
    }
    MoveWindow(app.headline, margin, 47, usable, 22, TRUE);
    MoveWindow(app.confirm, margin, 73, usable, 24, TRUE);
    MoveWindow(app.mapTitle, margin, 105, usable, 18, TRUE);
    MoveWindow(app.map, margin, 127, usable, 126, TRUE);
    MoveWindow(app.legend, margin, 257, usable, 19, TRUE);
    MoveWindow(app.statistics, margin, 280, usable, 22, TRUE);
    MoveWindow(app.filter, margin + 118, 308, usable - 250, 25, TRUE);
    MoveWindow(app.clearFilter, width - margin - 123, 308, 123, 25, TRUE);
    MoveWindow(app.tabs, margin, 343, usable, 29, TRUE);
    const int logHeight = height < 690 ? 88 : 120;
    const int logTop = height - 30 - logHeight - 7;
    const int listTop = 371;
    int listHeight = logTop - listTop - 7;
    if (listHeight < 64) listHeight = 64;
    MoveWindow(app.files, margin, listTop, usable, listHeight, TRUE);
    MoveWindow(app.folders, margin, listTop, usable, listHeight, TRUE);
    MoveWindow(app.log, margin, logTop, usable, logHeight, TRUE);
    MoveWindow(app.status, 0, height - 26, width, 26, TRUE);
    MoveWindow(app.progress, width - 205, height - 23, 192, 16, TRUE);
    MoveWindow(app.selectedOnly, margin, 308, 108, 25, TRUE);
}

LRESULT CALLBACK MapProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(window, &paint);
        RECT rect = {};
        GetClientRect(window, &rect);
        FillRect(dc, &rect, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        const int columns = 128, rows = 16;
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        HBRUSH brush[5] = {
            CreateSolidBrush(RGB(208, 218, 227)),
            CreateSolidBrush(RGB(104, 180, 218)),
            CreateSolidBrush(RGB(63, 132, 201)),
            CreateSolidBrush(RGB(240, 171, 78)),
            CreateSolidBrush(RGB(225, 93, 84))
        };
        for (int index = 0; index < columns * rows; ++index) {
            unsigned int amount = 0;
            if (app.result.allocationSamples.size() == 2048)
                amount = app.result.allocationSamples[static_cast<size_t>(index)];
            int level = amount > 220 ? 4 : amount > 165 ? 3 : amount > 90 ? 2 : amount > 30 ? 1 : 0;
            RECT cell = { index % columns * width / columns + 1,
                index / columns * height / rows + 1,
                (index % columns + 1) * width / columns - 1,
                (index / columns + 1) * height / rows - 1 };
            if (cell.right > cell.left && cell.bottom > cell.top)
                FillRect(dc, &cell, brush[app.result.volumeBitmapAvailable ? level : 0]);
        }
        for (size_t i = 0; i < 5; ++i) DeleteObject(brush[i]);
        if (!app.result.volumeBitmapAvailable) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(53, 66, 78));
            DrawTextW(dc, L"Allocation-map preview requires an elevated NTFS analysis",
                -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_MOUSEMOVE && app.result.volumeBitmapAvailable &&
        app.result.allocationSamples.size() == 2048) {
        RECT rect = {};
        GetClientRect(window, &rect);
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        if (width > 0 && height > 0) {
            int x = LOWORD(lParam), y = HIWORD(lParam);
            int col = x * 128 / width, row = y * 16 / height;
            if (col >= 0 && col < 128 && row >= 0 && row < 16) {
                const int index = row * 128 + col;
                const int percent = app.result.allocationSamples[index] * 100 / 255;
                const std::wstring message = L"Allocation sample " + Number(index + 1) +
                    L" / 2048: approximately " + Number(percent) + L"% allocated";
                SendMessageW(app.status, SB_SETTEXTW, 0,
                    reinterpret_cast<LPARAM>(message.c_str()));
            }
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void InsertColumn(HWND list, int position, const wchar_t* title, int width) {
    LVCOLUMNW column = {};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<LPWSTR>(title);
    column.cx = width;
    column.iSubItem = position;
    ListView_InsertColumn(list, position, &column);
}

void CreateMenus() {
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu(), actions = CreatePopupMenu();
    HMENU view = CreatePopupMenu(), help = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IdRefresh, L"Refresh drives");
    AppendMenuW(file, MF_STRING, IdExport, L"Export fragmentation CSV...");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IdExit, L"Exit");
    AppendMenuW(actions, MF_STRING, IdAnalyze, L"Analyze volume");
    AppendMenuW(actions, MF_STRING, IdPreview, L"Preview selected strategy");
    AppendMenuW(actions, MF_STRING, IdOptimize, L"Run selected strategy");
    AppendMenuW(actions, MF_STRING, IdPause, L"Pause / Resume");
    AppendMenuW(actions, MF_STRING, IdCancel, L"Cancel operation");
    AppendMenuW(view, MF_STRING, IdTabFiles, L"Fragmented files");
    AppendMenuW(view, MF_STRING, IdTabFolders, L"Folder hotspots");
    AppendMenuW(view, MF_STRING, IdClearFilter, L"Clear search filter");
    AppendMenuW(help, MF_STRING, IdAbout, L"About DefragForge...");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"File");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(actions), L"Actions");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"View");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"Help");
    SetMenu(app.window, menu);
}

void CreateUi() {
    app.font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    CreateMenus();
    app.drives = Add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IdDrive);
    app.refresh = Add(WC_BUTTONW, L"Drives", BS_PUSHBUTTON | WS_TABSTOP, IdRefresh);
    app.analyze = Add(WC_BUTTONW, L"Analyze", BS_PUSHBUTTON | WS_TABSTOP, IdAnalyze);
    app.strategy = Add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IdStrategy);
    app.preview = Add(WC_BUTTONW, L"Preview", BS_PUSHBUTTON | WS_TABSTOP, IdPreview);
    app.optimize = Add(WC_BUTTONW, L"Run", BS_PUSHBUTTON | WS_TABSTOP, IdOptimize);
    app.pause = Add(WC_BUTTONW, L"Pause", BS_PUSHBUTTON | WS_TABSTOP, IdPause);
    app.cancel = Add(WC_BUTTONW, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IdCancel);
    app.exportButton = Add(WC_BUTTONW, L"Export CSV", BS_PUSHBUTTON | WS_TABSTOP, IdExport);
    app.headline = Add(WC_STATICW, L"", SS_LEFT, IdHeadline);
    app.confirm = Add(WC_BUTTONW, L"", BS_AUTOCHECKBOX | WS_TABSTOP, IdConfirm);
    app.mapTitle = Add(WC_STATICW, L"PHYSICAL ALLOCATION MAP (2048 SAMPLES - APPROXIMATE SPACE OCCUPANCY, NOT FRAGMENTATION)", SS_LEFT, IdMapTitle);
    app.map = Add(L"DefragForgeMap", L"", WS_BORDER, IdMap);
    app.legend = Add(WC_STATICW, L"Free / mostly free                 Lightly allocated                 Used                 Mostly allocated                 Full", SS_LEFT, IdLegend);
    app.statistics = Add(WC_STATICW, L"Read-only analysis provides file, folder and cluster-map statistics.", SS_LEFT, IdStatistics);
    app.selectedOnly = Add(WC_BUTTONW, L"Selected only", BS_AUTOCHECKBOX | WS_TABSTOP, IdSelectedOnly);
    app.filter = Add(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, IdSearch, WS_EX_CLIENTEDGE);
    app.clearFilter = Add(WC_BUTTONW, L"Clear filter", BS_PUSHBUTTON, IdClearFilter);
    app.tabs = Add(WC_TABCONTROLW, L"", WS_TABSTOP | TCS_TABS, IdTabs);
    TCITEMW tab = {};
    tab.mask = TCIF_TEXT;
    tab.pszText = const_cast<LPWSTR>(L"Fragmented files");
    TabCtrl_InsertItem(app.tabs, 0, &tab);
    tab.pszText = const_cast<LPWSTR>(L"Folder hotspots");
    TabCtrl_InsertItem(app.tabs, 1, &tab);
    app.files = Add(WC_LISTVIEWW, L"", LVS_REPORT | LVS_OWNERDATA | WS_TABSTOP,
        IdFiles, WS_EX_CLIENTEDGE);
    app.folders = Add(WC_LISTVIEWW, L"", LVS_REPORT | LVS_OWNERDATA | WS_TABSTOP,
        IdFolders, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(app.files,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    ListView_SetExtendedListViewStyle(app.folders,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    InsertColumn(app.files, 0, L"File", 620);
    InsertColumn(app.files, 1, L"Size", 110);
    InsertColumn(app.files, 2, L"Extents", 90);
    InsertColumn(app.files, 3, L"Excess", 90);
    InsertColumn(app.files, 4, L"Priority", 90);
    InsertColumn(app.folders, 0, L"Folder", 660);
    InsertColumn(app.folders, 1, L"Files", 110);
    InsertColumn(app.folders, 2, L"Excess extents", 130);
    InsertColumn(app.folders, 3, L"Fragmented bytes", 150);
    app.log = Add(WC_EDITW, L"DefragForge ready. Use Analyze to inspect the selected volume.\r\n",
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
        IdLog, WS_EX_CLIENTEDGE);
    app.status = Add(STATUSCLASSNAMEW, L"Ready", SBARS_SIZEGRIP, IdStatus);
    app.progress = Add(PROGRESS_CLASSW, L"", PBS_SMOOTH, 0);
    SendMessageW(app.progress, PBM_SETRANGE32, 0, 100);
    PopulateVolumes();
    ShowCurrentTab();
    RECT area = {};
    GetClientRect(app.window, &area);
    Layout(area.right, area.bottom);
    RefreshButtons();
}

void ShowListText(NMLVDISPINFOW* notification) {
    LVITEMW& item = notification->item;
    if (!(item.mask & LVIF_TEXT)) return;
    static wchar_t buffer[4096];
    buffer[0] = 0;
    if (item.iItem < 0) { item.pszText = buffer; return; }
    std::wstring value;
    if (notification->hdr.hwndFrom == app.files &&
        static_cast<size_t>(item.iItem) < app.fileRows.size()) {
        const nd::FileRecord& record = app.result.files[app.fileRows[item.iItem]];
        switch (item.iSubItem) {
        case 0: value = record.path; break;
        case 1: value = nd::FormatSize(record.bytes); break;
        case 2: value = Number(record.extents); break;
        case 3: value = Number(record.extents - 1); break;
        case 4: value = FloatNumber(record.score); break;
        }
    } else if (notification->hdr.hwndFrom == app.folders &&
        static_cast<size_t>(item.iItem) < app.folderRows.size()) {
        const nd::FolderStat& folder = app.result.folders[app.folderRows[item.iItem]];
        switch (item.iSubItem) {
        case 0: value = folder.path; break;
        case 1: value = Number(folder.fragmentedFiles); break;
        case 2: value = Number(folder.excessExtents); break;
        case 3: value = nd::FormatSize(folder.bytes); break;
        }
    }
    wcsncpy_s(buffer, sizeof(buffer) / sizeof(buffer[0]), value.c_str(), _TRUNCATE);
    item.pszText = buffer;
}

void ClearDriveResults() {
    app.hasScan = false;
    app.result = nd::ScanResult();
    SendMessageW(app.confirm, BM_SETCHECK, BST_UNCHECKED, 0);
    SetWindowTextW(app.filter, L"");
    SendMessageW(app.selectedOnly, BM_SETCHECK, BST_UNCHECKED, 0);
    RebuildVisibleRows();
    SetStatistics();
    InvalidateRect(app.map, NULL, TRUE);
}

LRESULT CALLBACK MainProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: app.window = window; CreateUi(); return 0;
    case WM_SIZE: Layout(LOWORD(lParam), HIWORD(lParam)); return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 1070;
        info->ptMinTrackSize.y = 620;
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IdDrive:
            if (HIWORD(wParam) == CBN_SELCHANGE) { ClearDriveResults(); SetHeadline(); }
            break;
        case IdRefresh:
            if (!app.active) { ClearDriveResults(); PopulateVolumes(); LogLine(L"Volume list refreshed."); }
            break;
        case IdStrategy:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                SendMessageW(app.confirm, BM_SETCHECK, BST_UNCHECKED, 0);
                RefreshButtons();
            }
            break;
        case IdSearch:
            if (HIWORD(wParam) == EN_CHANGE) RebuildVisibleRows();
            break;
        case IdClearFilter: SetWindowTextW(app.filter, L""); break;
        case IdConfirm: case IdSelectedOnly: RefreshButtons(); break;
        case IdAnalyze: if (!app.active) StartWorker(ModeAnalyze); break;
        case IdPreview: if (!app.active) PreviewPlan(); break;
        case IdOptimize: if (!app.active) StartOptimization(); break;
        case IdPause:
            if (app.active && (app.active->mode == ModeAnalyze || app.active->mode == ModeQuick)) {
                const LONG prior = InterlockedCompareExchange(&app.active->paused, 0, 0);
                InterlockedExchange(&app.active->paused, prior ? 0 : 1);
                LogLine(prior ? L"Resuming..." : L"Paused between filesystem operations.");
                RefreshButtons();
            }
            break;
        case IdCancel:
            if (app.active) {
                InterlockedExchange(&app.active->cancel, 1);
                InterlockedExchange(&app.active->paused, 0);
                LogLine(L"Cancellation requested...");
                RefreshButtons();
            }
            break;
        case IdExport: if (!app.active) ExportCsv(); break;
        case IdExit: SendMessageW(window, WM_CLOSE, 0, 0); break;
        case IdTabFiles: TabCtrl_SetCurSel(app.tabs, 0); ShowCurrentTab(); break;
        case IdTabFolders: TabCtrl_SetCurSel(app.tabs, 1); ShowCurrentTab(); break;
        case IdAbout:
            MessageBoxW(window,
                L"DefragForge - Native NTFS Defragmentation Workbench\n\n"
                L"Read-only cluster map, fragmented file browser, folder hotspots, CSV export, "
                L"bounded manual HDD relocation, Windows HDD optimization and SSD ReTrim.\n\n"
                L"Experimental project. Traditional defragmentation on SSDs is disabled.\n"
                L"MIT licensed, source-only.", L"About DefragForge", MB_ICONINFORMATION);
            break;
        }
        return 0;
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wParam) == app.files ||
            reinterpret_cast<HWND>(wParam) == app.folders)
            ContextMenu(reinterpret_cast<HWND>(wParam), lParam);
        return 0;
    case WM_NOTIFY: {
        NMHDR* notice = reinterpret_cast<NMHDR*>(lParam);
        if (!notice) break;
        if (notice->hwndFrom == app.tabs && notice->code == TCN_SELCHANGE) {
            ShowCurrentTab(); return 0;
        }
        if (notice->hwndFrom == app.files || notice->hwndFrom == app.folders) {
            if (notice->code == LVN_GETDISPINFOW) {
                ShowListText(reinterpret_cast<NMLVDISPINFOW*>(lParam));
                return 0;
            }
            if (notice->code == LVN_COLUMNCLICK) {
                NMLISTVIEW* clicked = reinterpret_cast<NMLISTVIEW*>(lParam);
                if (notice->hwndFrom == app.files) {
                    const bool same = app.fileSortColumn == clicked->iSubItem;
                    app.fileSortDescending = same ? !app.fileSortDescending : clicked->iSubItem != 0;
                    app.fileSortColumn = clicked->iSubItem;
                } else {
                    const bool same = app.folderSortColumn == clicked->iSubItem;
                    app.folderSortDescending = same ? !app.folderSortDescending : clicked->iSubItem != 0;
                    app.folderSortColumn = clicked->iSubItem;
                }
                RebuildVisibleRows(); return 0;
            }
            if (notice->code == NM_DBLCLK && notice->hwndFrom == app.folders) {
                ChangeFolderFilter(); return 0;
            }
        }
        break;
    }
    case MsgLog: {
        std::wstring* line = reinterpret_cast<std::wstring*>(lParam);
        if (line) { LogLine(*line); delete line; }
        return 0;
    }
    case MsgProgress:
        SendMessageW(app.progress, PBM_SETPOS, wParam, 0);
        return 0;
    case MsgFinished: {
        WorkRequest* work = reinterpret_cast<WorkRequest*>(wParam);
        if (!app.active || app.active != work) return 0;
        const bool analyzed = work->mode == ModeAnalyze;
        const WorkMode mode = work->mode;
        app.result = work->result;
        app.hasScan = analyzed && !app.result.cancelled && app.result.ntfs &&
            app.result.lastError == ERROR_SUCCESS;
        if (analyzed && app.hasScan) PopulateResults();
        else {
            RebuildVisibleRows();
            InvalidateRect(app.map, NULL, TRUE);
            if (!analyzed) {
                LogLine(mode == ModeQuick ?
                    L"Quick relocation finished. Re-analyze to refresh physical allocation." :
                    L"Windows optimizer finished. Re-analyze to compare results.");
                if (mode == ModeQuick) LogLine(L"Moved: " + Number(app.result.moved) +
                    L"   Failed: " + Number(app.result.moveFailed));
            }
            SetStatistics();
        }
        if (app.result.cancelled) LogLine(L"Operation cancelled.");
        if (app.result.lastError != ERROR_SUCCESS)
            LogLine(L"Windows error: " + nd::FormatWindowsError(app.result.lastError));
        if (app.thread) { WaitForSingleObject(app.thread, INFINITE); CloseHandle(app.thread); app.thread = NULL; }
        delete app.active;
        app.active = NULL;
        SendMessageW(app.confirm, BM_SETCHECK, BST_UNCHECKED, 0);
        RefreshButtons();
        if (app.closing) DestroyWindow(app.window);
        return 0;
    }
    case WM_CLOSE:
        if (app.active) {
            app.closing = true;
            InterlockedExchange(&app.active->cancel, 1);
            InterlockedExchange(&app.active->paused, 0);
            LogLine(L"Waiting for current filesystem operation to finish before closing...");
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_LISTVIEW_CLASSES |
        ICC_PROGRESS_CLASS | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&controls);
    WNDCLASSEXW mapClass = {};
    mapClass.cbSize = sizeof(mapClass);
    mapClass.hInstance = instance;
    mapClass.lpfnWndProc = MapProc;
    mapClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    mapClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    mapClass.lpszClassName = L"DefragForgeMap";
    RegisterClassExW(&mapClass);
    WNDCLASSEXW mainClass = {};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.hInstance = instance;
    mainClass.lpfnWndProc = MainProc;
    mainClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    mainClass.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    mainClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    mainClass.lpszClassName = L"DefragForgeWindow";
    if (!RegisterClassExW(&mainClass)) return 1;
    HWND window = CreateWindowExW(0, mainClass.lpszClassName,
        L"DefragForge - Classic NTFS Defragmentation Workbench",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        1260, 850, NULL, NULL, instance, NULL);
    if (!window) return 2;
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message = {};
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return static_cast<int>(message.wParam);
}
