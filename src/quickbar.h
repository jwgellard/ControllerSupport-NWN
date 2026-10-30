// Quickbar slot mapping for the overlay. Pure logic, so the offline harness can check
// it; reading the slots themselves is game::QuickbarSlotName, and names are shown
// through text::Display.
#pragma once

namespace quickbar {

constexpr int kSlotsPerPage = 12, kPages = 3;

// Which of the 36 slots F<fkey> (1-12) fires: Shift picks the second page, Ctrl the
// third, a bare key the page the bar shows now (shownPage; the first when unknown).
int SlotIndex(int fkey, bool shift, bool ctrl, int shownPage);

} // namespace quickbar
