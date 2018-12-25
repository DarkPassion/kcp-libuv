cmake_minimum_required(VERSION 3.8)

project(new-kcp)

SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++0x")

include_directories(
        deps/libuv/include/
        deps/libuv/src/)

#link_directories(deps/libuv/libs)


#set(CMAKE_C_FLAGS  "${CMAKE_C_FLAGS}")
#set(LIBUV_A deps/libuv/libs/libuv_a.a)

set(uv_dir deps/libuv/src)
set(uv_sources
        ${uv_dir}/fs-poll.c
        ${uv_dir}/inet.c
        ${uv_dir}/threadpool.c
        ${uv_dir}/timer.c
        ${uv_dir}/uv-common.c
        ${uv_dir}/uv-data-getter-setters.c
        ${uv_dir}/version.c)

list(APPEND uv_sources
        ${uv_dir}/unix/async.c
        ${uv_dir}/unix/core.c
        ${uv_dir}/unix/dl.c
        ${uv_dir}/unix/fs.c
        ${uv_dir}/unix/getaddrinfo.c
        ${uv_dir}/unix/getnameinfo.c
        ${uv_dir}/unix/loop-watcher.c
        ${uv_dir}/unix/loop.c
        ${uv_dir}/unix/pipe.c
        ${uv_dir}/unix/poll.c
        ${uv_dir}/unix/process.c
        ${uv_dir}/unix/signal.c
        ${uv_dir}/unix/stream.c
        ${uv_dir}/unix/tcp.c
        ${uv_dir}/unix/thread.c
        ${uv_dir}/unix/tty.c
        ${uv_dir}/unix/udp.c)

if(CMAKE_SYSTEM_NAME MATCHES "Android|Darwin|Linux|OS/390")
    list(APPEND uv_sources ${uv_dir}/unix/proctitle.c)
endif()


if(CMAKE_SYSTEM_NAME MATCHES "Darwin|DragonFly|FreeBSD|NetBSD|OpenBSD")
    list(APPEND uv_sources ${uv_dir}/unix/bsd-ifaddrs.c ${uv_dir}/unix/kqueue.c)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND uv_defines _DARWIN_UNLIMITED_SELECT=1 _DARWIN_USE_64_BIT_INODE=1)
    list(APPEND uv_sources
            ${uv_dir}/unix/darwin-proctitle.c
            ${uv_dir}/unix/darwin.c
            ${uv_dir}/unix/fsevents.c)
endif()

#file(GLOB LMY_SRC src/*.c)
#set(LMY_SRC src/luv.c)
#file(GLOB LUA_SRC deps/lua/*.c)

file(GLOB C_SRC src/*.c)
file(GLOB CPP_SRC src/*.cpp src/*.cc)

add_executable(new-kcp ${C_SRC} ${CPP_SRC} ${uv_sources})


#target_link_libraries(lua-uv uv_a)




