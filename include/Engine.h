#ifndef DEFRAGFORGE_ENGINE_H
#define DEFRAGFORGE_ENGINE_H

#include "BuildTarget.h"
#include "Planner.h"
#include "SsdPolicy.h"
#include <windows.h>
#include <string>
#include <vector>

namespace nd {

enum MediaKind { MediaUnknown, MediaHdd, MediaSsd };

struct FileRecord {
    std::wstring path;
    unsigned long long bytes;
    unsigned long long clusters;
    unsigned int extents;
    double score;
    DWORD attributes;
};

struct VolumeInfo {
    std::wstring root;
    std::wstring label;
    std::wstring filesystem;
    unsigned long long bytesTotal;
    unsigned long long bytesFree;
    unsigned long long clusterBytes;
    MediaKind media;
    bool ntfs;
    bool trimKnown;
    bool trimEnabled;
    bool retrimSupported;
    VolumeInfo();
};

struct ScanResult {
    std::vector<FileRecord> files;
    std::vector<unsigned char> allocationSamples;
    unsigned long long scanned;
    unsigned long long fragmented;
    unsigned long long inaccessible;
    unsigned long long bytesScanned;
    unsigned long long volumeClusters;
    unsigned long long freeClusters;
    unsigned int moved;
    unsigned int moveFailed;
    DWORD lastError;
    bool cancelled;
    bool ntfs;
    bool volumeBitmapAvailable;
    ScanResult();
};

class Observer {
public:
    virtual ~Observer() {}
    virtual bool Cancelled() const = 0;
    virtual void Message(const std::wstring& message) = 0;
    virtual void Progress(unsigned int percent) = 0;
};

std::vector<VolumeInfo> GetLocalVolumes();
void Analyze(const std::wstring& root, Observer& observer, ScanResult& result);
void Optimize(const std::wstring& root, Observer& observer, ScanResult& result);
void Retrim(const std::wstring& root, Observer& observer, ScanResult& result);
std::wstring FormatWindowsError(DWORD code);
std::wstring FormatSize(unsigned long long bytes);
bool WriteCsvReport(const std::wstring& file, const ScanResult& result);

}
#endif
