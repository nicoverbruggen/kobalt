// SPDX-License-Identifier: GPL-2.0-or-later
#include "koblaze.h"

static int koblaze_first_spine_item = -1;
static int koblaze_last_spine_item = -1;

void koblazeSetSpineWindow(int first, int last)
{
    koblaze_first_spine_item = first;
    koblaze_last_spine_item = last;
}

bool koblazeSpineWindowSet()
{
    return koblaze_first_spine_item >= 0;
}

bool koblazeSpineItemLoaded(int index)
{
    return koblaze_first_spine_item < 0
            || (index >= koblaze_first_spine_item && index <= koblaze_last_spine_item);
}
