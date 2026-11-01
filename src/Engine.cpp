#include "Engine.h"
#include <winioctl.h>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <vector>

namespace nd {

namespace {

class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE && value_ != NULL) CloseHandle(value_); }
    HANDLE Get() const { return value_; }
    bool Valid() const { return value_ != INVALID_HANDLE_VALUE && value_ != NULL; }
private:
    Handle(const Handle&);
    Handle& operator=(const Handle&);
    HANDLE value_;
};

struct Extent {
    unsigned long long vcn;
    unsigned long long lcn;
    unsigned long long count;
};

bool StartsWithI(const std::wstring& text, const wchar_t* prefix) {
    size_t length = wcslen(prefix);
    return text.size() >= length && _wcsnicmp(text.c_str(), prefix, length) == 0;
}

bool EndsWithI(const std::wstring& text, const wchar_t* suffix) {
    size_t length = wcslen(suffix);
    return text.size() >= length && _wcsicmp(text.c_str() + text.size() - length, suffix) == 0;
}

bool UnsafeFile(const std::wstring& path, DWORD attributes) {
    if (attributes & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT |
                      FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE |
                      FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_DIRECTORY)) return true;
    std::wstring relative = path.size() > 3 ? path.substr(3) : L"";
    const wchar_t* blocked[] = {L"Windows\\", L"Program Files\\", L"Program Files (x86)\\",
        L"ProgramData\\", L"System Volume Information\\", L"$Recycle.Bin\\", L"Boot\\",
        L"Recovery\\", L"Documents and Settings\\"};
    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); ++i) {
        if (StartsWithI(relative, blocked[i])) return true;
    }
    return EndsWithI(path, L"\\pagefile.sys") || EndsWithI(path, L"\\swapfile.sys") ||
        EndsWithI(path, L"\\hiberfil.sys") || EndsWithI(path, L"\\bootmgr");
}

bool ReadExtents(HANDLE file, std::vector<Extent>& extents) {
    extents.clear();
    STARTING_VCN_INPUT_BUFFER input = {};
    const DWORD bufferSize = 65536;
    std::vector<BYTE> storage(bufferSize);
    unsigned long long lastVcn = 0;
    for (unsigned int pass = 0; pass < 4096; ++pass) {
        DWORD returned = 0;
        BOOL ok = DeviceIoControl(file, FSCTL_GET_RETRIEVAL_POINTERS,
            &input, sizeof(input), &storage[0], bufferSize, &returned, NULL);
        DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && error == ERROR_HANDLE_EOF) return !extents.empty();
        if (!ok && error != ERROR_MORE_DATA) return false;
        if (returned < FIELD_OFFSET(RETRIEVAL_POINTERS_BUFFER, Extents)) return false;
        RETRIEVAL_POINTERS_BUFFER* data = reinterpret_cast<RETRIEVAL_POINTERS_BUFFER*>(&storage[0]);
        const size_t header = FIELD_OFFSET(RETRIEVAL_POINTERS_BUFFER, Extents);
        const size_t capacity = (returned - header) / sizeof(data->Extents[0]);
        if (data->ExtentCount == 0 || data->ExtentCount > capacity) return false;
        unsigned long long previous = static_cast<unsigned long long>(data->StartingVcn.QuadPart);
        for (DWORD i = 0; i < data->ExtentCount; ++i) {
            const unsigned long long next = static_cast<unsigned long long>(data->Extents[i].NextVcn.QuadPart);
            if (next <= previous) return false;
            if (data->Extents[i].Lcn.QuadPart >= 0) {
                Extent run;
                run.vcn = previous;
                run.lcn = static_cast<unsigned long long>(data->Extents[i].Lcn.QuadPart);
                run.count = next - previous;
                extents.push_back(run);
            }
            previous = next;
        }
        if (ok) return true;
        if (previous <= lastVcn) return false;
        lastVcn = previous;
        input.StartingVcn.QuadPart = static_cast<LONGLONG>(previous);
    }
    return false;
}

std::wstring VolumePath(const std::wstring& root) {
    return L"\\\\.\\" + root.substr(0, 2);
}

bool GetVolumeBitmap(HANDLE volume, unsigned long long start, std::vector<BYTE>& output,
    unsigned long long& realStart, unsigned long long& count) {
    STARTING_LCN_INPUT_BUFFER input = {};
    input.StartingLcn.QuadPart = static_cast<LONGLONG>(start);
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(volume, FSCTL_GET_VOLUME_BITMAP, &input, sizeof(input),
        &output[0], static_cast<DWORD>(output.size()), &returned, NULL);
    if (!ok && GetLastError() != ERROR_MORE_DATA) return false;
    const DWORD header = FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer);
    if (returned <= header) return false;
    VOLUME_BITMAP_BUFFER* data = reinterpret_cast<VOLUME_BITMAP_BUFFER*>(&output[0]);
    if (data->StartingLcn.QuadPart < 0 || data->BitmapSize.QuadPart <= 0) return false;
    realStart = static_cast<unsigned long long>(data->StartingLcn.QuadPart);
    count = static_cast<unsigned long long>(returned - header) * 8ULL;
    if (count > static_cast<unsigned long long>(data->BitmapSize.QuadPart))
        count = static_cast<unsigned long long>(data->BitmapSize.QuadPart);
    return count != 0;
}

bool Used(const std::vector<BYTE>& buffer, unsigned long long bit) {
    const BYTE* bits = &buffer[0] + FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer);
    return (bits[bit / 8] & (1 << (bit % 8))) != 0;
}

bool FindFreeRun(HANDLE volume, unsigned long long needed, unsigned long long& found, Observer& observer) {
    std::vector<BYTE> buffer(65536 + sizeof(VOLUME_BITMAP_BUFFER));
    unsigned long long position = 0, freeStart = 0, freeCount = 0;
    while (!observer.Cancelled()) {
        unsigned long long realStart = 0, count = 0;
        if (!GetVolumeBitmap(volume, position, buffer, realStart, count)) return false;
        unsigned long long end = realStart + count;
        if (end <= position) return false;
        for (unsigned long long cluster = position; cluster < end; ++cluster) {
            if ((cluster & 65535ULL) == 0 && observer.Cancelled()) return false;
            if (!Used(buffer, cluster - realStart)) {
                if (freeCount == 0) freeStart = cluster;
                ++freeCount;
                if (freeCount >= needed) { found = freeStart; return true; }
            } else freeCount = 0;
        }
        VOLUME_BITMAP_BUFFER* data = reinterpret_cast<VOLUME_BITMAP_BUFFER*>(&buffer[0]);
        if (end >= realStart + static_cast<unsigned long long>(data->BitmapSize.QuadPart)) return false;
        position = end;
    }
    return false;
}

void ReadAllocationSamples(HANDLE volume, ScanResult& result, Observer& observer) {
    const unsigned int cells = 512;
    std::vector<unsigned long long> allocated(cells, 0), observed(cells, 0);
    std::vector<BYTE> buffer(65536 + sizeof(VOLUME_BITMAP_BUFFER));
    unsigned long long position = 0;
    unsigned long long volumeClusters = result.volumeClusters;
    if (volumeClusters == 0) return;
    while (!observer.Cancelled()) {
        unsigned long long realStart = 0, count = 0;
        if (!GetVolumeBitmap(volume, position, buffer, realStart, count)) break;
        unsigned long long end = realStart + count;
        if (end <= position) break;
        // Sample at most 16 points per 512-cluster interval to keep scanning responsive.
        unsigned long long step = count / 2048;
        if (step == 0) step = 1;
        for (unsigned long long cluster = position; cluster < end; cluster += step) {
            unsigned int cell = static_cast<unsigned int>((cluster * cells) / volumeClusters);
            if (cell >= cells) cell = cells - 1;
            ++observed[cell];
            if (Used(buffer, cluster - realStart)) ++allocated[cell];
        }
        VOLUME_BITMAP_BUFFER* data = reinterpret_cast<VOLUME_BITMAP_BUFFER*>(&buffer[0]);
        if (end >= realStart + static_cast<unsigned long long>(data->BitmapSize.QuadPart)) {
            result.volumeBitmapAvailable = true;
            break;
        }
        position = end;
    }
    result.allocationSamples.resize(cells);
    for (unsigned int i = 0; i < cells; ++i)
        result.allocationSamples[i] = observed[i] ? static_cast<unsigned char>(allocated[i] * 255 / observed[i]) : 0;
}

void ScanDirectory(const std::wstring& directory, Observer& observer, ScanResult& result,
    unsigned long long& lastNotify) {
    if (observer.Cancelled()) { result.cancelled = true; return; }
    WIN32_FIND_DATAW find = {};
    const std::wstring pattern = directory + L"*";
    HANDLE search = FindFirstFileW(pattern.c_str(), &find);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (observer.Cancelled()) { result.cancelled = true; break; }
        if (wcscmp(find.cFileName, L".") == 0 || wcscmp(find.cFileName, L"..") == 0) continue;
        if (find.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        std::wstring path = directory + find.cFileName;
        if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(find.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) &&
                !UnsafeFile(path + L"\\", find.dwFileAttributes & ~FILE_ATTRIBUTE_DIRECTORY))
                ScanDirectory(path + L"\\", observer, result, lastNotify);
            continue;
        }
        ++result.scanned;
        unsigned long long bytes = (static_cast<unsigned long long>(find.nFileSizeHigh) << 32) | find.nFileSizeLow;
        result.bytesScanned += bytes;
        if (bytes == 0) continue;
        Handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE |
            FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL));
        if (!file.Valid()) { ++result.inaccessible; continue; }
        std::vector<Extent> extents;
        if (!ReadExtents(file.Get(), extents)) { ++result.inaccessible; continue; }
        if (extents.size() > 1) {
            FileRecord record = {};
            record.path = path;
            record.bytes = bytes;
            record.attributes = find.dwFileAttributes;
            record.extents = static_cast<unsigned int>(extents.size());
            for (size_t i = 0; i < extents.size(); ++i) record.clusters += extents[i].count;
            Candidate candidate = {};
            candidate.fileBytes = record.bytes;
            candidate.clusterCount = record.clusters;
            candidate.extents = record.extents;
            candidate.sparseOrCompressed = (record.attributes & (FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_ENCRYPTED)) != 0;
            candidate.systemFile = UnsafeFile(path, record.attributes);
            record.score = IsEligible(candidate, DefaultLimits()) ? Score(candidate) : 0.0;
            result.files.push_back(record);
            ++result.fragmented;
        }
        if (result.scanned - lastNotify >= 750) {
            lastNotify = result.scanned;
            wchar_t status[160];
            _snwprintf(status, 159, L"Scanned %I64u files; fragmented: %I64u", result.scanned, result.fragmented);
            status[159] = 0;
            observer.Message(status);
        }
    } while (FindNextFileW(search, &find));
    FindClose(search);
}

bool FileScoreDescending(const FileRecord& a, const FileRecord& b) { return a.score > b.score; }

bool WindowsVersion(unsigned int& major, unsigned int& minor) {
    OSVERSIONINFOW info = {};
    info.dwOSVersionInfoSize = sizeof(info);
    typedef LONG (WINAPI *RtlGetVersionFunction)(OSVERSIONINFOW*);
    HMODULE library = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFunction getVersion = library ? reinterpret_cast<RtlGetVersionFunction>(
        GetProcAddress(library, "RtlGetVersion")) : NULL;
    if (getVersion && getVersion(&info) == 0) {
        major = info.dwMajorVersion;
        minor = info.dwMinorVersion;
        return true;
    }
    if (GetVersionExW(&info)) {
        major = info.dwMajorVersion;
        minor = info.dwMinorVersion;
        return true;
    }
    major = 0;
    minor = 0;
    return false;
}

bool DefragExecutable(std::wstring& executable) {
    wchar_t directory[MAX_PATH] = {};
    UINT count = GetSystemDirectoryW(directory, MAX_PATH);
    if (count == 0 || count >= MAX_PATH) return false;
    executable = std::wstring(directory) + L"\\defrag.exe";
    DWORD attributes = GetFileAttributesW(executable.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsSystemDrive(const std::wstring& root) {
    wchar_t directory[MAX_PATH] = {};
    return GetWindowsDirectoryW(directory, MAX_PATH) != 0 && !root.empty() &&
        towupper(root[0]) == towupper(directory[0]);
}

// Defrag /L output is captured from an inherited pipe. The helper is always the
// OS-installed system executable and the drive argument is a validated letter.
void RunRetrimProcess(const std::wstring& root, Observer& observer, ScanResult& result) {
    if (root.size() != 3 || !iswalpha(root[0]) || root[1] != L':' || root[2] != L'\\') {
        result.lastError = ERROR_INVALID_PARAMETER;
        return;
    }
    std::wstring executable;
    if (!DefragExecutable(executable)) {
        result.lastError = ERROR_FILE_NOT_FOUND;
        observer.Message(L"Windows defrag.exe was not found in the system directory.");
        return;
    }
    SECURITY_ATTRIBUTES security = {};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE readPipe = NULL, writePipe = NULL;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
        result.lastError = GetLastError();
        return;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    Handle readGuard(readPipe), writeGuard(writePipe);
    Handle nullInput(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullInput.Valid() ? nullInput.Get() : GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    PROCESS_INFORMATION process = {};
    std::wstring command = L"\"" + executable + L"\" " + root.substr(0, 2) + L" /L /U /V";
    std::vector<wchar_t> arguments(command.begin(), command.end());
    arguments.push_back(0);
    if (!CreateProcessW(executable.c_str(), &arguments[0], NULL, NULL, TRUE,
        CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
        result.lastError = GetLastError();
        observer.Message(L"Unable to start Windows ReTrim. Try running as administrator.");
        return;
    }
    Handle threadGuard(process.hThread), processGuard(process.hProcess);
    // Output is polled; parent pipe handles close on return.
    observer.Message(L"Windows is processing SSD ReTrim. This does not relocate file clusters.");
    unsigned int reported = 0;
    std::string pending;
    bool ended = false;
    while (!ended) {
        if (observer.Cancelled()) {
            result.cancelled = true;
            result.lastError = ERROR_CANCELLED;
            TerminateProcess(process.hProcess, ERROR_CANCELLED);
            observer.Message(L"ReTrim helper cancellation requested; Windows may finish already submitted work.");
        }
        DWORD available = 0;
        if (PeekNamedPipe(readPipe, NULL, 0, NULL, &available, NULL) && available > 0) {
            char chunk[2048];
            DWORD count = 0;
            DWORD length = available < sizeof(chunk) ? available : sizeof(chunk);
            if (ReadFile(readPipe, chunk, length, &count, NULL) && count) {
                pending.append(chunk, count);
                size_t lineEnd = 0;
                while ((lineEnd = pending.find_first_of("\r\n")) != std::string::npos) {
                    std::string line = pending.substr(0, lineEnd);
                    pending.erase(0, lineEnd + 1);
                    if (!line.empty() && reported < 100) {
                        int characters = MultiByteToWideChar(CP_OEMCP, 0, line.c_str(),
                            static_cast<int>(line.size()), NULL, 0);
                        if (characters > 0) {
                            std::vector<wchar_t> output(characters);
                            MultiByteToWideChar(CP_OEMCP, 0, line.c_str(),
                                static_cast<int>(line.size()), &output[0], characters);
                            observer.Message(std::wstring(output.begin(), output.end()));
                            ++reported;
                        }
                    }
                }
                if (pending.size() > 8192) pending.clear();
            }
        }
        ended = WaitForSingleObject(process.hProcess, 100) == WAIT_OBJECT_0;
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process.hProcess, &exitCode)) {
        if (result.lastError == ERROR_SUCCESS) result.lastError = GetLastError();
    } else if (!result.cancelled && exitCode != 0) {
        result.lastError = ERROR_GEN_FAILURE;
        wchar_t message[100] = {};
        _snwprintf(message, 99, L"Windows ReTrim failed, defrag.exe exit code %lu.", exitCode);
        observer.Message(message);
    } else if (!result.cancelled) {
        observer.Progress(100);
        observer.Message(L"SSD ReTrim completed successfully.");
    }
}


} // namespace

VolumeInfo::VolumeInfo() : bytesTotal(0), bytesFree(0), clusterBytes(0), media(MediaUnknown),
    ntfs(false), trimKnown(false), trimEnabled(false), retrimSupported(false) {}
ScanResult::ScanResult() : scanned(0), fragmented(0), inaccessible(0), bytesScanned(0),
    volumeClusters(0), freeClusters(0), moved(0), moveFailed(0), lastError(ERROR_SUCCESS),
    cancelled(false), ntfs(false), volumeBitmapAvailable(false) {}

std::vector<VolumeInfo> GetLocalVolumes() {
    std::vector<VolumeInfo> volumes;
    DWORD drives = GetLogicalDrives();
    for (unsigned int i = 0; i < 26; ++i) {
        if (!(drives & (1UL << i))) continue;
        wchar_t root[] = L"A:\\";
        root[0] = static_cast<wchar_t>(L'A' + i);
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        wchar_t label[MAX_PATH] = {}, fs[32] = {};
        if (!GetVolumeInformationW(root, label, MAX_PATH, NULL, NULL, NULL, fs, 32)) continue;
        VolumeInfo entry;
        entry.root = root;
        entry.label = label;
        entry.filesystem = fs;
        entry.ntfs = _wcsicmp(fs, L"NTFS") == 0;
        ULARGE_INTEGER freeAvailable = {}, total = {}, freeTotal = {};
        if (GetDiskFreeSpaceExW(root, &freeAvailable, &total, &freeTotal)) {
            entry.bytesTotal = total.QuadPart;
            entry.bytesFree = freeTotal.QuadPart;
        }
        DWORD sectors = 0, sectorSize = 0, freeClusters = 0, totalClusters = 0;
        if (GetDiskFreeSpaceW(root, &sectors, &sectorSize, &freeClusters, &totalClusters))
            entry.clusterBytes = static_cast<unsigned long long>(sectors) * sectorSize;
        // Seek-penalty query is not in the XP SDK. The numeric property 7 is documented
        // as StorageDeviceSeekPenaltyProperty on Windows 7+. Unknown => no automatic writes.
        Handle handle(CreateFileW(VolumePath(root).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, 0, NULL));
        if (handle.Valid()) {
            STORAGE_PROPERTY_QUERY query = {};
            query.QueryType = PropertyStandardQuery;
            struct DeviceFlag { DWORD Version; DWORD Size; BOOLEAN Enabled; } response = {};
            DWORD bytes = 0;
            query.PropertyId = static_cast<STORAGE_PROPERTY_ID>(7);
            if (DeviceIoControl(handle.Get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                &response, sizeof(response), &bytes, NULL) && bytes >= sizeof(response) &&
                response.Size >= sizeof(response))
                entry.media = response.Enabled ? MediaHdd : MediaSsd;
            response = DeviceFlag();
            query.PropertyId = static_cast<STORAGE_PROPERTY_ID>(8);
            if (DeviceIoControl(handle.Get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                &response, sizeof(response), &bytes, NULL) && bytes >= sizeof(response) &&
                response.Size >= sizeof(response)) {
                entry.trimKnown = true;
                entry.trimEnabled = response.Enabled != 0;
            }
        }
        unsigned int major = 0, minor = 0;
        WindowsVersion(major, minor);
        entry.retrimSupported = CanRetrim(entry.ntfs, entry.media == MediaSsd,
            entry.trimKnown, entry.trimEnabled, major, minor);
        if (entry.retrimSupported) {
            std::wstring executable;
            entry.retrimSupported = DefragExecutable(executable);
        }
        volumes.push_back(entry);
    }
    return volumes;
}

void Analyze(const std::wstring& root, Observer& observer, ScanResult& result) {
    std::vector<VolumeInfo> all = GetLocalVolumes();
    VolumeInfo info;
    bool found = false;
    for (size_t i = 0; i < all.size(); ++i)
        if (_wcsicmp(all[i].root.c_str(), root.c_str()) == 0) { info = all[i]; found = true; break; }
    if (!found || !info.ntfs) { result.lastError = ERROR_NOT_SUPPORTED; observer.Message(L"Select a local NTFS volume."); return; }
    result.ntfs = true;
    if (info.clusterBytes > 0) {
        result.volumeClusters = info.bytesTotal / info.clusterBytes;
        result.freeClusters = info.bytesFree / info.clusterBytes;
    }
    observer.Message(L"Reading NTFS allocation map (requires administrator privileges)...");
    Handle volume(CreateFileW(VolumePath(root).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL));
    if (volume.Valid()) ReadAllocationSamples(volume.Get(), result, observer);
    else observer.Message(L"Allocation map unavailable; file analysis will continue.");
    if (observer.Cancelled()) { result.cancelled = true; return; }
    observer.Message(L"Enumerating files and NTFS retrieval pointers...");
    unsigned long long notify = 0;
    ScanDirectory(root, observer, result, notify);
    std::sort(result.files.begin(), result.files.end(), FileScoreDescending);
    observer.Progress(100);
    observer.Message(result.cancelled ? L"Analysis cancelled." : L"Analysis completed.");
}

void Optimize(const std::wstring& root, Observer& observer, ScanResult& result) {
    std::vector<VolumeInfo> volumes = GetLocalVolumes();
    bool eligible = false;
    for (size_t i = 0; i < volumes.size(); ++i)
        if (_wcsicmp(volumes[i].root.c_str(), root.c_str()) == 0) {
            eligible = CanRelocate(volumes[i].ntfs, volumes[i].media == MediaHdd, IsSystemDrive(root));
            break;
        }
    if (!eligible) {
        result.lastError = ERROR_NOT_SUPPORTED;
        observer.Message(L"Relocation requires a positively identified NTFS HDD that is not the system volume.");
        return;
    }
    Handle volume(CreateFileW(VolumePath(root).c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL));
    if (!volume.Valid()) { result.lastError = GetLastError(); observer.Message(L"Administrator privileges required for file relocation."); return; }
    const PlanLimits limits = DefaultLimits();
    unsigned int attempts = 0;
    for (size_t i = 0; i < result.files.size() && attempts < limits.maximumMoves; ++i) {
        if (observer.Cancelled()) { result.cancelled = true; break; }
        const FileRecord& record = result.files[i];
        Candidate candidate = {};
        candidate.fileBytes = record.bytes;
        candidate.clusterCount = record.clusters;
        candidate.extents = record.extents;
        candidate.sparseOrCompressed = (record.attributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_ENCRYPTED)) != 0;
        candidate.systemFile = UnsafeFile(record.path, record.attributes);
        if (!IsEligible(candidate, limits)) continue;
        ++attempts;
        // Deny write sharing while relocating; files already open for writes are skipped.
        Handle file(CreateFileW(record.path.c_str(), GENERIC_READ,
            FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL));
        if (!file.Valid()) { ++result.moveFailed; continue; }
        BY_HANDLE_FILE_INFORMATION before = {};
        if (!GetFileInformationByHandle(file.Get(), &before) ||
            (before.dwFileAttributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE |
              FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_SYSTEM))) {
            ++result.moveFailed; continue;
        }
        std::vector<Extent> live;
        if (!ReadExtents(file.Get(), live) || live.size() < 2) continue;
        unsigned long long clusters = 0;
        for (size_t e = 0; e < live.size(); ++e) clusters += live[e].count;
        if (clusters != record.clusters) { ++result.moveFailed; continue; }
        // Only process fully allocated primary streams with VCNs starting at zero.
        unsigned long long expectedVcn = 0;
        for (size_t e = 0; e < live.size(); ++e) {
            if (live[e].vcn != expectedVcn) break;
            expectedVcn += live[e].count;
        }
        if (expectedVcn != clusters) { ++result.moveFailed; continue; }
        unsigned long long destination = 0;
        if (!FindFreeRun(volume.Get(), clusters, destination, observer)) {
            if (observer.Cancelled()) result.cancelled = true;
            else observer.Message(L"No suitable contiguous free region; skipping remaining candidates.");
            break;
        }
        // FSCTL_MOVE_FILE handles stale allocation and performs filesystem-managed moves.
        // A failure may leave prior successful chunks relocated; reanalysis is essential.
        bool success = true;
        unsigned long long movedClusters = 0;
        while (movedClusters < clusters && !observer.Cancelled()) {
            MOVE_FILE_DATA move = {};
            move.FileHandle = file.Get();
            move.StartingVcn.QuadPart = static_cast<LONGLONG>(movedClusters);
            move.StartingLcn.QuadPart = static_cast<LONGLONG>(destination + movedClusters);
            unsigned long long remaining = clusters - movedClusters;
            move.ClusterCount = static_cast<DWORD>(remaining > 1024 ? 1024 : remaining);
            DWORD returned = 0;
            if (!DeviceIoControl(volume.Get(), FSCTL_MOVE_FILE, &move, sizeof(move), NULL, 0, &returned, NULL)) {
                result.lastError = GetLastError(); success = false; break;
            }
            movedClusters += move.ClusterCount;
        }
        if (observer.Cancelled()) { result.cancelled = true; break; }
        if (success) ++result.moved;
        else ++result.moveFailed;
        wchar_t message[200];
        _snwprintf(message, 199, L"Relocation: %u completed; %u failed (attempt %u of %u)",
            result.moved, result.moveFailed, attempts, limits.maximumMoves);
        message[199] = 0;
        observer.Message(message);
        observer.Progress(attempts * 100 / limits.maximumMoves);
    }
    observer.Message(result.cancelled ? L"Relocation cancelled; verify and re-analyze." :
        L"Relocation pass ended. Re-analyze before further optimization.");
}

void Retrim(const std::wstring& root, Observer& observer, ScanResult& result) {
    std::vector<VolumeInfo> volumes = GetLocalVolumes();
    bool allowed = false;
    for (size_t i = 0; i < volumes.size(); ++i) {
        if (_wcsicmp(volumes[i].root.c_str(), root.c_str()) == 0) {
            allowed = volumes[i].retrimSupported;
            break;
        }
    }
    if (!allowed) {
        result.lastError = ERROR_NOT_SUPPORTED;
        observer.Message(L"ReTrim requires Windows 8+, a confirmed SSD, and enabled TRIM support.");
        return;
    }
    if (observer.Cancelled()) { result.cancelled = true; result.lastError = ERROR_CANCELLED; return; }
    RunRetrimProcess(root, observer, result);
}

std::wstring FormatWindowsError(DWORD code) {
    LPWSTR buffer = NULL;
    DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, NULL);
    std::wstring result = size && buffer ? buffer : L"Unknown Windows error";
    if (buffer) LocalFree(buffer);
    return result;
}

std::wstring FormatSize(unsigned long long bytes) {
    const wchar_t* units[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    double value = static_cast<double>(bytes);
    unsigned int unit = 0;
    while (value >= 1024 && unit < 4) { value /= 1024; ++unit; }
    wchar_t output[80];
    _snwprintf(output, 79, L"%.1f %s", value, units[unit]);
    output[79] = 0;
    return output;
}

bool WriteCsvReport(const std::wstring& file, const ScanResult& result) {
    HANDLE output = CreateFileW(file.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE) return false;
    const char bom[] = "\xEF\xBB\xBF";
    DWORD written = 0;
    WriteFile(output, bom, sizeof(bom) - 1, &written, NULL);
    const char* header = "File,Bytes,Clusters,Extents,Priority\r\n";
    WriteFile(output, header, static_cast<DWORD>(strlen(header)), &written, NULL);
    for (size_t i = 0; i < result.files.size(); ++i) {
        const FileRecord& item = result.files[i];
        std::wstring escaped = item.path;
        for (size_t j = 0; j < escaped.size(); ++j) if (escaped[j] == L'"') escaped.insert(j++, 1, L'"');
        wchar_t numbers[160];
        _snwprintf(numbers, 159, L"\",%I64u,%I64u,%u,%.3f\r\n", item.bytes, item.clusters, item.extents, item.score);
        numbers[159] = 0;
        std::wstring line = L"\"" + escaped + numbers;
        int needed = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), NULL, 0, NULL, NULL);
        if (needed <= 0) { CloseHandle(output); return false; }
        std::vector<char> utf8(needed);
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), &utf8[0], needed, NULL, NULL);
        if (!WriteFile(output, &utf8[0], static_cast<DWORD>(utf8.size()), &written, NULL) || written != utf8.size()) {
            CloseHandle(output); return false;
        }
    }
    return CloseHandle(output) != 0;
}

}
