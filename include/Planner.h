#ifndef DEFRAGFORGE_PLANNER_H
#define DEFRAGFORGE_PLANNER_H
#include <stddef.h>
namespace nd {
struct Candidate {
    unsigned long long fileBytes;
    unsigned long long clusterCount;
    unsigned int extents;
    bool sparseOrCompressed;
    bool systemFile;
};
struct PlanLimits {
    unsigned long long maximumFileBytes;
    unsigned int maximumExtents;
    unsigned int maximumMoves;
};
PlanLimits DefaultLimits();
bool IsEligible(const Candidate& candidate, const PlanLimits& limits);
double Score(const Candidate& candidate);
bool CanFit(unsigned long long freeRunClusters, unsigned long long fileClusters);
}
#endif
