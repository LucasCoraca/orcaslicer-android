// Stand-in for wx/app.h: included by upstream files compiled into liborcacore that use none of it.
#pragma once
#include "wx/string.h"


// wxApp::GetInstance(): the GUI_App stand-in (Core.cpp)
#include <functional>
void wx_call_after(std::function<void()> fn);   // Core::call_after
class wxApp
{
public:
    virtual ~wxApp() = default;
    static wxApp *GetInstance();
    template<class F> void CallAfter(F &&fn) { wx_call_after(std::function<void()>(std::forward<F>(fn))); }
};
// wxTheApp: the app object exists (upstream checks it before reaching wxStandardPaths)
extern wxApp *wxTheApp;
