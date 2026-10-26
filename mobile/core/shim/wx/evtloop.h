// Stand-in for wx/evtloop.h: a nested loop on the engine thread runs the queued calls until Exit() (PluginsUi.cpp)
#pragma once

class wxEventLoop
{
public:
    int  Run();
    void Exit(int = 0) { m_running = false; }
    bool IsRunning() const { return m_running; }

private:
    bool m_running{false};
};
