#ifndef HSVJ_UTILS_MEDIA_ENCRYPTION_KEY_H
#define HSVJ_UTILS_MEDIA_ENCRYPTION_KEY_H

// Fixed CENC v1 key. Must match server scripts/video_encryption_v1.json.
// Keep v1 stable once media has been encrypted; rotation needs a new version.
namespace hsvj::encrypted_media {
inline constexpr char kMediaEncryptionKeyV1[] = "37e5510e3bd890c1842490e878eda2cc";
}
#endif
