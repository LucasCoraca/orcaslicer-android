// Android has no GLFW. OrcaSlicer.cpp only uses it for the CLI's off-screen thumbnail renderer, which is not
// reachable from the GUI app: glfwInit() reports failure and everything else is inert.
#pragma once
#define GLFW_FALSE 0
#define GLFW_TRUE 1
#define GLFW_RED_BITS 0x21001
#define GLFW_GREEN_BITS 0x21002
#define GLFW_BLUE_BITS 0x21003
#define GLFW_ALPHA_BITS 0x21004
#define GLFW_VISIBLE 0x20004
#define GLFW_CONTEXT_VERSION_MAJOR 0x22002
#define GLFW_CONTEXT_VERSION_MINOR 0x22003
#define GLFW_OPENGL_FORWARD_COMPAT 0x22006
#define GLFW_OPENGL_PROFILE 0x22008
#define GLFW_OPENGL_ANY_PROFILE 0
#define GLFW_OPENGL_CORE_PROFILE 0x32001
#define GLFW_OPENGL_COMPAT_PROFILE 0x32002
#define GLFW_AUTO_POLL_EVENTS 0
struct GLFWwindow;
typedef void (*GLFWerrorfun)(int, const char*);
inline void glfwGetVersion(int *a, int *b, int *c) { *a = *b = *c = 0; }
inline GLFWerrorfun glfwSetErrorCallback(GLFWerrorfun) { return nullptr; }
inline int glfwInit() { return GLFW_FALSE; }
inline int glfwGetError(const char **d) { if (d) *d = "GLFW is not available on Android"; return 1; }
inline void glfwWindowHint(int, int) {}
inline GLFWwindow *glfwCreateWindow(int, int, const char *, void *, GLFWwindow *) { return nullptr; }
inline void glfwMakeContextCurrent(GLFWwindow *) {}
inline void glfwTerminate() {}
