#include "Analytics.h"
#include <algorithm>
#include <cwctype>
#include <map>

namespace nd {

std::wstring ParentFolder(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"(unknown)";
    if (slash == 2 && path.size() > 2 && path[1] == L':') return path.substr(0, 3);
    return path.substr(0, slash);
}

FragmentTotals SummarizeFragments(const std::vector<FragmentRecord>& records) {
    FragmentTotals totals;
    for (size_t index = 0; index < records.size(); ++index) {
        const FragmentRecord& record = records[index];
        if (record.extents <= 1) continue;
        ++totals.files;
        totals.bytes += record.bytes;
        totals.excessExtents += static_cast<unsigned long long>(record.extents - 1);
    }
    return totals;
}

namespace {
std::wstring Normalized(const std::wstring& source) {
    std::wstring result(source);
    for (size_t i = 0; i < result.size(); ++i) result[i] = static_cast<wchar_t>(towlower(result[i]));
    return result;
}

bool MoreFragmented(const FolderStat& left, const FolderStat& right) {
    if (left.excessExtents != right.excessExtents)
        return left.excessExtents > right.excessExtents;
    if (left.fragmentedFiles != right.fragmentedFiles)
        return left.fragmentedFiles > right.fragmentedFiles;
    return left.path < right.path;
}
}

std::vector<FolderStat> AggregateFolders(const std::vector<FragmentRecord>& records) {
    std::vector<FolderStat> summaries;
    std::map<std::wstring, size_t> folderIndexes;
    for (size_t index = 0; index < records.size(); ++index) {
        const FragmentRecord& record = records[index];
        if (record.extents <= 1) continue;
        const std::wstring folder = ParentFolder(record.path);
        const std::wstring key = Normalized(folder);
        std::map<std::wstring, size_t>::iterator found = folderIndexes.find(key);
        size_t summaryIndex;
        if (found == folderIndexes.end()) {
            summaryIndex = summaries.size();
            folderIndexes[key] = summaryIndex;
            FolderStat stat;
            stat.path = folder;
            summaries.push_back(stat);
        } else summaryIndex = found->second;
        FolderStat& stat = summaries[summaryIndex];
        stat.bytes += record.bytes;
        stat.excessExtents += static_cast<unsigned long long>(record.extents - 1);
        ++stat.fragmentedFiles;
    }
    std::sort(summaries.begin(), summaries.end(), MoreFragmented);
    return summaries;
}

}
