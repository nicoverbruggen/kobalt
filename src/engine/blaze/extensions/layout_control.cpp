// SPDX-License-Identifier: GPL-2.0-or-later
#include "koblaze.h"
#include <cstddef>
#include <exception>

static KoblazeCancelCheck koblaze_cancel_check = NULL;
static void * koblaze_cancel_context = NULL;
static int koblaze_no_cancel = 0;
static int koblaze_parser_can_stop = 0;

KoblazeParserCanStop::KoblazeParserCanStop() { koblaze_parser_can_stop++; }
KoblazeParserCanStop::~KoblazeParserCanStop() { koblaze_parser_can_stop--; }

void koblazeCheckCancelInParser()
{
    if ( koblaze_parser_can_stop > 0 )
        koblazeCheckCancel();
}

KoblazeNoCancel::KoblazeNoCancel() { koblaze_no_cancel++; }
KoblazeNoCancel::~KoblazeNoCancel() { koblaze_no_cancel--; }

void koblazeSwapCancelScopes(int * noCancel, int * parserCanStop)
{
    int value = koblaze_no_cancel;
    koblaze_no_cancel = *noCancel;
    *noCancel = value;
    value = koblaze_parser_can_stop;
    koblaze_parser_can_stop = *parserCanStop;
    *parserCanStop = value;
}

void koblazeSetCancelCheck(KoblazeCancelCheck check, void * context)
{
    koblaze_cancel_check = check;
    koblaze_cancel_context = context;
}

void koblazeCheckCancel()
{
    // A destructor that runs while a stop unwinds the stack can reach a
    // check again. A second exception there would end the program.
    if ( !koblaze_cancel_check || koblaze_no_cancel > 0 || std::uncaught_exception() )
        return;
    if ( koblaze_cancel_check(koblaze_cancel_context) )
        throw KoblazeRenderCancelled();
}

static KoblazePausePoint koblaze_pause_point = NULL;
static void * koblaze_pause_context = NULL;

void koblazeSetPausePoint(KoblazePausePoint pause, void * context)
{
    koblaze_pause_point = pause;
    koblaze_pause_context = context;
}

void koblazePausePoint()
{
    if ( koblaze_pause_point )
        koblaze_pause_point(koblaze_pause_context);
}
