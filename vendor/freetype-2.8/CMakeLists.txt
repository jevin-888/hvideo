cmake_minimum_required(VERSION 3.21)

project("freetype")


set(FREETYPE_PATH ./)

include_directories(${FREETYPE_PATH}/include)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_ANDROID")
add_definitions(-DFT2_BUILD_LIBRARY)

add_library( # Sets the name of the library.
        freetype

        # Sets the library as a shared library.
        STATIC

        # Provides a relative path to your source file(s).
        ${FREETYPE_PATH}/src/base/ftsystem.c
        ${FREETYPE_PATH}/src/base/ftinit.c
        ${FREETYPE_PATH}/src/base/ftdebug.c
        ${FREETYPE_PATH}/src/base/ftbase.c
        ${FREETYPE_PATH}/src/base/ftbbox.c
        ${FREETYPE_PATH}/src/base/ftglyph.c
        ${FREETYPE_PATH}/src/base/ftbdf.c
        ${FREETYPE_PATH}/src/base/ftbitmap.c
        ${FREETYPE_PATH}/src/base/ftcid.c
        ${FREETYPE_PATH}/src/base/ftfntfmt.c
        ${FREETYPE_PATH}/src/base/ftfstype.c
        ${FREETYPE_PATH}/src/base/ftgasp.c
        ${FREETYPE_PATH}/src/base/ftgxval.c
        ${FREETYPE_PATH}/src/base/ftlcdfil.c
        ${FREETYPE_PATH}/src/base/ftmm.c
        ${FREETYPE_PATH}/src/base/ftotval.c
        ${FREETYPE_PATH}/src/base/ftpatent.c
        ${FREETYPE_PATH}/src/base/ftpfr.c
        ${FREETYPE_PATH}/src/base/ftstroke.c
        ${FREETYPE_PATH}/src/base/ftsynth.c
        ${FREETYPE_PATH}/src/base/fttype1.c
        ${FREETYPE_PATH}/src/base/ftwinfnt.c
        ${FREETYPE_PATH}/src/bdf/bdf.c
        ${FREETYPE_PATH}/src/cff/cff.c
        ${FREETYPE_PATH}/src/cid/type1cid.c
        ${FREETYPE_PATH}/src/pcf/pcf.c
        ${FREETYPE_PATH}/src/pfr/pfr.c
        ${FREETYPE_PATH}/src/sfnt/sfnt.c
        ${FREETYPE_PATH}/src/truetype/truetype.c
        ${FREETYPE_PATH}/src/type1/type1.c
        ${FREETYPE_PATH}/src/type42/type42.c
        ${FREETYPE_PATH}/src/winfonts/winfnt.c
        ${FREETYPE_PATH}/src/raster/raster.c
        ${FREETYPE_PATH}/src/smooth/smooth.c
        ${FREETYPE_PATH}/src/autofit/autofit.c
        ${FREETYPE_PATH}/src/cache/ftcache.c
        ${FREETYPE_PATH}/src/gzip/ftgzip.c
        ${FREETYPE_PATH}/src/lzw/ftlzw.c
        ${FREETYPE_PATH}/src/bzip2/ftbzip2.c
        ${FREETYPE_PATH}/src/gxvalid/gxvalid.c
        ${FREETYPE_PATH}/src/otvalid/otvalid.c
        ${FREETYPE_PATH}/src/psaux/psaux.c
        ${FREETYPE_PATH}/src/pshinter/pshinter.c
        ${FREETYPE_PATH}/src/psnames/psnames.c)


find_library( # Sets the name of the path variable.
        log-lib
        log)

target_link_libraries(freetype
        -landroid
        ${log-lib}
        )
