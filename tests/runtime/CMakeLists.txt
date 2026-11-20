# oc_test_runtime: a stand-in OpenXR runtime for the tests, scripted with OC_TEST_RUNTIME_SCRIPT and recording what
# its client did in OC_TEST_RUNTIME_DUMP. D3D11, D3D12, Vulkan and OpenGL (WGL) on Windows; Vulkan and OpenGL (Xlib and
# GLX) on Linux.
add_library(oc_test_runtime SHARED
	runtime.cpp
	runtime_internal.h
	actions.cpp
	extensions.cpp
	swapchain_vk.cpp
	swapchain_gl.cpp
	probe.cpp
	probe.h
	script.cpp
	script.h
)
target_include_directories(oc_test_runtime PRIVATE
	${CMAKE_SOURCE_DIR}/libs/openxr-sdk/include
	${CMAKE_SOURCE_DIR}/libs/openxr-sdk/src/common
	${CMAKE_SOURCE_DIR}/RuntimeExtensions
)
# VK_NO_PROTOTYPES: Vulkan's functions are looked up at run time
target_compile_definitions(oc_test_runtime PRIVATE XR_USE_GRAPHICS_API_VULKAN XR_USE_GRAPHICS_API_OPENGL VK_NO_PROTOTYPES)
if (WIN32)
	target_sources(oc_test_runtime PRIVATE swapchain_d3d11.cpp swapchain_d3d12.cpp oc_test_runtime.def)
	target_include_directories(oc_test_runtime PRIVATE ${CMAKE_SOURCE_DIR}/libs/vulkan/Include)
	target_compile_definitions(oc_test_runtime PRIVATE XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12)
	target_link_libraries(oc_test_runtime PRIVATE ${jsoncppLib} d3d11 d3d12 dxgi opengl32)
else ()
	target_compile_definitions(oc_test_runtime PRIVATE XR_USE_PLATFORM_XLIB)
	# GL and GLX from libGL; X11 for the display the binding names; Vulkan's loader is opened at run time
	target_link_libraries(oc_test_runtime PRIVATE ${jsoncppLib} GL X11 ${CMAKE_DL_LIBS} -Wl,--no-undefined)
endif ()
set_target_properties(oc_test_runtime PROPERTIES PREFIX "")

set(OC_TEST_RUNTIME_PATH "$<TARGET_FILE:oc_test_runtime>")
set(OC_TEST_RUNTIME_MANIFEST ${CMAKE_CURRENT_BINARY_DIR}/oc_test_runtime.json)
file(READ oc_test_runtime.json.in manifest_template)
string(REPLACE "@OC_TEST_RUNTIME_PATH@" "${OC_TEST_RUNTIME_PATH}" manifest_template "${manifest_template}")
file(GENERATE OUTPUT ${OC_TEST_RUNTIME_MANIFEST} CONTENT "${manifest_template}")
set(OC_TEST_RUNTIME_MANIFEST ${OC_TEST_RUNTIME_MANIFEST} PARENT_SCOPE)

add_executable(runtime_smoke runtime_smoke.cpp)
target_include_directories(runtime_smoke PRIVATE ${CMAKE_SOURCE_DIR}/libs/doctest)
target_compile_definitions(runtime_smoke PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS VK_NO_PROTOTYPES)
if (WIN32)
	target_include_directories(runtime_smoke PRIVATE ${CMAKE_SOURCE_DIR}/libs/vulkan/Include)
	target_link_libraries(runtime_smoke PRIVATE ${XrLib} ${jsoncppLib} d3d11 d3d12 dxgi opengl32)
else ()
	target_link_libraries(runtime_smoke PRIVATE ${XrLib} ${jsoncppLib} GL X11 ${CMAKE_DL_LIBS})
endif ()
add_dependencies(runtime_smoke oc_test_runtime)

add_test(NAME runtime_smoke COMMAND ${Python_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/unelevated.py
	--env XR_RUNTIME_JSON=${OC_TEST_RUNTIME_MANIFEST}
	--env OC_TEST_RUNTIME_DUMP=${CMAKE_CURRENT_BINARY_DIR}/runtime_smoke_dump.json
	-- $<TARGET_FILE:runtime_smoke> --test-case-exclude=script_*,*_red_frame,*_session_checks)
# The script's swapchain formats and poses: a process of its own, since the script is read once per process
add_test(NAME runtime_smoke_script COMMAND ${Python_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/unelevated.py
	--env XR_RUNTIME_JSON=${OC_TEST_RUNTIME_MANIFEST}
	--env OC_TEST_RUNTIME_DUMP=${CMAKE_CURRENT_BINARY_DIR}/runtime_smoke_script_dump.json
	--env OC_TEST_RUNTIME_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/scripts/formats_poses.json
	-- $<TARGET_FILE:runtime_smoke> --test-case=script_formats_and_poses)
# A frame on each API (D3D12 on WARP without a GPU; Vulkan and OpenGL skipped without a driver for them, OpenGL on
# Linux also without an X server), and the session checks
add_test(NAME runtime_smoke_apis COMMAND ${Python_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/unelevated.py
	--env XR_RUNTIME_JSON=${OC_TEST_RUNTIME_MANIFEST}
	--env OC_TEST_RUNTIME_DUMP=${CMAKE_CURRENT_BINARY_DIR}/runtime_smoke_apis_dump.json
	--env OC_TEST_RUNTIME_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/scripts/all_apis.json
	-- $<TARGET_FILE:runtime_smoke> --test-case=*_red_frame,*_session_checks)
set_tests_properties(runtime_smoke runtime_smoke_script runtime_smoke_apis PROPERTIES LABELS runtime)

add_executable(runtime_input_smoke runtime_input_smoke.cpp)
if (WIN32)
	target_link_libraries(runtime_input_smoke PRIVATE ${XrLib} d3d11 dxgi)
else ()
	target_link_libraries(runtime_input_smoke PRIVATE ${XrLib} ${CMAKE_DL_LIBS})
endif ()
add_dependencies(runtime_input_smoke oc_test_runtime)
add_test(NAME runtime_input_noext COMMAND ${Python_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/unelevated.py
	--env XR_RUNTIME_JSON=${OC_TEST_RUNTIME_MANIFEST}
	-- $<TARGET_FILE:runtime_input_smoke> noext)
add_test(NAME runtime_input COMMAND ${Python_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/unelevated.py
	--env XR_RUNTIME_JSON=${OC_TEST_RUNTIME_MANIFEST}
	--env OC_TEST_RUNTIME_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/scripts/input_smoke.json
	-- $<TARGET_FILE:runtime_input_smoke> input)
set_tests_properties(runtime_input_noext runtime_input PROPERTIES LABELS runtime)

# Proton's use of vrclient.so: loaded, used, unloaded and loaded again in one process (reload.cpp)
if (NOT WIN32)
	add_executable(runtime_reload reload.cpp)
	target_link_libraries(runtime_reload PRIVATE ${CMAKE_DL_LIBS})
	add_dependencies(runtime_reload oc_test_runtime OCOVR)
	add_test(NAME runtime_reload COMMAND ${Python_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/unelevated.py
		--env XR_RUNTIME_JSON=${OC_TEST_RUNTIME_MANIFEST}
		--env OC_NO_MESSAGE_BOX=1 --env OC_TEST_NO_STEAMVR=1
		--env OC_LOG_DIR=${CMAKE_CURRENT_BINARY_DIR}/reload_logs
		--env OC_STORE_DIR=${CMAKE_CURRENT_BINARY_DIR}/reload_store
		--env XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/reload_config
		-- $<TARGET_FILE:runtime_reload> $<TARGET_FILE:OCOVR>)
	set_tests_properties(runtime_reload PROPERTIES LABELS runtime TIMEOUT 120)
endif ()
