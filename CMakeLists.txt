cmake_minimum_required(VERSION 3.20)
project(GameFrameTranslator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(XCOMPOSITE REQUIRED IMPORTED_TARGET xcomposite)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
pkg_check_modules(PANGO REQUIRED IMPORTED_TARGET pango)
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo)

set(GFT_CORE_SOURCES
    src/common/log.cpp
    src/common/config.cpp
    src/common/metrics.cpp
    src/common/thread_pool.cpp
    src/cache/sqlite_cache.cpp
    src/analyzer/analyzer.cpp
    src/ocr/ocr_engine.cpp
    src/translate/translator.cpp
    src/capture/frame_source.cpp
    src/capture/x11_capture.cpp
    src/capture/synthetic_capture.cpp
    src/overlay/overlay.cpp
    src/pipeline/pipeline.cpp
)

add_library(gft_core STATIC ${GFT_CORE_SOURCES})
target_include_directories(gft_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(gft_core PUBLIC
    Threads::Threads
    PkgConfig::X11
    PkgConfig::XCOMPOSITE
    PkgConfig::SQLITE3
    PkgConfig::CURL
    PkgConfig::JSONC
    PkgConfig::GTK3
    PkgConfig::CAIRO
    PkgConfig::PANGO
    PkgConfig::PANGOCAIRO
)
target_compile_options(gft_core PRIVATE -O3 -march=native -Wall -Wextra -Wpedantic)

add_executable(gft src/app/main.cpp)
target_link_libraries(gft PRIVATE gft_core)

# --- Testler ---
enable_testing()

function(gft_add_test name src)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE gft_core)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

gft_add_test(test_cache tests/test_cache.cpp)
gft_add_test(test_analyzer tests/test_analyzer.cpp)
gft_add_test(test_translate tests/test_translate.cpp)

# --- İsteğe bağlı Vulkan layer (Faz 2 iskeleti) ---
option(GFT_BUILD_VULKAN_LAYER "Vulkan layer ve manifest derle" OFF)
if(GFT_BUILD_VULKAN_LAYER)
    find_package(Vulkan)
    if(Vulkan_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/layer/gft_layer.cpp")
        add_library(gft_vulkan_layer SHARED layer/gft_layer.cpp)
        target_include_directories(gft_vulkan_layer PRIVATE ${Vulkan_INCLUDE_DIRS})
        target_link_libraries(gft_vulkan_layer PRIVATE ${Vulkan_LIBRARIES})
        set_target_properties(gft_vulkan_layer PROPERTIES PREFIX "lib" SUFFIX ".so")
    else()
        message(WARNING "Vulkan headers bulunamadı; layer derlenmedi")
    endif()
endif()
