// Stand-in for wx/timer.h: Notify() runs on the engine thread every interval until Stop() (Plugins.cpp)
#pragma once
#include <functional>
#include <memory>
#include "wx/event.h"
#include "wx/gdicmn.h"   // wxID_ANY

class wxTimerEvent {};
enum { wxEVT_TIMER = 10200 };

class wxTimer
{
public:
    wxTimer() = default;
    // a timer owned by a handler: its Bind(wxEVT_TIMER, ...) runs
    explicit wxTimer(wxEvtHandler *owner, int = -1) : m_owner(owner) {}
    virtual ~wxTimer();
    virtual void Notify() {}
    bool         Start(int milliseconds = -1, bool one_shot = false);
    void         Stop();
    bool         IsRunning() const;
    // Bind(wxEVT_TIMER, handler): the handler runs instead of Notify()
    template<class F> void Bind(int, F &&handler) { m_handler = [h = std::forward<F>(handler)]() mutable { wxTimerEvent e; h(e); }; }
    void         fire() { if (m_handler) m_handler(); else if (m_owner) m_owner->timer_fired(); else Notify(); }

private:
    struct State;
    std::shared_ptr<State> m_state;
    std::function<void()>  m_handler;
    wxEvtHandler          *m_owner{nullptr};
};
