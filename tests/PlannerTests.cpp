#include "Planner.h"
#include "SsdPolicy.h"
#include <stdio.h>

#define VERIFY(condition) do { if (!(condition)) { \
    printf("FAILED: %s at line %d\n", #condition, __LINE__); return 1; \
} } while (0)

int main() {
    nd::PlanLimits limits = nd::DefaultLimits();
    nd::Candidate file = {};
    file.fileBytes = 12ULL * 1024 * 1024;
    file.clusterCount = 3072;
    file.extents = 12;
    VERIFY(nd::IsEligible(file, limits));
    VERIFY(nd::Score(file) > 0.0);
    VERIFY(nd::CanFit(3072, file.clusterCount));
    VERIFY(!nd::CanFit(3000, file.clusterCount));
    VERIFY(!nd::CanFit(0, 0));
    file.extents = 1;
    VERIFY(!nd::IsEligible(file, limits));
    VERIFY(nd::Score(file) == 0.0);
    file.extents = 12;
    file.sparseOrCompressed = true;
    VERIFY(!nd::IsEligible(file, limits));
    file.sparseOrCompressed = false;
    file.fileBytes = 128ULL * 1024 * 1024;
    VERIFY(!nd::IsEligible(file, limits));
    file.fileBytes = 12ULL * 1024 * 1024;
    file.systemFile = true;
    VERIFY(!nd::IsEligible(file, limits));
    // SSD maintenance requires independent positive signals and Windows 8+.
    VERIFY(nd::CanRetrim(true, true, true, true, 6, 2));
    VERIFY(nd::CanRetrim(true, true, true, true, 10, 0));
    VERIFY(!nd::CanRetrim(true, true, true, true, 6, 1));
    VERIFY(!nd::CanRetrim(true, false, true, true, 10, 0));
    VERIFY(!nd::CanRetrim(true, true, false, true, 10, 0));
    VERIFY(!nd::CanRetrim(true, true, true, false, 10, 0));
    VERIFY(!nd::CanRetrim(false, true, true, true, 10, 0));
    VERIFY(nd::CanRelocate(true, true, false));
    VERIFY(!nd::CanRelocate(true, false, false));
    VERIFY(!nd::CanRelocate(true, true, true));
    puts("Planner and SSD safety policy tests passed");
    return 0;
}
