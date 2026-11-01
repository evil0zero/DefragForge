#include "Planner.h"

namespace nd {

PlanLimits DefaultLimits() {
    PlanLimits limits;
    limits.maximumFileBytes = 64ULL * 1024ULL * 1024ULL;
    limits.maximumExtents = 4096;
    limits.maximumMoves = 16;
    return limits;
}

bool IsEligible(const Candidate& candidate, const PlanLimits& limits) {
    return candidate.extents > 1 &&
        candidate.extents <= limits.maximumExtents &&
        candidate.fileBytes > 0 &&
        candidate.fileBytes <= limits.maximumFileBytes &&
        candidate.clusterCount > 0 &&
        !candidate.sparseOrCompressed && !candidate.systemFile;
}

double Score(const Candidate& candidate) {
    if (candidate.extents <= 1 || candidate.fileBytes == 0) {
        return 0.0;
    }
    const double fragmentation = static_cast<double>(candidate.extents - 1);
    const double megabytes = static_cast<double>(candidate.fileBytes) / (1024.0 * 1024.0);
    return fragmentation / (1.0 + megabytes / 32.0);
}

bool CanFit(unsigned long long freeRunClusters, unsigned long long fileClusters) {
    return fileClusters != 0 && fileClusters <= freeRunClusters;
}

}
