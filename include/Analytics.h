#ifndef DEFRAGFORGE_ANALYTICS_H
#define DEFRAGFORGE_ANALYTICS_H

#include <string>
#include <vector>

namespace nd {

struct FragmentRecord {
    std::wstring path;
    unsigned long long bytes;
    unsigned int extents;
};

struct FolderStat {
    std::wstring path;
    unsigned long long bytes;
    unsigned long long excessExtents;
    unsigned long long fragmentedFiles;
    FolderStat() : bytes(0), excessExtents(0), fragmentedFiles(0) {}
};

struct FragmentTotals {
    unsigned long long bytes;
    unsigned long long files;
    unsigned long long excessExtents;
    FragmentTotals() : bytes(0), files(0), excessExtents(0) {}
};

FragmentTotals SummarizeFragments(const std::vector<FragmentRecord>& records);
std::vector<FolderStat> AggregateFolders(const std::vector<FragmentRecord>& records);
std::wstring ParentFolder(const std::wstring& path);

}

#endif
