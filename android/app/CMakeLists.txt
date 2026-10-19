# Orca Slicer for Android: the desktop entry point (OrcaSlicer.cpp) + the unmodified GUI library, packaged by Qt.
# Included from the top-level CMakeLists.txt when ORCA_ANDROID_APP is ON (needs the Qt Android toolchain file).
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets PrintSupport)
qt_standard_project_setup()

set(ORCA_APP_PACKAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/package")
set(ORCA_WX_SRC "${CMAKE_SOURCE_DIR}/../third_party/wxWidgets")

# Java bridges of our wxWidgets Android backends + the manifest
file(MAKE_DIRECTORY "${ORCA_APP_PACKAGE_DIR}/src/org/orcaport/wx" "${ORCA_APP_PACKAGE_DIR}/assets")
file(GLOB _wx_java "${ORCA_WX_SRC}/src/qt/android/java/org/orcaport/wx/*.java")
foreach (_f ${_wx_java})
    get_filename_component(_n "${_f}" NAME)
    configure_file("${_f}" "${ORCA_APP_PACKAGE_DIR}/src/org/orcaport/wx/${_n}" COPYONLY)  # re-configures when the Java changes
endforeach ()
file(GLOB_RECURSE _pkg_res RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/package" CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/package/res/*")
foreach (_f AndroidManifest.xml build.gradle ${_pkg_res})  # res/: the launcher icon
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/package/${_f}" "${ORCA_APP_PACKAGE_DIR}/${_f}" COPYONLY)
endforeach ()

# resources/ -> one asset
find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(GLOB_RECURSE _orca_resource_files CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/resources/*")
add_custom_command(
    OUTPUT "${ORCA_APP_PACKAGE_DIR}/assets/resources.zip"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/make_resources_zip.py"
            "${CMAKE_SOURCE_DIR}/resources" "${ORCA_APP_PACKAGE_DIR}/assets/resources.zip"
    DEPENDS ${_orca_resource_files} "${CMAKE_CURRENT_SOURCE_DIR}/make_resources_zip.py"
    COMMENT "Packing resources/ into resources.zip")
add_custom_target(orca_resources_zip DEPENDS "${ORCA_APP_PACKAGE_DIR}/assets/resources.zip")

qt_add_executable(orcaslicer
    "${CMAKE_SOURCE_DIR}/src/OrcaSlicer.cpp"
    "${CMAKE_SOURCE_DIR}/src/OrcaSlicer.hpp"
    bootstrap.cpp)
add_dependencies(orcaslicer orca_resources_zip)

target_include_directories(orcaslicer PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/libslic3r" "${CMAKE_CURRENT_SOURCE_DIR}/glfw_stub")
target_link_libraries(orcaslicer PRIVATE libslic3r_gui libslic3r cereal::cereal miniz
                      Qt6::Widgets Qt6::OpenGLWidgets Qt6::OpenGL Qt6::PrintSupport android log ${CMAKE_DL_LIBS})
target_compile_definitions(orcaslicer PRIVATE wxDEBUG_LEVEL=0)

set_target_properties(orcaslicer PROPERTIES
    QT_ANDROID_PACKAGE_SOURCE_DIR "${ORCA_APP_PACKAGE_DIR}"
    QT_ANDROID_MIN_SDK_VERSION 28
    QT_ANDROID_TARGET_SDK_VERSION 34
    QT_ANDROID_APP_NAME "Orca Slicer")
