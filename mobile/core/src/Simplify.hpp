// The Simplify gizmo (GLGizmoSimplify): its_quadric_edge_collapse of the selected part on a worker thread, the result
// drawn in place of the part (with an optional wireframe) and applied to the model on "Apply".
#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <Eigen/Geometry>
#include <nlohmann/json.hpp>

#include "libslic3r/TriangleMesh.hpp"

namespace orca {

class Simplify
{
public:
    static Simplify &get();

    void           open();    // gizmo turned on: the selected part (GLGizmoSimplify::on_render_input_window)
    void           close();   // on_set_state(Off)
    void           set(const nlohmann::json &a);
    nlohmann::json state();
    nlohmann::json apply();   // apply_simplify
    bool           hides(int obj, int inst, int vol);   // the part whose preview replaces it
    void           render(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj);   // GL thread

private:
    struct Configuration
    {
        bool     use_count      = false;
        float    decimate_ratio = 50.f;   // in percent
        uint32_t wanted_count   = 0;      // initialize by percents
        float    max_error      = 1.;     // maximal quadric error
        void fix_count_by_ratio(size_t triangle_count);
        bool operator==(const Configuration &rhs) const
        {
            return use_count == rhs.use_count && decimate_ratio == rhs.decimate_ratio && wanted_count == rhs.wanted_count && max_error == rhs.max_error;
        }
        bool operator!=(const Configuration &rhs) const { return !(*this == rhs); }
    };
    enum class Status { idle, running, cancelling };

    void process();            // (re)starts the worker when the configuration changed
    void stop_worker();
    void worker_finished();

    std::mutex      m_mutex;
    int             m_obj{-1}, m_inst{-1}, m_vol{-1};
    const void     *m_volume{nullptr};   // the ModelVolume being simplified (selection changes are noticed)
    Configuration   m_configuration;
    int             m_reduction{2};      // the detail level slider (static in GLGizmoSimplify)
    bool            m_show_wireframe{false};
    size_t          m_triangle_count{0}; // of the preview
    // worker state (GLGizmoSimplify::State)
    Status                                  m_status{Status::idle};
    int                                     m_progress{0};
    Configuration                           m_state_config;
    const void                             *m_state_mv{nullptr};
    std::unique_ptr<indexed_triangle_set>   m_result;
    std::thread                             m_worker;
    bool                                    m_finished{false};   // the worker ended; worker_finished() not run yet
    std::shared_ptr<const Slic3r::TriangleMesh> m_preview;
    Slic3r::Transform3d                     m_matrix;
    float                                   m_color[4]{0.8f, 0.8f, 0.8f, 1.f};
};

} // namespace orca
