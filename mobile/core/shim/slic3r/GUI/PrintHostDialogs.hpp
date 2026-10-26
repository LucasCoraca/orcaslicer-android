// Stand-in for slic3r/GUI/PrintHostDialogs.hpp: PrintHostQueueDialog as the receiver of PrintHostJobQueue's events,
// forwarded by the core to the app's upload queue (mobile/core/src/Device.cpp).
#pragma once
#include "wx/event.h"
#include "wx/string.h"

namespace Slic3r {
struct PrintHostJob;
namespace GUI {

extern const wxEventType EVT_PRINTHOST_PROGRESS, EVT_PRINTHOST_ERROR, EVT_PRINTHOST_CANCEL, EVT_PRINTHOST_INFO;

class PrintHostQueueDialog : public wxEvtHandler
{
public:
    class Event : public wxEvent
    {
    public:
        size_t   job_id;
        int      progress = 0; // in percent
        wxString tag;
        wxString status;

        Event(wxEventType eventType, int winid, size_t job_id) : wxEvent(winid, eventType), job_id(job_id) {}
        Event(wxEventType eventType, int winid, size_t job_id, int progress) : wxEvent(winid, eventType), job_id(job_id), progress(progress) {}
        Event(wxEventType eventType, int winid, size_t job_id, wxString error) : wxEvent(winid, eventType), job_id(job_id), status(std::move(error)) {}
        Event(wxEventType eventType, int winid, size_t job_id, wxString tag, wxString status)
            : wxEvent(winid, eventType), job_id(job_id), tag(std::move(tag)), status(std::move(status)) {}
        wxEvent *Clone() const override { return new Event(*this); }
    };

    void append_job(const PrintHostJob &job);
    void ProcessEvent(wxEvent &event) override;
};

}} // namespace Slic3r::GUI
