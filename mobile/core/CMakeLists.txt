# liborcacore.so: OrcaSlicer's engine (libslic3r + libvgcode) behind a JNI interface, for the native mobile app (mobile/app).
# Added from Orca's top-level CMakeLists.txt when ORCA_MOBILE_CORE is ON (an engine build: SLIC3R_GUI=OFF, NDK toolchain).
# Upstream sources are used unchanged; where an upstream GUI file holds logic the app needs (ConfigManipulation), it is
# compiled here against small stand-ins for the wx GUI headers it includes (shim/).
set(ORCA_SRC "${CMAKE_SOURCE_DIR}")

if (NOT TARGET glad)
    add_subdirectory("${ORCA_SRC}/src/glad-gles" "${CMAKE_BINARY_DIR}/mobile-core/glad")
endif ()
if (NOT TARGET libvgcode)
    add_subdirectory("${ORCA_SRC}/src/libvgcode" "${CMAKE_BINARY_DIR}/mobile-core/libvgcode")
endif ()

# Upstream GUI sources compiled against the shims: copied next to the shim headers, so that their sibling
# includes ("GUI_App.hpp", ...) resolve to the stand-ins.
set(_gen "${CMAKE_CURRENT_BINARY_DIR}/upstream/slic3r/GUI")
set(ORCA_UPSTREAM_GUI_FILES ConfigManipulation.cpp ConfigManipulation.hpp LibVGCode/LibVGCodeWrapper.cpp LibVGCode/LibVGCodeWrapper.hpp
    TickCode.cpp TickCode.hpp HttpServer.cpp HttpServer.hpp Shortcuts.cpp Shortcuts.hpp KeyChord.cpp KeyChord.hpp
    ConfigValueFormatter.cpp ConfigValueFormatter.hpp Widgets/WebHosting.cpp Widgets/WebHosting.hpp
    PluginsDialog.cpp PluginsDialog.hpp PluginSort.hpp PluginSource.hpp PluginStatus.hpp PluginsConfigDialog.cpp PluginsConfigDialog.hpp
    TerminalDialog.cpp TerminalDialog.hpp ProcessRunner.cpp ProcessRunner.hpp TextureLibrary.cpp TextureLibrary.hpp)
set(_upstream_sources "")
foreach (_f ${ORCA_UPSTREAM_GUI_FILES})
    configure_file("${ORCA_SRC}/src/slic3r/GUI/${_f}" "${_gen}/${_f}" COPYONLY)
    if (_f MATCHES "\\.cpp$")
        list(APPEND _upstream_sources "${_gen}/${_f}")
    endif ()
endforeach ()
# the print host clients (PrintHost::get_print_host's classes) and Http, compiled unchanged against the shims; copied
# next to each other so their sibling includes resolve to the copies
set(_utils "${CMAKE_CURRENT_BINARY_DIR}/upstream/slic3r/Utils")
set(ORCA_UPSTREAM_UTILS_FILES Http PrintHost OctoPrint Duet UltiMaker FlashAir AstroBox Repetier MKS ESP3D CrealityPrint
    Obico Flashforge ElegooLink Moonraker SimplyPrint 3DPrinterOS ASCIIFolding Bonjour TCPConsole SerialMessage SerialMessageType WebSocketClient
    CloudProvider ICloudServiceAgent ICameraSignalingChannel IPrinterAgent bambu_networking OrcaCloudServiceAgent NetworkAgent
    BBLNetworkPlugin FileTransferUtils)
foreach (_f ${ORCA_UPSTREAM_UTILS_FILES})
    foreach (_ext cpp hpp)
        if (EXISTS "${ORCA_SRC}/src/slic3r/Utils/${_f}.${_ext}")
            configure_file("${ORCA_SRC}/src/slic3r/Utils/${_f}.${_ext}" "${_utils}/${_f}.${_ext}" COPYONLY)
            if (_ext STREQUAL "cpp" AND NOT _f STREQUAL "3DPrinterOS")   # its upload options are a desktop dialog: Device.cpp
                list(APPEND _upstream_sources "${_utils}/${_f}.${_ext}")
            endif ()
        endif ()
    endforeach ()
endforeach ()
# functions copied out of upstream GUI classes (mobile/tools/extract_functions.py), regenerated when upstream changes
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(_cut_src "${ORCA_SRC}/src/slic3r/GUI/Gizmos/GLGizmoCut.cpp")
set(_cut_inc "${_gen}/GroovePlane.inc")
add_custom_command(OUTPUT "${_cut_inc}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py" "${_cut_src}" GLGizmoCut3D GroovePlane "${_cut_inc}"
            offset_indices its_make_groove_plane
    DEPENDS "${_cut_src}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py")
set(_plate_src "${ORCA_SRC}/src/slic3r/GUI/PartPlate.cpp")
set(_plate_inc "${_gen}/PlateTower.inc")
add_custom_command(OUTPUT "${_plate_inc}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py" "${_plate_src}" PartPlate PlateTower "${_plate_inc}"
            "get_extruders(bool conside_custom_gcode, const" estimate_wipe_tower_footprint estimate_wipe_tower_polygon
    DEPENDS "${_plate_src}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py"
    VERBATIM)
set(_painter_src "${ORCA_SRC}/src/slic3r/GUI/Gizmos/GLGizmoPainterBase.cpp")
set(_patch_inc "${_gen}/SelectorPatch.inc")
add_custom_command(OUTPUT "${_patch_inc}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py" "${_painter_src}" TriangleSelectorPatch SelectorPatch "${_patch_inc}"
            --sub "(m_need_wireframe && wxGetApp().plater()->is_wireframe_enabled() && wxGetApp().plater()->is_show_wireframe())" "m_need_wireframe"
            update_selector_triangles update_triangles_per_patch
    DEPENDS "${_painter_src}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py"
    VERBATIM)
set(_ears_src "${ORCA_SRC}/src/slic3r/GUI/Gizmos/GLGizmoBrimEars.cpp")
set(_ears_inc "${_gen}/BrimEars.inc")
add_custom_command(OUTPUT "${_ears_inc}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py" "${_ears_src}" GLGizmoBrimEars BrimEarsGizmo "${_ears_inc}"
            --sub "update_raycasters();" ""
            --sub "const Selection         &selection = m_parent.get_selection();" ""
            --sub "const GLVolume          *volume    = selection.get_volume(*selection.get_volume_idxs().begin());" ""
            --sub "volume->get_instance_transformation();" "instance_transformation();"
            generate_points get_detection_radius_max add_point_to_cache make_polygon find_single select_point unselect_point
    DEPENDS "${_ears_src}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py"
    VERBATIM)
set(_emboss_src "${ORCA_SRC}/src/slic3r/GUI/Jobs/EmbossJob.cpp")
set(_emboss_inc "${_gen}/EmbossJob.inc")
add_custom_command(OUTPUT "${_emboss_inc}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py" "${_emboss_src}" - - "${_emboss_inc}"
            create_shape create_line_bounds create_mesh_per_glyph try_create_mesh create_default_mesh create_projection_for_cut
            create_emboss_projection cut_surface_to_its cut_per_glyph_surface cut_surface create_sources
    DEPENDS "${_emboss_src}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py"
    VERBATIM)
set(_textlines_src "${ORCA_SRC}/src/slic3r/GUI/TextLines.cpp")
set(_textlines_inc "${_gen}/TextLines.inc")
add_custom_command(OUTPUT "${_textlines_inc}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py" "${_textlines_src}" - - "${_textlines_inc}"
            select_closest_contour
    DEPENDS "${_textlines_src}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py"
    VERBATIM)
set(_drag_src "${ORCA_SRC}/src/slic3r/GUI/SurfaceDrag.cpp")
set(_drag_inc "${_gen}/SurfaceDrag.inc")
add_custom_command(OUTPUT "${_drag_inc}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py" "${_drag_src}" - - "${_drag_inc}"
            calc_scale get_volume_transformation
    DEPENDS "${_drag_src}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py"
    VERBATIM)
set(_svg_src "${ORCA_SRC}/src/slic3r/GUI/Gizmos/GLGizmoSVG.cpp")
set(_svg_inc "${_gen}/SvgGizmo.inc")
add_custom_command(OUTPUT "${_svg_inc}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py" "${_svg_src}" - - "${_svg_inc}"
            get_tesselation_tolerance is_closed add_comma_separated create_fill_warning create_stroke_warning create_shape_warnings
            get_file_name volume_name select_shape create_emboss_data_base "union_ex(const ExPolygonsWithIds"
    DEPENDS "${_svg_src}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py"
    VERBATIM)
set(_publish_src "${ORCA_SRC}/src/slic3r/GUI/Plater.cpp")
set(_publish_inc "${_gen}/Publish.inc")
add_custom_command(OUTPUT "${_publish_inc}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py" "${_publish_src}" - - "${_publish_inc}"
            "get_file_name(const std::string" create_unique_3mf_filepath set_by_local_path publish
    DEPENDS "${_publish_src}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py"
    VERBATIM)
set(_vgexp_src "${ORCA_SRC}/src/slic3r/GUI/GCodeViewer.cpp")
set(_vgexp_inc "${_gen}/ToolpathsObjExporter.inc")
add_custom_command(OUTPUT "${_vgexp_inc}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py" "${_vgexp_src}" - - "${_vgexp_inc}"
            "class ToolpathsObjExporter"
    DEPENDS "${_vgexp_src}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py"
    VERBATIM)
set(_prefs_src "${ORCA_SRC}/src/slic3r/GUI/Preferences.cpp")
set(_prefs_inc "${_gen}/Preferences.inc")
add_custom_command(OUTPUT "${_prefs_inc}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_preferences.py" "${_prefs_src}" "${_prefs_inc}"
    DEPENDS "${_prefs_src}" "${ORCA_SRC}/src/libslic3r/AppConfig.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_preferences.py"
    VERBATIM)
set(_homestr_src "${ORCA_SRC}/src/slic3r/GUI/WebViewDialog.cpp")
set(_homestr_inc "${_gen}/HomeStrings.inc")
add_custom_command(OUTPUT "${_homestr_inc}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py" "${_homestr_src}" - - "${_homestr_inc}"
            home_page_strings
    DEPENDS "${_homestr_src}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py"
    VERBATIM)
set(_projstr_src "${ORCA_SRC}/src/slic3r/GUI/Project.cpp")
set(_projstr_inc "${_gen}/ProjectStrings.inc")
add_custom_command(OUTPUT "${_projstr_inc}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py" "${_projstr_src}" - - "${_projstr_inc}"
            project_page_strings
    DEPENDS "${_projstr_src}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py"
    VERBATIM)
set(_cpd_src "${ORCA_SRC}/src/slic3r/GUI/CreatePresetsDialog.cpp")
set(_expcfg_inc "${_gen}/ExportConfigs.inc")
set(_cpdhelp_inc "${_gen}/CreatePresetsHelpers.inc")
add_custom_command(OUTPUT "${_expcfg_inc}" "${_cpdhelp_inc}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py" "${_cpd_src}" - - "${_cpdhelp_inc}"
            get_curr_time get_curr_timestmp get_machine_name get_filament_name get_vendor_name
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py" "${_cpd_src}" ExportConfigsDialog ExportConfigs "${_expcfg_inc}"
            --sub "NetworkAgent *agent = wxGetApp().getAgent();" "struct { std::string get_version() { return {}; } std::string get_user_id() { return {}; } } *agent = nullptr;"
            --sub "EndModal(wxCANCEL);" "return;"
            earse_preset_fields_for_safe initial_file_path initial_file_name initial_zip_archive save_zip_archive_to_file save_presets_to_zip
            archive_preset_bundle_to_file archive_filament_bundle_to_file archive_printer_preset_to_file archive_filament_preset_to_file
            archive_process_preset_to_file data_init
    DEPENDS "${_cpd_src}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py"
    VERBATIM)
# Calibration: the dialogs' on_start and Plater::calib_* (+ their helpers), compiled as they are
set(_calibdlg_src "${ORCA_SRC}/src/slic3r/GUI/calib_dlg.cpp")
set(_calibdlg_inc "${_gen}/CalibDialogs.inc")
set(_calibpl_inc "${_gen}/CalibPlater.inc")
set(_calibut_inc "${_gen}/CalibUtils.inc")
set(_shaper_inc "${_gen}/ShaperTypes.inc")
set(_x "${CMAKE_CURRENT_SOURCE_DIR}/../tools/extract_functions.py")
add_custom_command(OUTPUT "${_calibdlg_inc}" "${_calibpl_inc}" "${_calibut_inc}" "${_shaper_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_calibdlg_src}" - - "${_calibdlg_inc}"
            --sub "EndModal(wxID_OK);" "closed = true;"
            ParseStringValues get_shaper_type_values is_belt_printer_selected "enum FILAMENT_TYPE"
            PA_Calibration_Dlg::on_start Temp_Calibration_Dlg::on_start MaxVolumetricSpeed_Test_Dlg::on_start VFA_Test_Dlg::on_start
            Retraction_Test_Dlg::on_start Input_Shaping_Freq_Test_Dlg::on_start Input_Shaping_Damp_Test_Dlg::on_start
            Cornering_Test_Dlg::on_start FlowRateCalibrationDialog::on_start PA_Calibration_Dlg::reset_params
            Temp_Calibration_Dlg::on_filament_type_changed
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/Plater.cpp" - - "${_calibpl_inc}"
            --sub "p->background_process.fff_print()->set_calib_params(" "set_calib_params("
            --sub "wxGetApp().mainframe->select_tab(TAB_ID_PREPARE);" "select_prepare();"
            --sub "wxGetApp().get_tab(" "get_tab("
            --sub "const auto canvas    = wxGetApp().plater()->canvas3D();" "const auto canvas = calib_canvas();"
            --sub "p->model.objects" "model().objects"
            --sub "static_cast<wxWindow *>(wxGetApp().mainframe)" "nullptr"
            set_config_values zero_pressure_advance_where_disabled has_junction_deviation belt_calib_rotation_params
            belt_calib_flip_ringing_tower belt_temp_tower_asset adjust_settings_for_flowrate_calib
            Plater::_calib_apply_belt_mode Plater::calib_pa Plater::_calib_pa_pattern Plater::_calib_pa_pattern_gen_gcode
            Plater::cut_horizontal Plater::_calib_pa_tower Plater::calib_flowrate Plater::calib_temp Plater::_calib_temp_belt_sectioned
            Plater::calib_max_vol_speed Plater::calib_retraction Plater::calib_VFA Plater::calib_input_shaping_freq
            Plater::calib_input_shaping_damp Plater::Calib_Cornering
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/Utils/CalibUtils.cpp" - - "${_calibut_inc}"
            update_speed_parameter generate_max_speed_parameter_value
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/Tab.cpp" - - "${_shaper_inc}"
            input_shaper_types_for_flavor
    DEPENDS "${_calibdlg_src}" "${ORCA_SRC}/src/slic3r/GUI/Plater.cpp" "${ORCA_SRC}/src/slic3r/Utils/CalibUtils.cpp" "${ORCA_SRC}/src/slic3r/GUI/Tab.cpp" "${_x}"
    VERBATIM)
# stb_truetype's declarations (the implementation is in libslic3r's Emboss.cpp)
configure_file("${ORCA_SRC}/deps_src/imgui/imstb_truetype.h" "${CMAKE_CURRENT_BINARY_DIR}/upstream/imgui/imstb_truetype.h" COPYONLY)
# OAuthJob's data and token parsing for SimplyPrint (the login flow itself is the app's)
set(_oauth_src "${ORCA_SRC}/src/slic3r/GUI/Jobs/OAuthJob.cpp")
set(_oauth_types_inc "${_gen}/OAuthTypes.inc")
set(_oauth_fns_inc "${_gen}/OAuthJobFns.inc")
add_custom_command(OUTPUT "${_oauth_types_inc}" "${_oauth_fns_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/Jobs/OAuthJob.hpp" - - "${_oauth_types_inc}"
            "struct OAuthParams" "struct OAuthResult" "struct OAuthData"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_oauth_src}" OAuthJob OAuthJob "${_oauth_fns_inc}" parse_token_response process
    DEPENDS "${_oauth_src}" "${ORCA_SRC}/src/slic3r/GUI/Jobs/OAuthJob.hpp" "${_x}")
# PhysicalPrinterDialog's field logic (which host options show / enable / get defaults), run on stand-in fields
set(_ppd_src "${ORCA_SRC}/src/slic3r/GUI/PhysicalPrinterDialog.cpp")
set(_ppd_inc "${_gen}/PhysicalPrinterDialog.inc")
add_custom_command(OUTPUT "${_ppd_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_ppd_src}" PhysicalPrinterDialog PhysicalPrinterDialog "${_ppd_inc}"
            update update_host_type update_printers update_ports update_webui update_printhost_buttons update_preset_input
            check_host_key_valid
    DEPENDS "${_ppd_src}" "${_x}")
# Selection's clipboard, copy and paste (Plater::copy_selection_to_clipboard / paste_from_clipboard)
set(_sel_src "${ORCA_SRC}/src/slic3r/GUI/Selection.cpp")
set(_selclip_decl_inc "${_gen}/SelectionClipboardDecl.inc")
set(_selclip_inc "${_gen}/SelectionClipboard.inc")
set(_selclipfn_inc "${_gen}/SelectionClipboardFns.inc")
add_custom_command(OUTPUT "${_selclip_decl_inc}" "${_selclip_inc}" "${_selclipfn_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/Selection.hpp" - - "${_selclip_decl_inc}" "class Clipboard"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_sel_src}" Selection::Clipboard Selection::Clipboard "${_selclipfn_inc}"
            is_sla_compliant Clipboard reset is_empty add_object get_object get_objects
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_sel_src}" Selection Selection "${_selclip_inc}"
            --sub "PartPlate *            plate       = wxGetApp().plater()->get_partplate_list().get_curr_plate();"
                  "PartPlate plate_ = curr_plate(); PartPlate *plate = &plate_;"
            --sub "wxGetApp().plater()->canvas3D()->get_nearest_empty_cell(" "nearest_empty_cell("
            --sub "wxGetApp().obj_list()->paste_objects_into_list(" "paste_objects_into_list("
            --sub "wxGetApp().obj_list()->paste_volumes_into_list(" "paste_volumes_into_list("
            copy_to_clipboard paste_from_clipboard paste_volumes_from_clipboard paste_objects_from_clipboard
    DEPENDS "${_sel_src}" "${ORCA_SRC}/src/slic3r/GUI/Selection.hpp" "${_x}"
    VERBATIM)
# GLVolumeCollection::get_selection_support_normal_z: the overhang highlight angle (ViewOptions.cpp)
set(_snz_inc "${_gen}/SupportNormalZ.inc")
add_custom_command(OUTPUT "${_snz_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/3DScene.cpp" GLVolumeCollection GLVolumeCollection "${_snz_inc}"
            get_selection_support_normal_z
    DEPENDS "${ORCA_SRC}/src/slic3r/GUI/3DScene.cpp" "${_x}")
# the hint database of "Daily Tips" (HintNotification.cpp), its links routed to the app (Tips.cpp)
set(_hint_src "${ORCA_SRC}/src/slic3r/GUI/HintNotification.cpp")
set(_hint_types_inc "${_gen}/HintTypes.inc")
set(_hint_fns_inc "${_gen}/HintHelpers.inc")
set(_hint_db_inc "${_gen}/HintDatabase.inc")
set(_hint_cereal_inc "${_gen}/HintHelpersCereal.inc")
add_custom_command(OUTPUT "${_hint_types_inc}" "${_hint_fns_inc}" "${_hint_db_inc}" "${_hint_cereal_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_hint_src}" - - "${_hint_cereal_inc}" "struct HintsCerealData"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/HintNotification.hpp" - - "${_hint_types_inc}"
            "struct HintData" "enum class HintDataNavigation" "class HintDatabase"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_hint_src}" - - "${_hint_fns_inc}"
            --sub "wxGetApp().get_mode()" "hint_mode()"
            --sub "wxGetApp().extruders_edited_cnt()" "hint_extruders()"
            --sub "const GUI::Tab* tab = wxGetApp().get_tab(Preset::Type::TYPE_FILAMENT)" "const HintTab* tab = hint_filament_tab()"
            --sub "wxGetApp().open_browser_with_warning_dialog(url);" "hint_action({{\"url\", url}});"
            write_used_binary read_used_binary "enum TagCheckResult" tag_check_mode tag_check_tech
            tag_check_system tag_check_material tags_check launch_browser_if_allowed
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_hint_src}" HintDatabase HintDatabase "${_hint_db_inc}"
            --sub "GUI::wxGetApp().sidebar().jump_to_option(opt, type, category);"
                  "hint_action({{\"settings\", opt}, {\"type\", int(type)}, {\"category\", boost::nowide::narrow(category)}});"
            --sub "wxGetApp().open_preferences(PreferencesTab::Control, item);" "hint_action({{\"preferences\", item}});"
            --sub "wxGetApp().plater()->canvas3D()->highlight_toolbar_item(item);" "hint_action({{\"plater\", item}});"
            --sub "wxGetApp().plater()->canvas3D()->highlight_gizmo(item);" "hint_action({{\"gizmo\", item}});"
            --sub "wxGetApp().plater()->canvas3D()->deselect_all();" "hint_action({{\"gallery\", true}});"
            --sub "wxGetApp().mainframe->open_menubar_item(menu, item);"
                  "hint_action({{\"menubar\", std::string(menu)}, {\"item\", std::string(item)}});"
            ~HintDatabase uninit reinit init init_random_hint_id load_hints_from_file get_hint get_next_hint_id
            get_prev_hint_id get_random_next is_used set_used clear_used
    DEPENDS "${_hint_src}" "${ORCA_SRC}/src/slic3r/GUI/HintNotification.hpp" "${_x}"
    VERBATIM)
# the About dialog's build label (BuildCommit.cpp with its generated git_commit_hash.h) and library list
set(_git_hash_h "${_gen}/git_commit_hash.h")
add_custom_target(orcacore_git_hash
    BYPRODUCTS "${_git_hash_h}"
    COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${ORCA_SRC}" "-DOUT_FILE=${_git_hash_h}" -P "${ORCA_SRC}/src/slic3r/GitCommitHash.cmake")
configure_file("${ORCA_SRC}/src/slic3r/GUI/BuildCommit.cpp" "${_gen}/BuildCommit.cpp" COPYONLY)
configure_file("${ORCA_SRC}/src/slic3r/GUI/BuildCommit.hpp" "${_gen}/BuildCommit.hpp" COPYONLY)
list(APPEND _upstream_sources "${_gen}/BuildCommit.cpp")
set(_about_inc "${_gen}/AboutEntries.inc")
add_custom_command(OUTPUT "${_about_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/AboutDialog.cpp" CopyrightsDialog CopyrightsDialog "${_about_inc}" fill_entries
    DEPENDS "${ORCA_SRC}/src/slic3r/GUI/AboutDialog.cpp" "${_x}")
# Help > Network Test (NetworkTest.cpp): NetworkTestDialog's tests on a stand-in dialog
set(_nettest_src "${ORCA_SRC}/src/slic3r/GUI/NetworkTestDialog.cpp")
set(_nettest_inc "${_gen}/NetworkTest.inc")
set(_nettest_types_inc "${_gen}/NetworkTestTypes.inc")
add_custom_command(OUTPUT "${_nettest_inc}" "${_nettest_types_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/NetworkTestDialog.hpp" - - "${_nettest_types_inc}" "enum TestJob"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_nettest_src}" NetworkTestDialog NetworkTestDialog "${_nettest_inc}"
            get_os_info get_dns_info start_all_job start_all_job_sequence start_test_url start_test_ping_thread
            start_test_github_thread start_test_bing_thread get_studio_version
    DEPENDS "${_nettest_src}" "${ORCA_SRC}/src/slic3r/GUI/NetworkTestDialog.hpp" "${_x}")
# Help > Migrate Filament Presets (LegacyPresets.cpp): GUI_App::check_legacy_user_filament_presets on a stand-in app
set(_legacy_inc "${_gen}/LegacyFilamentPresets.inc")
add_custom_command(OUTPUT "${_legacy_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/GUI_App.cpp" GUI_App LegacyApp "${_legacy_inc}"
            check_legacy_user_filament_presets
    DEPENDS "${ORCA_SRC}/src/slic3r/GUI/GUI_App.cpp" "${_x}")
# Help > Troubleshoot Center (Troubleshoot.cpp): TroubleshootDialog's system info, profiles overview, packing, logs
set(_ts_src "${ORCA_SRC}/src/slic3r/GUI/TroubleshootDialog.cpp")
set(_ts_inc "${_gen}/Troubleshoot.inc")
add_custom_command(OUTPUT "${_ts_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_ts_src}" TroubleshootDialog TroubleshootDialog "${_ts_inc}"
            --sub "wxGetApp().plater()" "ts_plater()"
            --sub "OpenGLManager::get_gl_info()" "ts_gl_info()"
            GetSysInfoAll GetProfilesOverview GetCPUinfo parse_lscpu_etc GetRAMinfo GetGPUinfo GetMONinfo
            PackAll RebuildSystemProfiles ClearLogs UpdateLogsStorage
    DEPENDS "${_ts_src}" "${_x}"
    VERBATIM)
# Help > Check for Updates (Updates.cpp): GUI_App::check_new_version_sf against this port's GitHub releases
set(_upd_inc "${_gen}/CheckNewVersion.inc")
add_custom_command(OUTPUT "${_upd_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/GUI_App.cpp" GUI_App UpdApp "${_upd_inc}"
            --sub "return; // desktop release downloads do not apply to the Android build" ""
            --sub "app_config->version_check_url()" "port_releases_url()"
            --sub "get_version(SoftFever_VERSION, matcher)" "get_version(port_version(), matcher)"
            check_new_version_sf
    DEPENDS "${ORCA_SRC}/src/slic3r/GUI/GUI_App.cpp" "${_x}"
    VERBATIM)
# Help > Benchmark 3D Scene (Benchmark.cpp): frame_time_stats and FrameTimeStats of SceneBenchmark.cpp
set(_bench_inc "${_gen}/SceneBenchmarkTypes.inc")
set(_bench_fns_inc "${_gen}/SceneBenchmark.inc")
add_custom_command(OUTPUT "${_bench_inc}" "${_bench_fns_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/SceneBenchmark.hpp" - - "${_bench_inc}" "struct FrameTimeStats"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/SceneBenchmark.cpp" - - "${_bench_fns_inc}" frame_time_stats
    DEPENDS "${ORCA_SRC}/src/slic3r/GUI/SceneBenchmark.cpp" "${ORCA_SRC}/src/slic3r/GUI/SceneBenchmark.hpp" "${_x}")
# Speed Dial (SpeedDial.cpp + WebDialogs.cpp): ActionRegistry, the NativeCommands catalog (its runners are the app's) and
# SpeedDialWebDialog's page commands
set(_sd_reg_src "${ORCA_SRC}/src/slic3r/GUI/ActionRegistry.cpp")
set(_sd_cmd_src "${ORCA_SRC}/src/slic3r/GUI/NativeCommands.cpp")
set(_sd_dlg_src "${ORCA_SRC}/src/slic3r/GUI/SpeedDialDialog.cpp")
set(_sd_types_inc "${_gen}/SpeedDialTypes.inc")
set(_sd_reg_inc "${_gen}/SpeedDialRegistry.inc")
set(_sd_regfns_inc "${_gen}/SpeedDialRegistryFns.inc")
set(_sd_cat_inc "${_gen}/SpeedDialCatalog.inc")
set(_sd_dlg_inc "${_gen}/SpeedDialDialog.inc")
add_custom_command(OUTPUT "${_sd_types_inc}" "${_sd_reg_inc}" "${_sd_regfns_inc}" "${_sd_cat_inc}" "${_sd_dlg_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/ActionRegistry.hpp" - - "${_sd_types_inc}"
            "enum class ActionChange" "enum class AppActionKind" "struct AppActionRunResult" "struct AppActionId" "struct AppAction"
            "class ActionRegistry" requires_mode_switch
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_sd_reg_src}" - - "${_sd_reg_inc}"
            --sub "wxGetApp().sidebar().jump_to_option(opt_key, type, category);" "jump_to_option(opt_key, type, category);"
            cap_favourites parse_config_json read_section write_section read_string_array frecency_score setting_type_context
            mode_key "struct SettingAction" seed_from drop_stale "struct PlateAction" "struct RecentProjectAction" action_json
            find_loaded_source_name "struct PluginScriptAction" "make_action(const std::string& plugin_key" "struct PluginPageAction"
            make_page_action
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_sd_reg_src}" ActionRegistry ActionRegistry "${_sd_regfns_inc}"
            --sub "assert(wxThread::IsMain());" ""
            relocalize_builtins upsert remove seed_state load_persisted by_id find run set_favourite favourite_ids
            reorder_favourites materialize_plate_actions materialize_recent_project_actions should_ask suppress_ask
            tooltip_expanded set_tooltip_expanded snapshot init refresh_source refresh_capability refresh_page_capability
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_sd_cmd_src}" - - "${_sd_cat_inc}" --drop-lambdas "const std::string&"
            "struct CommandAction" build_command_catalog
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_sd_dlg_src}" - - "${_sd_dlg_inc}.fns" json_int_or mode_label speed_dial_ui_strings
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_sd_dlg_src}" SpeedDialWebDialog SpeedDialWebDialog "${_sd_dlg_inc}"
            localized_strings on_script_message handle_web_command search_tabs run_action open_wiki send_actions
    DEPENDS "${_sd_reg_src}" "${_sd_cmd_src}" "${_sd_dlg_src}" "${ORCA_SRC}/src/slic3r/GUI/ActionRegistry.hpp" "${_x}"
    VERBATIM)
# EditGCodeDialog's placeholder tree and descriptions (GCodePlaceholders.cpp)
set(_egc_src "${ORCA_SRC}/src/slic3r/GUI/EditGCodeDialog.cpp")
set(_egc_inc "${_gen}/EditGCode.inc")
set(_egc_types_inc "${_gen}/EditGCodeTypes.inc")
add_custom_command(OUTPUT "${_egc_inc}" "${_egc_types_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/EditGCodeDialog.hpp" - - "${_egc_types_inc}" "enum class ParamType"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/MsgDialog.cpp" - - "${_egc_inc}.wrap" get_wraped_wxString
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_egc_src}" - - "${_egc_inc}.fns" get_type
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_egc_src}" EditGCodeDialog EditGCodeDialog "${_egc_inc}"
            init_params_list add_presets_placeholders selection_changed
    DEPENDS "${_egc_src}" "${ORCA_SRC}/src/slic3r/GUI/EditGCodeDialog.hpp" "${ORCA_SRC}/src/slic3r/GUI/MsgDialog.cpp" "${_x}")
# IDEX/IQEX modes editor (IMEXModes.cpp): IMEXModesCtrl compiled from upstream on stand-in widgets
set(_imex_src "${ORCA_SRC}/src/slic3r/GUI/IMEXModesCtrl.cpp")
set(_imex_types_inc "${_gen}/IMEXModesTypes.inc")
set(_imex_inc "${_gen}/IMEXModes.inc")
add_custom_command(OUTPUT "${_imex_types_inc}" "${_imex_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/IMEXModesCtrl.hpp" - - "${_imex_types_inc}" "class IMEXModesCtrl"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_imex_src}" IMEXModesCtrl IMEXModesCtrl "${_imex_inc}"
            set_grid_size load_from_config get_mode_data matches_config roles_for_mode unique_mode_name fix_row_name
            role_style role_hint role_offered role_allowed_on_tile next_tile_role suggested_mode_names apply_btn
            snapshot_row row_differs_from_parent row_has_parent_counterpart remove_row clear_rows active_tools_string
            notify refresh_reset_buttons reset_row_to_parent
    DEPENDS "${_imex_src}" "${ORCA_SRC}/src/slic3r/GUI/IMEXModesCtrl.hpp" "${_x}")
# IDEX/IQEX plate zones and ghosts (ImexPlate.cpp): PartPlate's zone / ghost / head-colour code on a stand-in plate
set(_pp_src "${ORCA_SRC}/src/slic3r/GUI/PartPlate.cpp")
set(_imexplate_inc "${_gen}/ImexPlate.inc")
add_custom_command(OUTPUT "${_imexplate_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_pp_src}" PartPlate ImexPlate "${_imexplate_inc}"
            get_imex_mode get_effective_imex_mode get_imex_head_filament_map calc_imex_zones resolve_active_mode_tools
            calc_imex_ghosts "get_imex_head_filament_color(int physical_head," has_imex_multimaterial_conflict set_imex_head_filament_map
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/Plater.cpp" - - "${_imexplate_inc}.warn" collect_imex_warnings
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/GCodeViewer.cpp" GCodeViewer GCodeViewer "${_imexplate_inc}.marker" resolve_imex_marker_plan
    DEPENDS "${_pp_src}" "${ORCA_SRC}/src/slic3r/GUI/Plater.cpp" "${ORCA_SRC}/src/slic3r/GUI/GCodeViewer.cpp" "${_x}"
    VERBATIM)
# 3DPrinterOS (CloudHosts.cpp): the client's members; its two dialogs are the app's
set(_c3dp_inc "${_gen}/C3DPrinterOS.inc")
add_custom_command(OUTPUT "${_c3dp_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/Utils/3DPrinterOS.cpp" C3DPrinterOS C3DPrinterOS "${_c3dp_inc}"
            C3DPrinterOS get_name test login get_test_ok_msg get_test_failed_msg upload log_out validate_version_text
            make_url get_api_auth_token login_with_token check_session save_api_session load_api_session send_form
            get_cloud_projects_list get_cloud_printer_types update_file
    DEPENDS "${ORCA_SRC}/src/slic3r/Utils/3DPrinterOS.cpp" "${_x}"
    VERBATIM)
# the print host send dialogs' extras (SendDialogs.cpp): Flashforge's slot / material matching
set(_ffsend_inc "${_gen}/FfSend.inc")
add_custom_command(OUTPUT "${_ffsend_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/PrintHostDialogs.cpp" - - "${_ffsend_inc}.fn" color_distance_sq
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/PrintHostDialogs.cpp" FlashforgePrintHostSendDialog FfSendLogic "${_ffsend_inc}"
            normalize_material slot_matches_filament to_wx_colour
    DEPENDS "${ORCA_SRC}/src/slic3r/GUI/PrintHostDialogs.cpp" "${_x}"
    VERBATIM)
# the keyboard shortcuts dialog (KeyShortcuts.cpp): its pages, the capture rules and the conflict question
set(_kb_src "${ORCA_SRC}/src/slic3r/GUI/KBShortcutsDialog.cpp")
set(_kb_inc "${_gen}/KbDialog.inc")
add_custom_command(OUTPUT "${_kb_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_kb_src}" - - "${_kb_inc}.fn" shortcut_names mouse_action to_wx global_key_advice join_keys
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_kb_src}" KBShortcutsDialog KbDialog "${_kb_inc}" fill_pages reset_shortcut take_chord_from
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_kb_src}" ShortcutCaptureDialog KbCapture "${_kb_inc}.capture" record
    DEPENDS "${_kb_src}" "${_x}"
    VERBATIM)
# File > Publish 3MF (PublishDialog.cpp): PublishSettingsDialog's model, selection, filter and result on stand-in widgets,
# its helpers, Tab::translate_category and Field's value formatting (ConfigValueFormatter)
set(_pubdlg_src "${ORCA_SRC}/src/slic3r/GUI/PublishSettingsDialog.cpp")
set(_pubdlg_inc "${_gen}/PublishDialog.inc")
add_custom_command(OUTPUT "${_pubdlg_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/PublishSettingsDialog.hpp" - - "${_pubdlg_inc}.types"
            "struct PublishMaterialIdentity" "struct MixedDependencyIssue"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_pubdlg_src}" - - "${_pubdlg_inc}.fn" filament_color_hex material_identity
            material_display_name material_title mixed_slot_components mixed_filament_label project_used_filament_slots
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_pubdlg_src}" PublishSettingsDialog PubDialog "${_pubdlg_inc}" build_option_model
            apply_selection GetPublishedKeys GetPublishedMaterialKeys unpublished_mixed_components category_has_selection
            refresh_tab_indicators full_keys_for_slot on_full_toggle on_enable_toggle select_all select_visible apply_filter
            apply_pseudo_filter refresh_filter apply_visibility row_is_visible save_scroll_position show_outer_page
            show_inner_page show_mixed_page
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/Tab.cpp" Tab Tab "${_pubdlg_inc}.tab" translate_category
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/Field.cpp" - - "${_pubdlg_inc}.field" double_to_string
            get_thumbnail_string get_thumbnails_string
    DEPENDS "${_pubdlg_src}" "${ORCA_SRC}/src/slic3r/GUI/PublishSettingsDialog.hpp" "${ORCA_SRC}/src/slic3r/GUI/Tab.cpp"
            "${ORCA_SRC}/src/slic3r/GUI/Field.cpp" "${_x}"
    VERBATIM)
# the plugin actions of GUI_App (Plugins.cpp): Refresh Plugins and Install Local Plugin of the Speed Dial
set(_plugapp_inc "${_gen}/PluginsApp.inc")
add_custom_command(OUTPUT "${_plugapp_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/GUI_App.cpp" GUI_App GUI_App "${_plugapp_inc}" refresh_plugins install_local_plugin
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/OptionsGroup.cpp" OptionsGroup PickGroup "${_plugapp_inc}.pick" pick_plugin
    DEPENDS "${ORCA_SRC}/src/slic3r/GUI/GUI_App.cpp" "${ORCA_SRC}/src/slic3r/GUI/OptionsGroup.cpp" "${_x}"
    VERBATIM)
# the sidebar's filaments (Filaments.cpp): Sidebar's add / delete / merge, the mixed filaments, the auto flushing volumes,
# Plater's filament count / delete updates, ObjectList's extruder remaps, PartPlate's per-filament data, on stand-ins
set(_fil_inc "${_gen}/Filaments.inc")
set(_plater_src "${ORCA_SRC}/src/slic3r/GUI/Plater.cpp")
add_custom_command(OUTPUT "${_fil_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_plater_src}" Sidebar FlSidebar "${_fil_inc}"
            add_filament add_custom_filament delete_filament change_filament collect_physical_filament_info ensure_mixed_filament
            add_mixed_filament edit_mixed_filament remove_all_mixed_filaments delete_mixed_filament_at "has_broken_mixed_filament() const"
            "has_broken_mixed_filament(const PartPlate" auto_calc_flushing_volumes auto_calc_flushing_volumes_internal should_show_SEMM_buttons
            decompose_filament_color
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_plater_src}" - - "${_fil_inc}.fn" blend_mixed_color serialize_mixed_gradient_curve_if_custom
            create_mixed_filament_from_result get_min_flush_volumes
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_plater_src}" Plater FlPlater "${_fil_inc}.pl" --sub "Slic3r::GUI::PartPlateList" "FlPlateList" on_filament_count_change on_filaments_delete
            mixed_filament_config_indices physical_filament_config_indices get_extruder_colors_from_plater_config update_filament_colors_in_full_config
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/GUI_ObjectList.cpp" ObjectList FlObjectList "${_fil_inc}.ol"
            update_filament_values_for_items update_filament_values_for_items_when_delete_filament
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/PartPlate.cpp" PartPlate FlPlate "${_fil_inc}.pp"
            get_other_layers_print_sequence set_other_layers_print_sequence update_first_layer_print_sequence
            update_first_layer_print_sequence_when_delete_filament set_filament_count on_filament_added on_filament_deleted
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/GUI_App.cpp" - - "${_fil_inc}.app" has_filaments is_support_filament
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/WipeTowerDialog.cpp" WipingDialog FlWiping "${_fil_inc}.wipe"
            CalcFlushingVolume CalcFlushingVolumes
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/WipeTowerDialog.cpp" - - "${_fil_inc}.mod" is_flush_config_modified
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/MixedFilamentDialog.hpp" - - "${_fil_inc}.res" "struct MixedFilamentResult"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/ColorDecomposeDialog.hpp" - - "${_fil_inc}.cdt" "enum class DecomposeBaseColor"
            "struct DecomposeComponent" "struct ColorDecomposeResult"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/ColorDecomposeSupport.hpp" - - "${_fil_inc}.cst" "struct DecomposeOfficialComponent"
            "struct DecomposeMissingComponent"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/ColorDecomposeSupport.cpp" - - "${_fil_inc}.cs" decompose_normalize_color_hex
            decompose_base_color_en decompose_base_color_display decompose_basic_type_from_source decompose_basic_filament_id
            set_created_standard_component_metadata lookup_decompose_official_component find_decompose_standard_preset_name
            official_basic_type_from_preset_name filament_type_for_color_decompose find_existing_decompose_component
            prepare_decompose_mixed_result count_decompose_new_physical_filaments confirm_create_decompose_missing_components
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/ColorDecomposeDialog.cpp" - - "${_fil_inc}.cdfn" mode_index wx_colour_to_recipe_rgb
            hex_to_wx_colour same_rgb standard_base_color_from_key pure_color_for_base standard_base_color_for to_dialog_result
            material_type_matches
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/ColorDecomposeDialog.cpp" ColorDecomposeDialog ColorDecomposeDialog "${_fil_inc}.cd"
            create_filament_selector select_mode update_card_styles update_card_visibility update_filament_limit_warning
            set_missing_physical_calculator update_ok_button_state update_matched_color_display try_build_single_base_result
            compute_decomposition
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/TextureImportDialog.hpp" - - "${_fil_inc}.tit" "enum class TextureImportState"
            "enum class TextureAutoMixMode" "enum class TextureFilamentKind" "struct TextureFilamentEntry" "struct TextureNewMixedFilament"
            "struct FilamentMappingRow"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/TextureImportDialog.cpp" - - "${_fil_inc}.tifn" parse_color_string
            rgb_to_hex filament_name_to_wx_string texture_normalize_color_hex texture_rgba_to_hex texture_entry_is_physical
            texture_entry_is_mixed texture_entry_is_pla_basic texture_entry_official_basic texture_recipe_mode starts_with_preset_name
            resolve_default_virtual_filament_preset_name
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/TextureImportDialog.cpp" TextureImportDialog TextureImportDialog
            "${_fil_inc}.ti" sort_current_matches_by_filament_index restore_current_match_order max_filament_count
            can_add_virtual_filament find_closest_filament_index add_virtual_filament add_virtual_mixed_filament
            compact_used_virtual_filaments compute_display_numbers apply_auto_standard_mix reset_auto_mix
            add_decomposed_mixed_filament do_auto_match build_matches_from_rows has_valid_result is_params_dirty
            update_filament_color_map
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_plater_src}" - - "${_fil_inc}.tihas" has_importable_texture
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_plater_src}" Plater::priv FlPlater "${_fil_inc}.tiapply" --sub "sidebar->add_custom_filament" "m_sidebar->add_custom_filament"
            --sub "create_mixed_filament_from_result(sidebar," "create_mixed_filament_from_result(m_sidebar," apply_textured_mesh_import_result
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/DeviceCore/DevConfigUtil.cpp" DevPrinterConfigUtil DevPrinterConfigUtil "${_fil_inc}.dev"
            get_toolhead_display_name
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/WipeTowerDialog.cpp" WipingDialog FlushDialog "${_fil_inc}.wd"
            BuildTableObjStr BuildTextObjStr StoreFlushData ExpandToFullMatrix GetFlattenMatrix GetMultipliers
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/WipeTowerDialog.cpp" - - "${_fil_inc}.wdfn" MatrixFlatten
            extract_physical_sub_matrix expand_physical_to_full_matrix
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/Utils/ColorSpaceConvert.cpp" - - "${_fil_inc}.csc" rgb_to_yuv PivotRGB PivotXYZ
            XYZ2RGB Lab2XYZ Lab2RGB RGB2XYZ XYZ2Lab RGB2Lab RGB2HSV DeltaE00 DeltaE94 DeltaE76
    DEPENDS "${_plater_src}" "${ORCA_SRC}/src/slic3r/GUI/GUI_ObjectList.cpp" "${ORCA_SRC}/src/slic3r/GUI/PartPlate.cpp"
            "${ORCA_SRC}/src/slic3r/GUI/GUI_App.cpp" "${ORCA_SRC}/src/slic3r/GUI/WipeTowerDialog.cpp" "${ORCA_SRC}/src/slic3r/GUI/MixedFilamentDialog.hpp" "${ORCA_SRC}/src/slic3r/Utils/ColorSpaceConvert.cpp" "${ORCA_SRC}/src/slic3r/GUI/DeviceCore/DevConfigUtil.cpp" "${ORCA_SRC}/src/slic3r/GUI/ColorDecomposeDialog.cpp" "${ORCA_SRC}/src/slic3r/GUI/ColorDecomposeSupport.cpp" "${ORCA_SRC}/src/slic3r/GUI/TextureImportDialog.cpp" "${ORCA_SRC}/src/slic3r/GUI/TextureImportDialog.hpp" "${_x}"
    VERBATIM)
# belt printers' purge prism (BeltPurge.cpp): ensure_belt_purge_tower on stand-in plates
set(_belt_inc "${_gen}/BeltPurgeTower.inc")
add_custom_command(OUTPUT "${_belt_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/BeltPurgeTower.hpp" - - "${_belt_inc}.types" "struct BeltPurgeSignature"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/BeltPurgeTower.cpp" - - "${_belt_inc}" ensure_belt_purge_tower
    DEPENDS "${ORCA_SRC}/src/slic3r/GUI/BeltPurgeTower.hpp" "${ORCA_SRC}/src/slic3r/GUI/BeltPurgeTower.cpp" "${_x}"
    VERBATIM)
# the Move gizmo's Align / Distribute (Align.cpp): GLGizmoAlignment on a stand-in selection and canvas
set(_align_inc "${_gen}/Alignment.inc")
set(_align_src "${ORCA_SRC}/src/slic3r/GUI/Gizmos/GLGizmoAlignment.cpp")
add_custom_command(OUTPUT "${_align_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/Gizmos/GLGizmoAlignment.hpp" - - "${_align_inc}.cls" "class GLGizmoAlignment"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_align_src}" GLGizmoAlignment GLGizmoAlignment "${_align_inc}" GLGizmoAlignment align_objects
            distribute_objects align_to_y_max align_to_y_min align_to_x_max align_to_x_min align_to_center align_to_z_max
            align_to_z_min distribute_y distribute_x distribute_z align_objects_generic distribute_objects_generic can_align
            can_distribute get_current_coord generate_displacement get_selected_objects_info is_parts_selection
            get_selected_part_volumes get_items_box get_parent_box get_selection take_snapshot apply_transformation
            finish_operation get_items_count
    DEPENDS "${_align_src}" "${ORCA_SRC}/src/slic3r/GUI/Gizmos/GLGizmoAlignment.hpp" "${_x}"
    VERBATIM)
# the Texture displacement gizmo (TextureDisp.cpp): its layer stack, filament palette and bake on a stand-in gizmo, the
# bake job run in the command
set(_td_inc "${_gen}/TextureDisp.inc")
set(_td_src "${ORCA_SRC}/src/slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.cpp")
set(_td_bake "${ORCA_SRC}/src/slic3r/GUI/Jobs/TextureDisplacementBakeJob")
set(_td_prep "${ORCA_SRC}/src/slic3r/GUI/Jobs/TextureDisplacementPrepareJob")
add_custom_command(OUTPUT "${_td_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_td_src}" - - "${_td_inc}.fn" mixed_slot_signature same_palette srgb_to_lab lab_to_srgb
            point_in_triangle_coplanar remap_texture_paint_spatial ray_plane_hit upload_height_thumbnail upload_color_texture
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_td_src}" GLGizmoTextureDisplacement GLGizmoTextureDisplacement "${_td_inc}" facets_data_of
            any_layer_colors color_settings_for cached_palette palette_quantizers filament_palette mix_targets rank_mixes pick_mixes
            make_palette palette_filaments make_palette_quantizer add_texture_layer get_library_texture set_layer_texture
            import_custom_texture remove_texture_layer swap_layer_slots move_texture_layer bake apply_standard_mode_presets
            bake_standard store_panel_state texture_volume set_active_layer capture_view_projection select_visible_faces
            texture_volume_raycaster_index subdivide_model collect_paint_region masks_after_subdivision painted_area_mm2
            estimated_refined_triangles v2_recommendation prepare_mesh subdivide_model_adaptive smooth_model plan_remesh remesh_model
            rebuild_subdivide_preview queue_prepare load_image_adjust_source open_image_adjust invalidate_adjust_textures
            close_image_adjust commit_image_adjust update_adjust_anchor adjust_plane_point adjust_handle_center adjust_tangent_basis
            on_mouse_adjust_texture layer_texture_aspect update_uv_editor push_uv_pane_state run_uv_command on_uv_command
            process_uv_commands uv_editor_island_transforms on_island_edited on_uv_vertex_edited cut_island
            compute_uv_editor_distortion_colors patch_in_world build_island_move_set island_group_of join_island_groups
            on_mouse_seam seam_edge_at toggle_seam_at seam_vertex_at mark_seam_path rebuild_seam_anchor_overlay
            rebuild_seam_hover_overlay rebuild_seam_overlay apply_view_mode rebuild_uvcheck_mesh render_uvcheck_mesh
            rebuild_shaded_preview_mesh render_shaded_preview_mesh compute_shaded_active_faces shaded_preview_ready
            get_layer_height_texture get_layer_color_texture model_uv_corners compute_layer_vertex_uvs layer_projection_frame
            compute_layer_corner_uvs island_highlight_selection rebuild_island_overlay render_island_overlay apply_projection_frame
            --sub "glsafe(::glBindTexture(GL_TEXTURE_2D, tex->get_id()));" "rec_bind(0, tex);"
            --sub "glsafe(::glBindTexture(GL_TEXTURE_2D, (GLuint) color_tex->get_id()));" "rec_bind(1, color_tex);"
            --sub "glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap));" "rec_wrap(wrap);"
            --sub "constexpr double pick_tolerance_px = 8.0;" "const double pick_tolerance_px = 8.0 * touch_scale();"
            --sub "TriangleSelectorGUI &selector = *m_triangle_selectors[size_t(idx)];" "PainterPart selector(idx);"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/MeshUtils.cpp" MeshRaycaster MeshRaycaster "${_td_inc}.ray"
            get_unobscured_idxs unproject_on_mesh
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/UVEditorCanvas.cpp" - - "${_td_inc}.uvfn" undirected_edge_key
            point_in_triangle angle_delta
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/UVEditorCanvas.cpp" UVEditorCanvas UVEditorCanvas "${_td_inc}.uv"
            set_islands set_island_transforms set_island_fill_colors set_uv_transform set_background_texture reset_view
            run_command set_pane_state update_status island_uv content_bounds framed_bounds fit_view_to_content
            view_half_extents screen_to_uv island_at update_hover set_select_mode vertex_at edge_at move_vertex_raw
            island_rotation_deg island_centroid uv_delta_to_unwrap snap_correction selected_edge_endpoints end_gesture
            cancel_gesture grab_mouse drop_mouse on_key on_mouse
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_td_bake}.hpp" - - "${_td_inc}.bcls" "struct TextureDisplacementBakeInput"
            "class TextureDisplacementBakeJob"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_td_bake}.cpp" TextureDisplacementBakeJob TextureDisplacementBakeJob "${_td_inc}.bake"
            TextureDisplacementBakeJob process finalize
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_td_bake}.cpp" - - "${_td_inc}.bfn" queue_texture_displacement_bake
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_td_prep}.hpp" - - "${_td_inc}.pcls" "enum class TextureDisplacementPrepareOutcome"
            "struct TextureDisplacementPrepareInput" "class TextureDisplacementPrepareJob"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_td_prep}.cpp" TextureDisplacementPrepareJob TextureDisplacementPrepareJob "${_td_inc}.prep"
            TextureDisplacementPrepareJob process finalize
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_td_prep}.cpp" - - "${_td_inc}.pfn" queue_texture_displacement_prepare
    DEPENDS "${_td_src}" "${_td_bake}.hpp" "${_td_bake}.cpp" "${ORCA_SRC}/src/slic3r/GUI/MeshUtils.cpp" "${ORCA_SRC}/src/slic3r/GUI/UVEditorCanvas.cpp" "${_td_prep}.hpp" "${_td_prep}.cpp" "${_x}"
    VERBATIM)
# the OrcaCloud account (Cloud.cpp): ZUserLogin's page commands and GUI_App's login / logout / preset sync on stand-ins
set(_login_src "${ORCA_SRC}/src/slic3r/GUI/WebUserLoginDialog.cpp")
set(_guiapp_src "${ORCA_SRC}/src/slic3r/GUI/GUI_App.cpp")
set(_cloud_inc "${_gen}/Cloud.inc")
add_custom_command(OUTPUT "${_cloud_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_login_src}" - - "${_cloud_inc}.fn" reserve_loopback_port rewrite_loopback_url
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_login_src}" ZUserLogin ZUserLogin "${_cloud_inc}.login"
            --sub "wxTheApp->CallAfter" "wxGetApp().CallAfter" ensure_loopback_port OnScriptMessage
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_guiapp_src}" GUI_App CloudApp "${_cloud_inc}"
            --sub "Slic3r::PluginManager" "PluginManager" --sub "GUI::wxGetApp()" "wxGetApp()"
            on_http_error enable_user_preset_folder check_privacy_update reload_settings remove_user_presets
            maybe_migrate_user_presets_on_login check_preset_parent_available add_pending_vendor_preset load_pending_vendors
            sync_preset update_single_bundle sync_bundle check_bundle_updates unsubscribe_bundle start_sync_user_preset
            stop_sync_user_preset restart_sync_user_preset force_push_conflicting_preset get_delete_cache_presets
            get_delete_cache_presets_lock process_delete_presets delete_preset_from_cloud preset_deleted_from_cloud
            extract_setting_id_from_info scan_orphaned_info_files request_user_logout get_login_info post_logout_to_webview
    DEPENDS "${_login_src}" "${_guiapp_src}" "${_x}"
    VERBATIM)
# the web dialogs (WebDialogs.cpp): the host theme of WebViewHostDialog, PresetBundleDialog, ExportPresetBundleDialog
set(_whd_src "${ORCA_SRC}/src/slic3r/GUI/Widgets/WebViewHostDialog.cpp")
set(_pbd_src "${ORCA_SRC}/src/slic3r/GUI/PresetBundleDialog.cpp")
set(_epb_src "${ORCA_SRC}/src/slic3r/GUI/ExportPresetBundleDialog.cpp")
set(_webdlg_incs "${_gen}/WebThemeFns.inc" "${_gen}/WebThemeMembers.inc" "${_gen}/PresetBundleDialogFns.inc"
    "${_gen}/ExportPresetCase.inc" "${_gen}/ExportPresetBundleStatics.inc" "${_gen}/ExportPresetBundleFns.inc")
add_custom_command(OUTPUT ${_webdlg_incs}
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_whd_src}" - - "${_gen}/WebThemeFns.inc"
            --sub "wxGetApp().dark_mode()" "web_dark_mode()"
            --sub "GUI_App&       app    = wxGetApp();" "WebApp app;"
            --sub "StateColor::darkModeColorFor(wxColour(\"#009688\"))" "orca_accent()"
            css_color host_theme_name host_theme_vars_css host_theme_apply_js
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_whd_src}" WebViewHostDialog WebViewHostDialog "${_gen}/WebThemeMembers.inc"
            theme_user_script document_start_injector localized_strings_script element_defaults_user_script
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_pbd_src}" PresetBundleDialog PresetBundleDialog "${_gen}/PresetBundleDialogFns.inc"
            --sub "wxGetApp().mainframe->update_side_preset_ui();" "mainframe.update_side_preset_ui();"
            --sub "auto* evt = new wxCommandEvent(EVT_UPDATE_PRESET_BUNDLE);" "auto* evt = new UpdateBundleEvent();"
            --sub "wxQueueEvent(&wxGetApp(), evt);" "queue_update_bundle(evt);"
            --sub "wxGetApp().CallAfter(" "web_call_after("
            --sub "ExportPresetBundleDialog dlg(this);" "ExportOpener dlg(this);"
            create localized_strings DeleteBundleById on_script_message ListBundles OpenFolder DeleteBundle UnsubscribeBundle
            RefreshBundleMap OpenBundleOnCloud CheckUpdateCloud CompareVer
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/ExportPresetBundleDialog.hpp" - - "${_gen}/ExportPresetCase.inc"
            --sub "enum ExportCase" "enum ExportPresetCase" "enum ExportCase"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_epb_src}" - - "${_gen}/ExportPresetBundleStatics.inc"
            get_machine_name get_filament_name get_vendor_name get_curr_time initial_zip_archive
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_epb_src}" ExportPresetBundleDialog ExportPresetBundleDialog "${_gen}/ExportPresetBundleFns.inc"
            --sub "ExportCase" "ExportPresetCase"
            --sub "NetworkAgent* agent = wxGetApp().getAgent();" "NetworkAgent* agent = nullptr;"
            --sub "wxFileName file_name(path);" "wxFileName2 file_name(path);"
            --sub "OnExportData(path, name, j.value(\"data\", json()));"
                  "OnExportData(path, name, j.value(\"data\", json())); if (!path.empty() && boost::filesystem::exists(std::string(path))) m_save = {{\"path\", std::string(path)}, {\"name\", std::string(name) + \".orca_bundle\"}};"
            Init localized_strings on_script_message InitExportData OnRequestPresets OnExportData show_export_result
    DEPENDS "${_whd_src}" "${_pbd_src}" "${_epb_src}" "${ORCA_SRC}/src/slic3r/GUI/ExportPresetBundleDialog.hpp" "${_x}"
    VERBATIM)
# GLCanvas3D::_render_3d_navigator on ImGuizmo (Navigator.cpp)
set(_nav_inc "${_gen}/Navigator.inc")
add_custom_command(OUTPUT "${_nav_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/GLCanvas3D.cpp" GLCanvas3D NavCanvas "${_nav_inc}"
            --sub "wxGetApp().show_3d_navigator()" "shown"
            --sub "m_rectangle_selection.is_dragging()" "false"
            --sub "m_gizmos.is_dragging()" "gizmo_drag"
            --sub "m_layers_editing.state == LayersEditing::Editing" "false"
            --sub "ImGuiWrapper::to_ImVec4(" "to_imvec4("
            --sub "Camera&     camera           = wxGetApp().plater()->get_camera();" "NavCamera camera{cam, type_request};"
            _render_3d_navigator
    DEPENDS "${ORCA_SRC}/src/slic3r/GUI/GLCanvas3D.cpp" "${_x}"
    VERBATIM)
# ImGuiWrapper::init_style: the base ImGui style of Orca's overlays (ImGuiOverlay.cpp)
set(_imstyle_inc "${_gen}/ImGuiStyle.inc")
add_custom_command(OUTPUT "${_imstyle_inc}"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/ImGuiWrapper.cpp" ImGuiWrapper ImGuiWrapper "${_imstyle_inc}" init_style
            push_toolbar_style pop_toolbar_style push_common_window_style pop_common_window_style push_combo_style pop_combo_style
            bbl_slider_float_style bbl_slider_float "tooltip(const char" "calc_text_size(std::string_view" get_item_spacing
    DEPENDS "${ORCA_SRC}/src/slic3r/GUI/ImGuiWrapper.cpp" "${_x}"
    VERBATIM)
# GLCanvas3D::CenterOfMass (CenterOfMass.cpp) with smooth_sphere of GLModel.cpp
set(_glc_src "${ORCA_SRC}/src/slic3r/GUI/GLCanvas3D.cpp")
set(_com_incs "${_gen}/CenterOfMassDecl.inc" "${_gen}/CenterOfMassHelpers.inc" "${_gen}/CenterOfMassFns.inc" "${_gen}/SmoothSphere.inc")
add_custom_command(OUTPUT ${_com_incs}
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/GLCanvas3D.hpp" - - "${_gen}/CenterOfMassDecl.inc" "class CenterOfMass"
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_glc_src}" - - "${_gen}/CenterOfMassHelpers.inc"
            --sub "#ifdef WIN32" "#if 0"
            mass_sum marker_scale
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_glc_src}" GLCanvas3D::CenterOfMass GLCanvas3D::CenterOfMass "${_gen}/CenterOfMassFns.inc"
            --sub "*wxGetApp().preset_bundle" "*GUI::wxGetApp().preset_bundle"
            --sub "PartPlateList&                   plate_list = wxGetApp().plater()->get_partplate_list();" "PlateLookup plate_list;"
            --sub "!wxGetApp().show_center_of_mass()" "false"
            --sub "wxGetApp().get_shader(\"gouraud_light\")" "com_shader(\"gouraud_light\")"
            --sub "wxGetApp().plater()->get_camera()" "com_camera()"
            --sub "*wxGetApp().imgui()" "imgui_wrapper()"
            model_markers render on_left_down render_details
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/GLModel.cpp" - - "${_gen}/SmoothSphere.inc" smooth_sphere
    DEPENDS "${_glc_src}" "${ORCA_SRC}/src/slic3r/GUI/GLCanvas3D.hpp" "${ORCA_SRC}/src/slic3r/GUI/GLModel.cpp" "${_x}"
    VERBATIM)
# the Assembly view's overlay (Assemble.cpp): GLCanvas3D::_render_assemble_control / _render_assemble_info, has_assemble_view
set(_asm_incs "${_gen}/AssembleOverlay.inc" "${_gen}/HasAssembleView.inc" "${_gen}/AsmTooltipButton.inc")
add_custom_command(OUTPUT ${_asm_incs}
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${_glc_src}" GLCanvas3D AsmCanvas "${_gen}/AssembleOverlay.inc"
            --sub "GLVolume::explosion_ratio" "g_explosion_ratio"
            --sub "wxGetApp().imgui()" "&imgui_wrapper()"
            --sub "m_canvas_type != ECanvasType::CanvasAssembleView" "m_canvas_type != ECanvasType::CanvasAssembleView"
            --sub "GLGizmosManager::EType::MmSegmentation" "AsmGizmos::EType::MmSegmentation"
            --sub "Selection::Instance" "AsmSelection::Instance"
            --sub "Selection::Volume" "AsmSelection::Volume"
            --sub "const float slider_width = 60.0f;" "const float slider_width = 60.0f * get_scale();"
            _render_assemble_control _render_assemble_info _render_assembly_tooltip_button
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/Gizmos/GLGizmoUtils.cpp" - - "${_gen}/AsmTooltipButton.inc"
            --sub "const GLCanvas3D& canvas" "const AsmCanvas& canvas" render_tooltip_button toolbar_circular_button
    COMMAND ${Python3_EXECUTABLE} "${_x}" "${ORCA_SRC}/src/slic3r/GUI/Plater.cpp" Plater::priv PlaterPriv "${_gen}/HasAssembleView.inc"
            --sub "model.objects" "Plater::get().model().objects"
            has_assemble_view
    DEPENDS "${_glc_src}" "${ORCA_SRC}/src/slic3r/GUI/Plater.cpp" "${ORCA_SRC}/src/slic3r/GUI/Gizmos/GLGizmoUtils.cpp" "${_x}"
    VERBATIM)
add_custom_target(orcacore_extracted DEPENDS "${_cut_inc}" "${_plate_inc}" "${_patch_inc}" "${_ears_inc}" "${_emboss_inc}" "${_textlines_inc}" "${_drag_inc}" "${_svg_inc}" "${_publish_inc}" "${_vgexp_inc}" "${_prefs_inc}" "${_homestr_inc}" "${_projstr_inc}" "${_expcfg_inc}" "${_cpdhelp_inc}" "${_calibdlg_inc}" "${_calibpl_inc}" "${_calibut_inc}" "${_shaper_inc}" "${_oauth_types_inc}" "${_oauth_fns_inc}" "${_ppd_inc}" "${_selclip_decl_inc}" "${_selclip_inc}" "${_selclipfn_inc}" "${_snz_inc}" "${_hint_types_inc}" "${_hint_fns_inc}" "${_hint_db_inc}" "${_hint_cereal_inc}" "${_about_inc}" "${_nettest_inc}" "${_nettest_types_inc}" "${_legacy_inc}" "${_ts_inc}" "${_upd_inc}" "${_bench_inc}" "${_bench_fns_inc}" ${_sd_types_inc} ${_sd_reg_inc} ${_sd_regfns_inc} ${_sd_cat_inc} ${_sd_dlg_inc} "${_egc_inc}" "${_egc_types_inc}" "${_imex_types_inc}" "${_imex_inc}" "${_imexplate_inc}" ${_webdlg_incs} "${_nav_inc}" "${_imstyle_inc}" ${_com_incs} ${_asm_incs} "${_cloud_inc}" "${_c3dp_inc}" "${_ffsend_inc}" "${_kb_inc}" "${_pubdlg_inc}" "${_plugapp_inc}" "${_fil_inc}" "${_belt_inc}" "${_align_inc}" "${_td_inc}")
file(GLOB_RECURSE _shim_headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shim/*")
foreach (_f ${_shim_headers})
    file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}/shim" "${_f}")
    configure_file("${_f}" "${CMAKE_CURRENT_BINARY_DIR}/upstream/${_rel}" COPYONLY)
endforeach ()

file(GLOB _core_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp")
add_library(orcacore SHARED ${_core_sources} ${_upstream_sources})
target_include_directories(orcacore PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/upstream"        # shims first: wx/*.h, slic3r/GUI/*.hpp stand-ins
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${ORCA_SRC}/src"
    "${ORCA_SRC}/src/libslic3r"
    "${ORCA_SRC}/deps_src/md4c/src"
    "${_utils}")                                   # HttpServer.cpp includes "CloudProvider.hpp" from slic3r/Utils
# the plugin descriptors the cloud agent fills (slic3r/plugin) are declared next to the pybind11 interface
list(GET CMAKE_PREFIX_PATH 0 _deps_prefix)
file(GLOB _python_include "${_deps_prefix}/libpython/include/python3*")
target_include_directories(orcacore SYSTEM PRIVATE "${ORCA_SRC}/deps_src/pybind11/include" ${_python_include})
# the Python plugin system (slic3r/plugin): compiled from upstream unchanged, copied next to the shims so that its
# relative includes ("../GUI/...") reach the stand-ins, on the bundled CPython (GeneratedConfig.hpp: no bundled uv)
file(GLOB_RECURSE _plugin_files RELATIVE "${ORCA_SRC}/src/slic3r/plugin" "${ORCA_SRC}/src/slic3r/plugin/*")
set(_plugin_sources "")
foreach (_f ${_plugin_files})
    # a stand-in in shim/slic3r/plugin replaces the file (and its .cpp, the app's version is in src/)
    get_filename_component(_stem "${_f}" NAME_WLE)
    get_filename_component(_dir "${_f}" DIRECTORY)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/shim/slic3r/plugin/${_f}" OR EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/shim/slic3r/plugin/${_dir}/${_stem}.hpp")
        continue()
    endif ()
    configure_file("${ORCA_SRC}/src/slic3r/plugin/${_f}" "${CMAKE_CURRENT_BINARY_DIR}/upstream/slic3r/plugin/${_f}" COPYONLY)
    if (_f MATCHES "\\.cpp$")
        list(APPEND _plugin_sources "${CMAKE_CURRENT_BINARY_DIR}/upstream/slic3r/plugin/${_f}")
    endif ()
endforeach ()
set(_bundled_python_root "")
set(ORCA_BUNDLED_UV_EXECUTABLE_CONFIG "")
set(ORCA_UPDATER_SIG_KEY_B64 "")
set(ORCA_UPDATER_SIG_KEY_AVAILABLE 0)
configure_file("${ORCA_SRC}/src/slic3r/GeneratedConfig.hpp.in" "${CMAKE_CURRENT_BINARY_DIR}/upstream/GeneratedConfig.hpp" @ONLY)
target_sources(orcacore PRIVATE ${_plugin_sources})
target_link_libraries(orcacore PRIVATE "${_deps_prefix}/libpython/lib/libpython3.14.so")
find_package(OpenSSL REQUIRED)
target_link_libraries(orcacore PRIVATE OpenSSL::SSL OpenSSL::Crypto imguizmo imgui libslic3r libslic3r_cgal libvgcode glad cereal::cereal miniz md4c-html libcurl
                      EGL GLESv3 android log jnigraphics ${CMAKE_DL_LIBS})
target_compile_definitions(orcacore PRIVATE ORCA_MOBILE_CORE=1 PYBIND11_HAS_SUBINTERPRETER_SUPPORT=0)
add_dependencies(orcacore orcacore_extracted orcacore_git_hash)
# keep only the JNI entry points exported
target_link_options(orcacore PRIVATE "-Wl,--gc-sections" "-Wl,--exclude-libs,ALL" "-Wl,--error-limit=0")

# the plugin terminal's python executable (tools/python_launcher.c), installed by Android from lib/<abi>
add_executable(python3_exe tools/python_launcher.c)
set_target_properties(python3_exe PROPERTIES OUTPUT_NAME "libpython3_exe" SUFFIX ".so")
target_include_directories(python3_exe SYSTEM PRIVATE ${_python_include})
target_link_libraries(python3_exe PRIVATE "${_deps_prefix}/libpython/lib/libpython3.14.so")
target_link_options(python3_exe PRIVATE "LINKER:-rpath,$ORIGIN")
