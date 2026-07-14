set(DATABASE_SOURCES
    PlaylistManager.cpp
    PlaylistDatabase.cpp
    PlaylistDatabase_Create.cpp
    PlaylistDatabase_Query.cpp
    PlaylistDatabase_Update.cpp
    PlaylistDatabase_State.cpp
    MaterialIndex.cpp
    VodDatabase.cpp
    VodDatabase_Queue.cpp
)

# 获取项目根目录（相对于当前CMakeLists.txt的位置）
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_library(hsvj_database STATIC ${DATABASE_SOURCES})

target_include_directories(hsvj_database PUBLIC
    ${PROJECT_ROOT}/include
)

# 使用 SQLite3 Multiple Ciphers（内置加密，无需外部 OpenSSL）
set(SQLITE3MC_DIR "${PROJECT_ROOT}/vendor/sqlite3mc")

if(USE_SQLCIPHER AND EXISTS "${SQLITE3MC_DIR}/sqlite3mc_amalgamation.c")
    message(STATUS "✓ Using SQLite3 Multiple Ciphers for encrypted database support")
    
    target_sources(hsvj_database PRIVATE
        ${SQLITE3MC_DIR}/sqlite3mc_amalgamation.c
    )
    target_include_directories(hsvj_database PRIVATE
        ${SQLITE3MC_DIR}
    )
    
    # SQLite3 Multiple Ciphers 定义
    target_compile_definitions(hsvj_database PRIVATE
        SQLITE_HAS_CODEC
        CODEC_TYPE=CODEC_TYPE_SQLCIPHER  # SQLCipher 兼容模式
        SQLITE_ENABLE_FTS5               # 全文搜索（FTS）
        SQLITE_TEMP_STORE=2
        SQLITE_THREADSAFE=1
    )
    
    # ARM64 需要启用 Crypto 扩展以支持硬件 AES
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        target_compile_options(hsvj_database PRIVATE -march=armv8-a+crypto)
    endif()
    
    message(STATUS "✓ SQLCipher-compatible encryption enabled for SQLite databases")
else()
    # 使用标准 SQLite（无加密）
    message(STATUS "Using standard SQLite (no encryption)")
    target_sources(hsvj_database PRIVATE
        ${PROJECT_ROOT}/vendor/sqlite/sqlite3.c
    )
    target_include_directories(hsvj_database PRIVATE
        ${PROJECT_ROOT}/vendor/sqlite
    )
endif()

target_link_libraries(hsvj_database
    hsvj_utils
    $<$<BOOL:${ANDROID}>:android>
)


