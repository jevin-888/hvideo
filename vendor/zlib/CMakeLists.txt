
project("libZ")


set(ZLIB_PATH ./)

include_directories(${OPENTHREADS_PATH}/include ${OPENTHREADS_PATH}/src/OpenThreads)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_ANDROID")

add_library( # Sets the name of the library.
		libZ

		# Sets the library as a shared library.
		STATIC

		# Provides a relative path to your source file(s).
		${ZLIB_PATH}/adler32.c
		${ZLIB_PATH}/compress.c
		${ZLIB_PATH}/crc32.c
		${ZLIB_PATH}/deflate.c
		${ZLIB_PATH}/gzclose.c
		${ZLIB_PATH}/gzlib.c
		${ZLIB_PATH}/gzread.c
		${ZLIB_PATH}/gzwrite.c
		${ZLIB_PATH}/infback.c
		${ZLIB_PATH}/inffast.c
		${ZLIB_PATH}/inflate.c
		${ZLIB_PATH}/inftrees.c
		${ZLIB_PATH}/trees.c
		${ZLIB_PATH}/uncompr.c
		${ZLIB_PATH}/zutil.c
		)


find_library( # Sets the name of the path variable.
        log-lib
        log)

target_link_libraries(libZ
		-landroid
		${log-lib}
)
