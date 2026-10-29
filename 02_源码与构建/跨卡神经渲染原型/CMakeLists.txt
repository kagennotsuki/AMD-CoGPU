cmake_minimum_required(VERSION 3.20)
project(CrossGpuNR CXX)
set(imguiDir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui-1.90.9")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT MSVC)
    message(WARNING "本项目只在 MSVC + Windows SDK 下验证过；MinGW 未测试")
endif ()

add_executable(transport_bench src/transport_bench.cpp)

target_compile_definitions(transport_bench PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

target_link_libraries(transport_bench PRIVATE
    d3d12 dxgi dxguid)

add_executable(share_probe src/share_probe.cpp)
target_compile_definitions(share_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(share_probe PRIVATE d3d12 dxgi dxguid)

add_executable(resource_probe src/resource_probe.cpp)
target_compile_definitions(resource_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(resource_probe PRIVATE d3d12 dxgi dxguid)

add_executable(transport_cpu src/transport_cpu.cpp)
target_compile_definitions(transport_cpu PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(transport_cpu PRIVATE d3d12 dxgi dxguid)

add_executable(nr_lmxxf_test src/nr_lmxxf_test.cpp)
target_include_directories(nr_lmxxf_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ref")
target_compile_definitions(nr_lmxxf_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(nr_lmxxf_test PRIVATE d3d12 dxgi dxguid)

add_executable(api_probe src/api_probe.cpp)
target_compile_definitions(api_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(api_scan src/api_scan.cpp)
target_compile_definitions(api_scan PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(nr_lmxxf_test2 src/nr_lmxxf_test2.cpp)
target_include_directories(nr_lmxxf_test2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ref)
target_compile_definitions(nr_lmxxf_test2 PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(nr_lmxxf_test2 PRIVATE d3d12 dxgi dxguid)

add_executable(nr_lmxxf_test3 src/nr_lmxxf_test3.cpp)
target_include_directories(nr_lmxxf_test3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ref)
target_compile_definitions(nr_lmxxf_test3 PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(nr_lmxxf_test3 PRIVATE d3d12 dxgi dxguid)

add_executable(pipeline_loop src/pipeline_loop.cpp)
target_include_directories(pipeline_loop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ref)
target_compile_definitions(pipeline_loop PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(pipeline_loop PRIVATE d3d12 dxgi dxguid)

# ===== 游戏内集成：dxgi.dll 代理 =====
enable_language(ASM_MASM)
add_library(dxgi_proxy SHARED ingame/dxgi_proxy_v4.cpp ingame/forwarders.asm ingame/dxgi_proxy.def)
set_target_properties(dxgi_proxy PROPERTIES OUTPUT_NAME "dxgi" PREFIX "" SUFFIX ".dll" MSVC_RUNTIME_LIBRARY "MultiThreaded")
target_compile_definitions(dxgi_proxy PRIVATE ${COMMON_DEFS})
target_include_directories(dxgi_proxy PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ref")
target_link_libraries(dxgi_proxy PRIVATE d3dcompiler dcomp)
add_executable(proxy_selftest ingame/proxy_selftest.cpp)
target_compile_definitions(proxy_selftest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(proxy_selftest PRIVATE d3d12 dxgi dxguid)

# ===== 游戏内集成：d3d12.dll 代理（抓设备与命令队列）=====
add_library(d3d12_proxy SHARED ingame/d3d12_proxy.cpp ingame/d3d12_forwarders.asm ingame/d3d12_proxy.def)
set_target_properties(d3d12_proxy PROPERTIES OUTPUT_NAME "d3d12" PREFIX "" SUFFIX ".dll" MSVC_RUNTIME_LIBRARY "MultiThreaded")
target_compile_definitions(d3d12_proxy PRIVATE ${COMMON_DEFS})
add_executable(copy_isolate ingame/copy_isolate.cpp)
target_compile_definitions(copy_isolate PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(copy_isolate PRIVATE d3d12 dxgi dxguid)

add_executable(swapcopy_probe ingame/swapcopy_probe.cpp)
target_compile_definitions(swapcopy_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(swapcopy_probe PRIVATE d3d12 dxgi dxguid)

add_executable(pipeline4k src/pipeline4k.cpp)
target_include_directories(pipeline4k PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ref)
target_compile_definitions(pipeline4k PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(pipeline4k PRIVATE d3d12 dxgi dxguid d3dcompiler)

add_executable(fake_game ingame/fake_game.cpp)
target_include_directories(fake_game PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ref)
target_compile_definitions(fake_game PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(fake_game PRIVATE d3d12 dxgi dxguid d3dcompiler)

add_executable(present_probe ingame/present_probe.cpp)
target_compile_definitions(present_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(present_probe PRIVATE d3d12 dxgi dxguid)

add_executable(present_probe2 ingame/present_probe2.cpp)
target_compile_definitions(present_probe2 PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(present_probe2 PRIVATE d3d12 dxgi dxguid)

add_executable(present_probe3 ingame/present_probe3.cpp)
target_compile_definitions(present_probe3 PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(present_probe3 PRIVATE d3d12 dxgi dxguid)

add_executable(present_probe4 ingame/present_probe4.cpp)
target_compile_definitions(present_probe4 PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(present_probe4 PRIVATE d3d12 dxgi dxguid dcomp)

add_library(xess_proxy SHARED ingame/libxess_proxy_v9.cpp ingame/xess_forwarders.asm ingame/xess_proxy.def)
target_sources(xess_proxy PRIVATE ${imguiDir}/imgui.cpp ${imguiDir}/imgui_draw.cpp ${imguiDir}/imgui_tables.cpp ${imguiDir}/imgui_widgets.cpp ${imguiDir}/backends/imgui_impl_dx12.cpp)
target_include_directories(xess_proxy PRIVATE ${imguiDir} ${imguiDir}/backends)
set_target_properties(xess_proxy PROPERTIES OUTPUT_NAME "libxess" PREFIX "" SUFFIX ".dll" MSVC_RUNTIME_LIBRARY "MultiThreaded")
target_link_libraries(xess_proxy PRIVATE dxgi d3d12 d3dcompiler)
target_compile_options(xess_proxy PRIVATE /utf-8)
target_compile_definitions(xess_proxy PRIVATE ${COMMON_DEFS} UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# ===== 离线决定性探针：NR 结果落在 fi.color 还是 private_output =====
add_executable(nr_probe_output src/nr_probe_output.cpp)
target_include_directories(nr_probe_output PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ref")
target_compile_definitions(nr_probe_output PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(nr_probe_output PRIVATE /utf-8)
target_link_libraries(nr_probe_output PRIVATE d3d12 dxgi dxguid)

add_executable(nr_probe_abi src/nr_probe_abi.cpp)
target_include_directories(nr_probe_abi PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ref")
target_compile_definitions(nr_probe_abi PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(nr_probe_abi PRIVATE /utf-8)
target_link_libraries(nr_probe_abi PRIVATE d3d12 dxgi dxguid)


# ===== 对照用：把 v5（游戏内跑通过 8800 帧的那版）也编出来 =====
add_library(xess_proxy_v5 SHARED ingame/libxess_proxy_v9.cpp ingame/xess_forwarders.asm ingame/xess_proxy.def)
set_target_properties(xess_proxy_v5 PROPERTIES OUTPUT_NAME "libxess_v5" PREFIX "" SUFFIX ".dll" MSVC_RUNTIME_LIBRARY "MultiThreaded")
target_link_libraries(xess_proxy_v5 PRIVATE dxgi d3d12 d3dcompiler)
target_compile_definitions(xess_proxy_v5 PRIVATE ${COMMON_DEFS} UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(nr_from_capture src/nr_from_capture.cpp)
target_include_directories(nr_from_capture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ref")
target_compile_definitions(nr_from_capture PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(nr_from_capture PRIVATE /utf-8)
target_link_libraries(nr_from_capture PRIVATE d3d12 dxgi dxguid)

add_executable(xess_abi_check ingame/xess_abi_check.cpp)
target_compile_definitions(xess_abi_check PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(xess_abi_check PRIVATE dxgi d3d12)
add_executable(mynr_probe src/mynr_probe.cpp)
target_compile_definitions(mynr_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(mynr_probe PRIVATE dxgi d3d12)
add_executable(mynr_variants src/mynr_variants.cpp)
target_compile_definitions(mynr_variants PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(mynr_variants PRIVATE dxgi d3d12)
add_executable(mynr_knobs src/mynr_knobs.cpp)
target_compile_definitions(mynr_knobs PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(mynr_knobs PRIVATE dxgi d3d12)
add_executable(mynr_history src/mynr_history.cpp)
target_compile_definitions(mynr_history PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(mynr_history PRIVATE dxgi d3d12)
add_executable(mynr_enqueue src/mynr_enqueue.cpp)
target_compile_definitions(mynr_enqueue PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(mynr_enqueue PRIVATE dxgi d3d12)
add_executable(luid_dump src/luid_dump.cpp)
target_compile_definitions(luid_dump PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(luid_dump PRIVATE dxgi d3d12)
# ===== ImGui 冒烟测试（不碰游戏） =====
add_executable(imgui_smoke
    src/imgui_smoke.cpp
    ${imguiDir}/imgui.cpp
    ${imguiDir}/imgui_draw.cpp
    ${imguiDir}/imgui_tables.cpp
    ${imguiDir}/imgui_widgets.cpp
    ${imguiDir}/backends/imgui_impl_dx12.cpp)
target_include_directories(imgui_smoke PRIVATE ${imguiDir} ${imguiDir}/backends)
target_compile_definitions(imgui_smoke PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(imgui_smoke PRIVATE d3d12 dxgi dxguid d3dcompiler)



# ===== lmxxf 能力探测（A1，无推理） =====
add_executable(lmxxf_caps src/lmxxf_caps.cpp)
target_compile_definitions(lmxxf_caps PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(lmxxf_caps PRIVATE d3d12 dxgi dxguid)

add_executable(xshare_probe src/xshare_probe.cpp)
target_compile_definitions(xshare_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(xshare_probe PRIVATE d3d12 dxgi dxguid)

add_executable(mynr_style src/mynr_style.cpp)
target_compile_definitions(mynr_style PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(mynr_style PRIVATE d3d12 dxgi dxguid)

add_executable(mochi_probe src/mochi_probe.cpp)
target_compile_definitions(mochi_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(mochi_probe PRIVATE d3d12 dxgi dxguid)

add_executable(mochi_bench src/mochi_bench.cpp)
target_compile_definitions(mochi_bench PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(mochi_bench PRIVATE /utf-8)
target_link_libraries(mochi_bench PRIVATE d3d12 dxgi dxguid)

add_executable(mochi_controls src/mochi_controls.cpp)


add_executable(mochi_controls2 src/mochi_controls2.cpp)
target_link_libraries(mochi_controls2 PRIVATE d3d12 dxgi)


add_executable(mochi_controls3 src/mochi_controls3.cpp)
target_link_libraries(mochi_controls3 PRIVATE d3d12 dxgi)


add_executable(mochi_controls4 src/mochi_controls4.cpp)
target_link_libraries(mochi_controls4 PRIVATE d3d12 dxgi)


add_executable(mochi_abi_scan src/mochi_abi_scan.cpp)


add_executable(lmxxf_ext_probe src/lmxxf_ext_probe.cpp)


add_executable(lmxxf_ext_probe2 src/lmxxf_ext_probe2.cpp)
target_link_libraries(lmxxf_ext_probe2 PRIVATE d3d12 dxgi)


add_executable(lmxxf_frame_probe src/lmxxf_frame_probe.cpp)
target_link_libraries(lmxxf_frame_probe PRIVATE d3d12 dxgi)


add_executable(queue_overlap_probe src/queue_overlap_probe.cpp)
target_compile_definitions(queue_overlap_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(queue_overlap_probe PRIVATE d3d12 dxgi dxguid d3dcompiler)
target_compile_options(queue_overlap_probe PRIVATE /utf-8)



add_executable(bcard_headroom_probe src/bcard_headroom_probe.cpp)
target_compile_definitions(bcard_headroom_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(bcard_headroom_probe PRIVATE d3d12 dxgi dxguid d3dcompiler)
target_compile_options(bcard_headroom_probe PRIVATE /utf-8)


add_executable(pcie_bandwidth_probe src/pcie_bandwidth_probe.cpp)
target_compile_definitions(pcie_bandwidth_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(pcie_bandwidth_probe PRIVATE d3d12 dxgi dxguid)
target_compile_options(pcie_bandwidth_probe PRIVATE /utf-8)

