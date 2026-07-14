
project("libPng")


set(LIBPNG_PATH ./)


set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_ANDROID")

add_library( # Sets the name of the library.
		libPng

		# Sets the library as a shared library.
		STATIC

		# Provides a relative path to your source file(s).
		${LIBPNG_PATH}/png.c
		${LIBPNG_PATH}/pngerror.c
		${LIBPNG_PATH}/pngget.c
		${LIBPNG_PATH}/pngmem.c
		${LIBPNG_PATH}/pngpread.c
		${LIBPNG_PATH}/pngread.c
		${LIBPNG_PATH}/pngrio.c
		${LIBPNG_PATH}/pngrtran.c
		${LIBPNG_PATH}/pngrutil.c
		${LIBPNG_PATH}/pngset.c
		${LIBPNG_PATH}/pngtrans.c
		${LIBPNG_PATH}/pngwio.c
		${LIBPNG_PATH}/pngwrite.c
		${LIBPNG_PATH}/pngwtran.c
		${LIBPNG_PATH}/pngwutil.c
		)


find_library( # Sets the name of the path variable.
        log-lib
        log)

target_link_libraries(libPng
		-landroid
		${log-lib}
)
