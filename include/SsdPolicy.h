#ifndef DEFRAGFORGE_SSD_POLICY_H
#define DEFRAGFORGE_SSD_POLICY_H

namespace nd {

// Keep policy independent of Win32 so safety checks can be unit-tested.
bool CanRetrim(bool ntfs, bool confirmedSsd, bool trimKnown,
    bool trimEnabled, unsigned int osMajor, unsigned int osMinor);
bool CanRelocate(bool ntfs, bool confirmedHdd, bool systemVolume);
bool CanNativeHddOptimize(bool ntfs, bool confirmedHdd,
    unsigned int osMajor, unsigned int osMinor);

}
#endif
