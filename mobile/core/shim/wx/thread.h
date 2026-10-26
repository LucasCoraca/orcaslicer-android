// Stand-in for wx/thread.h: the "UI thread" of upstream GUI logic is the core's engine thread (Core::call_after)
#pragma once

bool wxIsMainThread();
