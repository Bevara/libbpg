cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBBPG_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_bpg.c
)

SET(LIBBPG_LIB
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libbpgdec.a
)

SET(LIBBPG_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libbpg
        "${LIBBPG_SRC}"
        "${LIBBPG_LIB}"
        []
        ""
        "${LIBBPG_INC}"
        ""
        "1")
