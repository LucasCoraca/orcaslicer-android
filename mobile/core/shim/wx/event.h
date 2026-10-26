// Stand-in for wx/event.h: events posted by upstream logic compiled into liborcacore are handled at once by the
// receiver's ProcessEvent (the core forwards them to the app as host events).
#pragma once
#include <functional>
#include <memory>
#include "wx/string.h"

typedef int wxEventType;
wxEventType wxNewEventType();

class wxEvent
{
public:
    wxEvent(int winid = 0, wxEventType type = 0) : m_type(type), m_id(winid) {}
    virtual ~wxEvent() = default;
    virtual wxEvent *Clone() const = 0;
    wxEventType GetEventType() const { return m_type; }
    int  GetId() const { return m_id; }
    void SetEventType(wxEventType t) { m_type = t; }
    void Skip(bool = true) {}

protected:
    wxEventType m_type;
    int         m_id;
};

class wxEvtHandler
{
public:
    virtual ~wxEvtHandler() = default;
    virtual void ProcessEvent(wxEvent &) {}
    int GetId() const { return 0; }
    // Bind(wxEVT_TIMER, &C::on_timer, this): what a wxTimer owned by this handler runs (ProcessRunner)
    template<class E, class C> void Bind(int, void (C::*method)(E &), C *obj) { m_timer_handler = [method, obj]() { E e; (obj->*method)(e); }; }
    void timer_fired() { if (m_timer_handler) m_timer_handler(); }

private:
    std::function<void()> m_timer_handler;
};

inline void wxQueueEvent(wxEvtHandler *dest, wxEvent *event)
{
    std::unique_ptr<wxEvent> e(event);
    if (dest) dest->ProcessEvent(*e);
}

// wxKeyEvent: a hardware key press from the app (KeyChord::from_event)
#include "wx/defs.h"
enum { wxEVT_KEY_DOWN = 10100, wxEVT_CHAR = 10101 };
class wxKeyEvent
{
public:
    wxKeyEvent(wxEventType type = wxEVT_KEY_DOWN, int key = 0, int modifiers = 0) : m_type(type), m_key(key), m_modifiers(modifiers) {}
    wxEventType GetEventType() const { return m_type; }
    int  GetKeyCode() const { return m_key; }
    int  GetModifiers() const { return m_modifiers; }
    bool ControlDown() const { return (m_modifiers & wxMOD_CONTROL) != 0; }
    bool ShiftDown() const { return (m_modifiers & wxMOD_SHIFT) != 0; }
    bool AltDown() const { return (m_modifiers & wxMOD_ALT) != 0; }
    bool HasAnyModifiers() const { return m_modifiers != 0; }
    void Skip(bool = true) {}

private:
    wxEventType m_type;
    int         m_key, m_modifiers;
};
