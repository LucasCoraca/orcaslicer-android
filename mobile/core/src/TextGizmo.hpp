// The Text gizmo (GLGizmoEmboss): see TextGizmo.cpp
#pragma once
#include "Core.hpp"
#include "Viewport.hpp"

namespace orca {
bool text_gizmo_active();
void text_gizmo_close();
// the surface drag of the edited text volume
bool text_gizmo_drag_begin(const Camera &cam, double px, double py);
void text_gizmo_drag_move(const Camera &cam, double px, double py);
void text_gizmo_drag_end();
// m_rotate_gizmo: the ring around the volume's Z (frame / radius under the core lock) and its dragged rotation
bool text_gizmo_ring(Slic3r::Transform3d &frame, double &radius);
void text_gizmo_ring_rotate(double delta);
} // namespace orca
