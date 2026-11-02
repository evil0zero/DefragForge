#include "SsdPolicy.h"

namespace nd {

bool CanRetrim(bool ntfs, bool confirmedSsd, bool trimKnown,
    bool trimEnabled, unsigned int osMajor, unsigned int osMinor) {
    const bool supportedWindows = osMajor > 6 || (osMajor == 6 && osMinor >= 2);
    return ntfs && confirmedSsd && trimKnown && trimEnabled && supportedWindows;
}

bool CanRelocate(bool ntfs, bool confirmedHdd, bool systemVolume) {
    return ntfs && confirmedHdd && !systemVolume;
}

bool CanNativeHddOptimize(bool ntfs, bool confirmedHdd,
    unsigned int osMajor, unsigned int osMinor) {
    const bool supportedWindows = osMajor > 6 || (osMajor == 6 && osMinor >= 2);
    return ntfs && confirmedHdd && supportedWindows;
}

}
