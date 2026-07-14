#include "database/VodDatabase.h"

namespace hsvj {

VodDatabase::~VodDatabase() {
    shutdown();
}

void VodDatabase::shutdown() {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    if (vodQueueDb_) {
        sqlite3_close(vodQueueDb_);
        vodQueueDb_ = nullptr;
    }
}

} // 命名空间 hsvj
