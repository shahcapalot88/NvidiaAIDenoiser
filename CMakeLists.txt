cmake_minimum_required(VERSION 3.15)
project(OptiXDenoiserGUI LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "This is a Win32 application and only builds on Windows.")
endif()
enable_language(RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT: no Visual C++ redistributable needed on the target PC.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(OptiXDenoiserGUI WIN32 src/main.cpp res/app.rc)
target_compile_definitions(OptiXDenoiserGUI PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(OptiXDenoiserGUI PRIVATE
  user32 gdi32 comdlg32 shell32 ole32 dwmapi comctl32 uxtheme)
if(MSVC)
  target_compile_options(OptiXDenoiserGUI PRIVATE /W3 /utf-8)
endif()
