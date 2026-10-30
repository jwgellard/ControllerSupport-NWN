#include "quickbar.h"

namespace quickbar {

int SlotIndex(int fkey, bool shift, bool ctrl, int shownPage)
{
    if (fkey < 1 || fkey > kSlotsPerPage)
        return -1;
    int page = shift ? 1 : ctrl ? 2 : shownPage >= 0 && shownPage < kPages ? shownPage : 0;
    return page * kSlotsPerPage + fkey - 1;
}

} // namespace quickbar
