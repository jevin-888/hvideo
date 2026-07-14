/**
 * @file pthread_stub.c（文件名）
 * @brief 说明：用于满足 Android 上 librockit.so 依赖的 libpthread.so.0 占位库
 *
 * Android libc 已包含 pthread 实现，但桌面 Linux 库
 * 说明：经常显式依赖 libpthread.so.0，该占位库用于允许加载
 * 说明：这类库。
 */

// 说明：如果符号已由 libc 解析，这里无需实现任何逻辑。
// 说明：但仍定义一些常见符号以防万一。

void pthread_create() {}
void pthread_join() {}
void pthread_mutex_init() {}
void pthread_mutex_lock() {}
void pthread_mutex_unlock() {}
void pthread_cond_wait() {}
void pthread_cond_signal() {}
