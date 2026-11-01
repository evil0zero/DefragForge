#include "BuildTarget.h"
#include "Engine.h"
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <process.h>
#include <algorithm>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace {

enum ControlId {
    IdVolume = 100, IdAnalyze, IdPreview, IdOptimize, IdCancel, IdExport,
    IdConfirm, IdFiles, IdLog, IdStatus, IdMap
};

enum WorkMode { ModeAnalyze, ModeOptimize };
const UINT MsgLog = WM_APP + 1;
const UINT MsgProgress = WM_APP + 2;
const UINT MsgFinished = WM_APP + 3;

struct WorkRequest {
    HWND window;
    std::wstring root;
    WorkMode mode;
    nd::ScanResult result;
    volatile LONG cancel;
    WorkRequest() : window(NULL), mode(ModeAnalyze), cancel(0) {}
};

struct Application {
    HWND window, drives, analyze, preview, optimize, cancel, exportButton, confirm;
    HWND files, log, status, progress, map, headline;
    HFONT font;
    HANDLE thread;
    WorkRequest* active;
    nd::ScanResult result;
    std::vector<nd::VolumeInfo> volumes;
    bool hasScan;
    bool closing;
    Application() : window(NULL), drives(NULL), analyze(NULL), preview(NULL),
        optimize(NULL), cancel(NULL), exportButton(NULL), confirm(NULL), files(NULL),
        log(NULL), status(NULL), progress(NULL), map(NULL), headline(NULL), font(NULL),
        thread(NULL), active(NULL), hasScan(false), closing(false) {}
};
Application app;

std::wstring Number(unsigned long long value) {
    wchar_t buffer[64];
    _snwprintf(buffer, 63, L"%I64u", value);
    buffer[63] = 0;
    return buffer;
}

void UseFont(HWND control) { SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE); }

HWND AddControl(const wchar_t* kind, const wchar_t* title, DWORD style, int id,
    DWORD additional = 0) {
    HWND control = CreateWindowExW(additional, kind, title, WS_CHILD | WS_VISIBLE | style,
        0, 0, 10, 10, app.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(NULL), NULL);
    if (control && app.font) UseFont(control);
    return control;
}

void LogLine(const std::wstring& text) {
    if (!app.log) return;
    int length = GetWindowTextLengthW(app.log);
    SendMessageW(app.log, EM_SETSEL, length, length);
    std::wstring line = text + L"\r\n";
    SendMessageW(app.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(app.log, EM_SCROLLCARET, 0, 0);
    // Avoid unlimited memory consumption during very long scans.
    if (GetWindowTextLengthW(app.log) > 24000) {
        SendMessageW(app.log, EM_SETSEL, 0, 6000);
        SendMessageW(app.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
    }
}

nd::VolumeInfo* SelectedVolume() {
    int index = static_cast<int>(SendMessageW(app.drives, CB_GETCURSEL, 0, 0));
    if (index < 0 || static_cast<size_t>(index) >= app.volumes.size()) return NULL;
    return &app.volumes[index];
}

void RefreshButtons() {
    bool busy = app.active != NULL;
    nd::VolumeInfo* volume = SelectedVolume();
    bool ntfs = volume && volume->ntfs;
    bool hddOkay = volume && volume->media != nd::MediaSsd;
    bool confirmed = SendMessageW(app.confirm, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(app.drives, !busy);
    EnableWindow(app.analyze, !busy && ntfs);
    EnableWindow(app.preview, !busy && app.hasScan);
    EnableWindow(app.optimize, !busy && app.hasScan && ntfs && hddOkay && confirmed);
    EnableWindow(app.cancel, busy);
    EnableWindow(app.exportButton, !busy && app.hasScan);
    EnableWindow(app.confirm, !busy && hddOkay && ntfs);
}

void SetHeadline() {
    nd::VolumeInfo* info = SelectedVolume();
    if (!info) return;
    std::wstring kind = info->media == nd::MediaHdd ? L"Mechanical HDD" :
        info->media == nd::MediaSsd ? L"Solid-state disk" : L"Unknown media";
    std::wstring line = info->filesystem + L"  |  " + kind + L"  |  " +
        nd::FormatSize(info->bytesTotal) + L" total  |  " + nd::FormatSize(info->bytesFree) + L" free";
    SetWindowTextW(app.headline, line.c_str());
    RefreshButtons();
}

void PopulateVolumes() {
    app.volumes = nd::GetLocalVolumes();
    SendMessageW(app.drives, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < app.volumes.size(); ++i) {
        const nd::VolumeInfo& v = app.volumes[i];
        std::wstring text = v.root + L"  " + (v.label.empty() ? L"Local Disk" : v.label) +
            L"  (" + v.filesystem + L")";
        SendMessageW(app.drives, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    if (!app.volumes.empty()) SendMessageW(app.drives, CB_SETCURSEL, 0, 0);
    SetHeadline();
}

void PopulateResults() {
    ListView_DeleteAllItems(app.files);
    const size_t maxRows = 1500;
    size_t total = app.result.files.size();
    size_t visible = total < maxRows ? total : maxRows;
    for (size_t i = 0; i < visible; ++i) {
        const nd::FileRecord& item = app.result.files[i];
        LVITEMW row = {};
        row.mask = LVIF_TEXT;
        row.iItem = static_cast<int>(i);
        row.pszText = const_cast<LPWSTR>(item.path.c_str());
        ListView_InsertItem(app.files, &row);
        std::wstring size = nd::FormatSize(item.bytes);
        std::wstring extents = Number(item.extents);
        wchar_t rank[40];
        _snwprintf(rank, 39, L"%.2f", item.score);
        rank[39] = 0;
        ListView_SetItemText(app.files, static_cast<int>(i), 1, const_cast<LPWSTR>(size.c_str()));
        ListView_SetItemText(app.files, static_cast<int>(i), 2, const_cast<LPWSTR>(extents.c_str()));
        ListView_SetItemText(app.files, static_cast<int>(i), 3, rank);
    }
    std::wstring summary = L"Files scanned: " + Number(app.result.scanned) +
        L"  |  Fragmented: " + Number(app.result.fragmented) +
        L"  |  Access denied/unmapped: " + Number(app.result.inaccessible);
    if (!app.result.volumeBitmapAvailable) summary += L"  |  Map unavailable (run elevated)";
    SendMessageW(app.status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(summary.c_str()));
    InvalidateRect(app.map, NULL, TRUE);
    if (visible < total) LogLine(L"Displaying top 1,500 candidates. CSV export contains all results.");
}

class WindowObserver : public nd::Observer {
public:
    explicit WindowObserver(WorkRequest* work) : work_(work) {}
    virtual bool Cancelled() const { return InterlockedCompareExchange(&work_->cancel, 0, 0) != 0; }
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
    WorkRequest* work = static_cast<WorkRequest*>(data);
    WindowObserver observer(work);
    if (work->mode == ModeAnalyze) nd::Analyze(work->root, observer, work->result);
    else nd::Optimize(work->root, observer, work->result);
    PostMessageW(work->window, MsgFinished, reinterpret_cast<WPARAM>(work), 0);
    return 0;
}

void StartWorker(WorkMode mode) {
    if (app.active) return;
    nd::VolumeInfo* volume = SelectedVolume();
    if (!volume || !volume->ntfs) return;
    WorkRequest* request = new WorkRequest();
    request->window = app.window;
    request->root = volume->root;
    request->mode = mode;
    if (mode == ModeOptimize) request->result = app.result;
    unsigned threadId = 0;
    uintptr_t handle = _beginthreadex(NULL, 0, Worker, request, 0, &threadId);
    if (!handle) { delete request; MessageBoxW(app.window, L"Could not start worker thread.", L"DefragForge", MB_ICONERROR); return; }
    app.active = request;
    app.thread = reinterpret_cast<HANDLE>(handle);
    SendMessageW(app.progress, PBM_SETPOS, 0, 0);
    LogLine(mode == ModeAnalyze ? L"Starting read-only NTFS analysis..." : L"Beginning capped relocation pass...");
    RefreshButtons();
}

void PreviewPlan() {
    const nd::PlanLimits limits = nd::DefaultLimits();
    unsigned int eligible = 0;
    unsigned long long eligibleBytes = 0;
    for (size_t i = 0; i < app.result.files.size(); ++i) {
        const nd::FileRecord& file = app.result.files[i];
        if (file.score <= 0) continue;
        nd::Candidate candidate = {};
        candidate.fileBytes = file.bytes;
        candidate.clusterCount = file.clusters;
        candidate.extents = file.extents;
        if (nd::IsEligible(candidate, limits)) { ++eligible; eligibleBytes += file.bytes; }
    }
    std::wstring message = L"Read-only plan estimate\n\nEligible candidates: " + Number(eligible) +
        L"\nPotential file bytes: " + nd::FormatSize(eligibleBytes) +
        L"\nMaximum files attempted per pass: " + Number(limits.maximumMoves) +
        L"\nMaximum individual file size: " + nd::FormatSize(limits.maximumFileBytes) +
        L"\n\nActual availability and fragmentation are checked again before every move. "
        L"This is a priority estimate, not a guarantee of performance improvement.";
    MessageBoxW(app.window, message.c_str(), L"DefragForge - Preview", MB_OK | MB_ICONINFORMATION);
}

void StartOptimization() {
    nd::VolumeInfo* volume = SelectedVolume();
    if (!volume || volume->media == nd::MediaSsd) return;
    const wchar_t* text =
        L"EXPERIMENTAL HDD RELOCATION\n\n"
        L"This changes NTFS allocation on the selected drive. Files currently being written "
        L"may be skipped. A power failure, storage fault, or filesystem problem can cause data loss.\n\n"
        L"Before proceeding, make a verified backup and ensure reliable power. "
        L"Only NTFS filesystem-managed cluster moves will be used, capped at 16 small files per pass.\n\n"
        L"Are you certain this is a mechanical HDD and that you want to continue?";
    if (MessageBoxW(app.window, text, L"Confirm filesystem writes", MB_YESNO | MB_DEFBUTTON2 |
        MB_ICONWARNING) == IDYES) StartWorker(ModeOptimize);
}

void ExportReport() {
    wchar_t filename[MAX_PATH] = L"DefragForge-report.csv";
    const wchar_t filter[] = L"CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0\0";
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = app.window;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = filename;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"csv";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (GetSaveFileNameW(&dialog)) {
        if (nd::WriteCsvReport(filename, app.result)) LogLine(L"Saved CSV report: " + std::wstring(filename));
        else MessageBoxW(app.window, L"Could not write CSV report.", L"Export failed", MB_ICONERROR);
    }
}

void Layout(int width, int height) {
    if (!app.drives) return;
    const int margin = 12;
    const int controlsY = 12;
    int comboWidth = width > 960 ? 235 : 185;
    MoveWindow(app.drives, margin, controlsY, comboWidth, 250, TRUE);
    int x = margin + comboWidth + 8;
    struct Position { HWND item; int width; } controls[] = {
        {app.analyze, 82}, {app.preview, 85}, {app.optimize, 105}, {app.cancel, 70}, {app.exportButton, 80}
    };
    for (size_t i = 0; i < sizeof(controls)/sizeof(controls[0]); ++i) {
        MoveWindow(controls[i].item, x, controlsY, controls[i].width, 26, TRUE);
        x += controls[i].width + 6;
    }
    MoveWindow(app.headline, margin, 46, width - 2 * margin, 20, TRUE);
    MoveWindow(app.confirm, margin, 69, width - 2 * margin, 22, TRUE);
    MoveWindow(app.map, margin, 97, width - 2 * margin, 88, TRUE);
    int bottom = height - 28;
    int logHeight = height < 550 ? 80 : 112;
    int logTop = bottom - logHeight - 5;
    int listTop = 192;
    int listHeight = logTop - listTop - 6;
    if (listHeight < 80) listHeight = 80;
    MoveWindow(app.files, margin, listTop, width - 2 * margin, listHeight, TRUE);
    MoveWindow(app.log, margin, logTop, width - 2 * margin, logHeight, TRUE);
    MoveWindow(app.status, 0, height - 26, width, 26, TRUE);
    MoveWindow(app.progress, width - 190, height - 22, 180, 15, TRUE);
}

LRESULT CALLBACK MapProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        RECT rect;
        GetClientRect(window, &rect);
        HBRUSH background = CreateSolidBrush(RGB(246, 248, 250));
        FillRect(dc, &rect, background);
        DeleteObject(background);
        const int columns = 64, rows = 8;
        int width = rect.right - rect.left, height = rect.bottom - rect.top;
        for (int i = 0; i < columns * rows; ++i) {
            int intensity = app.result.allocationSamples.size() == 512 ? app.result.allocationSamples[i] : 0;
            RECT cell = {i % columns * width / columns + 1,
                i / columns * height / rows + 1,
                (i % columns + 1) * width / columns - 1,
                (i / columns + 1) * height / rows - 1};
            COLORREF color = app.result.volumeBitmapAvailable ?
                RGB(42 + intensity * 150 / 255, 154 - intensity * 90 / 255, 94 + intensity * 65 / 255) :
                RGB(215, 220, 226);
            HBRUSH brush = CreateSolidBrush(color);
            FillRect(dc, &cell, brush);
            DeleteObject(brush);
        }
        if (!app.result.volumeBitmapAvailable) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(55, 64, 75));
            DrawTextW(dc, L"Allocation map appears after elevated NTFS analysis", -1, &rect,
                DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void CreateUi() {
    app.font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    app.drives = AddControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IdVolume);
    app.analyze = AddControl(WC_BUTTONW, L"Analyze", BS_PUSHBUTTON | WS_TABSTOP, IdAnalyze);
    app.preview = AddControl(WC_BUTTONW, L"Preview", BS_PUSHBUTTON | WS_TABSTOP, IdPreview);
    app.optimize = AddControl(WC_BUTTONW, L"Optimize HDD", BS_PUSHBUTTON | WS_TABSTOP, IdOptimize);
    app.cancel = AddControl(WC_BUTTONW, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IdCancel);
    app.exportButton = AddControl(WC_BUTTONW, L"Export CSV", BS_PUSHBUTTON | WS_TABSTOP, IdExport);
    app.headline = AddControl(WC_STATICW, L"", SS_LEFT, 0);
    app.confirm = AddControl(WC_BUTTONW,
        L"I have a verified backup and confirm this device is a mechanical HDD (required for experimental writes)",
        BS_AUTOCHECKBOX | WS_TABSTOP, IdConfirm);
    app.map = AddControl(L"DefragForgeMap", L"", WS_BORDER, IdMap);
    app.files = AddControl(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | WS_TABSTOP, IdFiles, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(app.files, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    struct Column { const wchar_t* text; int width; } columns[] = {
        {L"File", 540}, {L"Size", 100}, {L"Extents", 85}, {L"Priority", 90}
    };
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW column = {};
        column.mask = LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
        column.cx = columns[i].width;
        column.pszText = const_cast<LPWSTR>(columns[i].text);
        column.iSubItem = i;
        ListView_InsertColumn(app.files, i, &column);
    }
    app.log = AddControl(WC_EDITW, L"DefragForge ready. Analysis is read-only.",
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, IdLog, WS_EX_CLIENTEDGE);
    app.status = AddControl(STATUSCLASSNAMEW, L"Ready", SBARS_SIZEGRIP, IdStatus);
    app.progress = AddControl(PROGRESS_CLASSW, L"", PBS_SMOOTH, 0);
    SendMessageW(app.progress, PBM_SETRANGE32, 0, 100);
    PopulateVolumes();
    RECT area;
    GetClientRect(app.window, &area);
    Layout(area.right, area.bottom);
    RefreshButtons();
}

LRESULT CALLBACK MainProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: app.window = window; CreateUi(); return 0;
    case WM_SIZE: Layout(LOWORD(lParam), HIWORD(lParam)); return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 820;
        info->ptMinTrackSize.y = 470;
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IdVolume:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                app.hasScan = false;
                app.result = nd::ScanResult();
                ListView_DeleteAllItems(app.files);
                SendMessageW(app.confirm, BM_SETCHECK, BST_UNCHECKED, 0);
                SetHeadline();
                InvalidateRect(app.map, NULL, TRUE);
            }
            break;
        case IdConfirm: RefreshButtons(); break;
        case IdAnalyze: StartWorker(ModeAnalyze); break;
        case IdPreview: PreviewPlan(); break;
        case IdOptimize: StartOptimization(); break;
        case IdCancel:
            if (app.active) { InterlockedExchange(&app.active->cancel, 1); LogLine(L"Cancellation requested..."); }
            break;
        case IdExport: ExportReport(); break;
        }
        return 0;
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
        if (!app.active || work != app.active) return 0;
        bool isAnalyze = work->mode == ModeAnalyze;
        app.result = work->result;
        app.hasScan = isAnalyze && app.result.ntfs && app.result.lastError == ERROR_SUCCESS;
        if (isAnalyze) PopulateResults();
        else {
            app.hasScan = false;
            LogLine(L"Re-analyze the drive before any additional relocation pass.");
        }
        if (app.result.lastError != ERROR_SUCCESS) LogLine(L"Windows reported: " + nd::FormatWindowsError(app.result.lastError));
        if (app.thread) { WaitForSingleObject(app.thread, INFINITE); CloseHandle(app.thread); app.thread = NULL; }
        delete app.active;
        app.active = NULL;
        RefreshButtons();
        if (app.closing) DestroyWindow(app.window);
        return 0;
    }
    case WM_CLOSE:
        if (app.active) {
            app.closing = true;
            InterlockedExchange(&app.active->cancel, 1);
            LogLine(L"Closing after the current filesystem operation ends...");
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
        ICC_PROGRESS_CLASS | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&controls);
    WNDCLASSEXW mapClass = {};
    mapClass.cbSize = sizeof(mapClass);
    mapClass.hInstance = instance;
    mapClass.lpfnWndProc = MapProc;
    mapClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    mapClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    mapClass.lpszClassName = L"DefragForgeMap";
    RegisterClassExW(&mapClass);
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = MainProc;
    windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    windowClass.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = L"DefragForgeWindow";
    if (!RegisterClassExW(&windowClass)) return 1;
    HWND window = CreateWindowExW(0, windowClass.lpszClassName,
        L"DefragForge - Native NTFS Defragmentation Workbench", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1040, 680, NULL, NULL, instance, NULL);
    if (!window) return 2;
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return static_cast<int>(message.wParam);
}
