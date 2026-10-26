// Slicing: one Print per plate, processed on a worker thread (desktop BackgroundSlicingProcess + the CLI's per plate
// loop in OrcaSlicer.cpp), results kept for the preview and for export.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"

namespace orca {

struct PlateSlice
{
    std::unique_ptr<Slic3r::Print>  print;
    Slic3r::GCodeProcessorResult    result;
    std::string                     gcode_path;   // temporary file of the last export
    bool                            valid{false};
    unsigned                        result_id{0};
};

class Slicer
{
public:
    static Slicer &get();
    // start slicing plates (-1: all); returns false when a slice is already running
    bool start(int plate);
    void cancel();
    bool running() const { return m_running; }
    PlateSlice *slice(int plate);
    void invalidate_all();
    std::mutex result_mutex;   // guards PlateSlice::result against the preview

private:
    void run(std::vector<int> plates);
    std::vector<std::unique_ptr<PlateSlice>> m_slices;
    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cancel{false};
    Slic3r::Print    *m_current{nullptr};
};

// Preferences "Auto slice after changes": a slice of the current plate after a settings change while the preview shows
void schedule_auto_reslice_if_needed();

} // namespace orca
