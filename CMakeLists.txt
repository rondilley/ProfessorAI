cmake_minimum_required(VERSION 3.14)
project(professord VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)

# --- Options for local overrides ---
set(LOCAL_LLAMA_PATH "" CACHE PATH "Path to local llama.cpp source")
set(LOCAL_MONGOOSE_PATH "" CACHE PATH "Path to local mongoose source")
set(LOCAL_CJSON_PATH "" CACHE PATH "Path to local cJSON source")
option(PROF_BUILD_TESTS "Build tests" OFF)
option(PROF_USE_SYSTEMD "Enable sd_notify support" OFF)

# --- llama.cpp ---
if(LOCAL_LLAMA_PATH)
    add_subdirectory(${LOCAL_LLAMA_PATH} llama.cpp)
else()
    FetchContent_Declare(llama_cpp
        GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(llama_cpp)
endif()

# --- mongoose ---
if(LOCAL_MONGOOSE_PATH)
    add_library(mongoose STATIC ${LOCAL_MONGOOSE_PATH}/mongoose.c)
    target_include_directories(mongoose PUBLIC ${LOCAL_MONGOOSE_PATH})
else()
    FetchContent_Declare(mongoose
        GIT_REPOSITORY https://github.com/cesanta/mongoose.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(mongoose)
    add_library(mongoose_lib STATIC ${mongoose_SOURCE_DIR}/mongoose.c)
    target_include_directories(mongoose_lib PUBLIC ${mongoose_SOURCE_DIR})
    add_library(mongoose ALIAS mongoose_lib)
endif()

# --- cJSON ---
if(LOCAL_CJSON_PATH)
    add_library(cjson STATIC ${LOCAL_CJSON_PATH}/cJSON.c)
    target_include_directories(cjson PUBLIC ${LOCAL_CJSON_PATH})
else()
    FetchContent_Declare(cjson
        GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE)
    set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_AND_STATIC_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(cjson)
endif()

# --- Main executable ---
add_executable(professord
    src/main.c
    src/logger.c
    src/config.c
    src/daemon.c
    src/api_types.c
    src/inference.c
    src/worker.c
    src/server.c
    src/recommend.c
    src/stats.c)

target_include_directories(professord PRIVATE
    src
    ${llama_cpp_SOURCE_DIR}/include
    ${cjson_SOURCE_DIR})

target_compile_definitions(professord PRIVATE
    PROF_VERSION="${PROJECT_VERSION}"
    PROF_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    PROF_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PROF_VERSION_PATCH=${PROJECT_VERSION_PATCH})

target_link_libraries(professord PRIVATE
    llama ggml mongoose cjson pthread m)

if(PROF_USE_SYSTEMD)
    target_compile_definitions(professord PRIVATE PROF_USE_SYSTEMD=1)
    target_link_libraries(professord PRIVATE systemd)
endif()

install(TARGETS professord DESTINATION bin)

# --- Tests ---
if(PROF_BUILD_TESTS)
    enable_testing()

    add_executable(test_config tests/test_config.c src/config.c src/logger.c)
    target_include_directories(test_config PRIVATE src)
    target_link_libraries(test_config PRIVATE pthread)
    add_test(NAME test_config COMMAND test_config)

    add_executable(test_api_types tests/test_api_types.c src/api_types.c)
    target_include_directories(test_api_types PRIVATE src ${cjson_SOURCE_DIR})
    target_link_libraries(test_api_types PRIVATE cjson m)
    add_test(NAME test_api_types COMMAND test_api_types)
endif()
