cmake_minimum_required(VERSION 3.24)

project(
    IEDSyncTogether
    VERSION 0.3.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CommonLibSSE REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES
        src/PCH.h
        src/main.cpp
        src/Config.cpp
        src/Config.h
        src/FormIdentity.cpp
        src/FormIdentity.h
        src/IEDBridge.cpp
        src/IEDBridge.h
        src/ProxyResolver.cpp
        src/ProxyResolver.h
        src/UdpTransport.cpp
        src/UdpTransport.h
        src/SyncService.cpp
        src/SyncService.h
        include/IEDSyncTogether/Interface.h
)

target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

target_include_directories(${PROJECT_NAME} PUBLIC include)

target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)

target_compile_definitions(${PROJECT_NAME}
    PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        IEDST_BUILD
        IEDST_VERSION_STRING="${PROJECT_VERSION}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32)
