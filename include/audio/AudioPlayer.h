/**
 * @file AudioPlayer.h（文件名）
 * @brief 闊抽鎾斁鍣ㄧ被瀹氫箟
 * 
 * 鏈枃浠跺畾涔変簡闊抽鎾斁鍣ㄧ被锛岃礋璐ｏ細
 * - 浣庡欢杩熼煶棰戞挱鏀撅紙浣跨敤AAudio API锛* - 闊抽鏁版嵁闃熷垪绠＄悊
 * - 闊抽噺鎺у埗鍜屾贰鍏ユ贰鍑* - 闊抽璁惧绠＄悊
 */

#ifndef HSVJ_AUDIO_PLAYER_H
#define HSVJ_AUDIO_PLAYER_H

#include <cstdint>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <memory>

#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#endif

namespace hsvj {

/**
 * @brief 闊抽鏁版嵁鍥炶皟鍑芥暟绫诲瀷
 * @param data PCM闊抽鏁版嵁锛坕nt16_t鏍煎紡锛* @param numFrames 甯ф暟
 * @param sampleRate 閲囨牱鐜* 
 * 鐢ㄤ簬闊抽鏁堟灉鑱斿姩绯荤粺锛屽皢鎾斁鐨勯煶棰戞暟鎹紶閫掔粰AudioProcessor杩涜鍒嗘瀽
 */
using AudioDataCallback = std::function<void(const int16_t* data, int32_t numFrames, int32_t sampleRate)>;


/**
 * @brief 闊抽澹伴亾妯″紡鏋氫妇
 */
enum class AudioChannelMode {
    STEREO,  // 绔嬩綋澹帮紙榛樿锛?
    LEFT,    // 鍙挱鏀惧乏澹伴亾锛堝鍒跺埌鍙屽０閬擄級
    RIGHT    // 鍙挱鏀惧彸澹伴亾锛堝鍒跺埌鍙屽０閬擄級
};

/**
 * @brief 闊抽鎾斁鍣ㄧ被
 * 
 * 浣跨敤AAudio API杩涜浣庡欢杩熼煶棰戞挱鏀撅紝鏀寔闊抽噺鎺у埗鍜屾贰鍏ユ贰鍑*/
class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();
    
    /**
     * @brief 璁剧疆闊抽澹伴亾妯″紡
     * @param mode 澹伴亾妯″紡锛圫TEREO, LEFT, RIGHT锛*/
    void setAudioChannelMode(AudioChannelMode mode);
    
    /**
     * @brief 鑾峰彇褰撳墠闊抽澹伴亾妯″紡
     * @return 褰撳墠澹伴亾妯″紡
     */
    AudioChannelMode getAudioChannelMode() const { return channelMode_; }

    /**
     * @brief 鍒濆鍖栭煶棰戞挱鏀惧櫒
     * @param sampleRate 閲囨牱鐜囷紙濡?4100, 48000锛* @param channelCount 澹伴亾鏁帮紙1=鍗曞０閬? 2=绔嬩綋澹帮級
     * @param format 閲囨牱鏍煎紡锛堥粯璁?6浣峆CM锛* @param deviceId 闊抽璁惧ID锛圓AUDIO_UNSPECIFIED琛ㄧず浣跨敤榛樿璁惧锛* @return 鏄惁鍒濆鍖栨垚鍔*/
    bool initialize(int32_t sampleRate, int32_t channelCount, 
                    int32_t format = AAUDIO_FORMAT_PCM_I16,
                    int32_t deviceId = AAUDIO_UNSPECIFIED);

    int32_t getSampleRate() const {
#ifdef __ANDROID__
        return sampleRate_;
#else
        return 0;
#endif
    }

    int32_t getChannelCount() const {
#ifdef __ANDROID__
        return channelCount_;
#else
        return 0;
#endif
    }

    int32_t getFormat() const {
#ifdef __ANDROID__
        return format_;
#else
        return 0;
#endif
    }

    int32_t getDeviceId() const {
#ifdef __ANDROID__
        return deviceId_;
#else
        return 0;
#endif
    }
    
    /**
     * @brief 鍏抽棴闊抽鎾斁鍣紝閲婃斁璧勬簮
     */
    void shutdown();
    
    /**
     * @brief 寮€濮嬫挱鏀* @return 鏄惁寮€濮嬫垚鍔*/
    bool start();
    
    /**
     * @brief 鍋滄鎾斁
     */
    void stop();
    
    /**
     * @brief 鏆傚仠鎾斁
     */
    void pause();
    
    /**
     * @brief 鎭㈠鎾斁
     */
    void resume();
    
    /**
     * @brief 娓呯┖闊抽闃熷垪锛堢敤浜巗eek/replay鏃舵竻闄ゆ畫鐣欐暟鎹級
     */
    void flush();

    /**
     * @brief 设置 write() 到 AAudio 回调之间允许缓存的队列块数。
     *        USB 镜像使用较小队列以降低音画延迟，本地视频可恢复默认值。
     */
    void setQueueLimit(size_t maxItems);

    /**
     * @brief 设置队列满时 write() 等待消费端腾出空间的最长时间。
     *        本地视频/纯音频可等待更久以避免丢音频块，实时镜像保持较短等待。
     */
    void setQueueBackpressureWaitMs(int32_t waitMs);

    /**
     * @brief 按 AAudio burst 数调整底层流 buffer size。
     *        返回 true 表示请求已被 AAudio 接受。
     */
    bool setBufferSizeInBursts(int32_t burstCount);

    /**
     * @brief 设置 flush() 后首包自动淡入时长。
     */
    void setAutoFadeInDurationMs(int32_t durationMs);
    
    /**
     * @brief 鍐欏叆闊抽鏁版嵁
     * @param data PCM闊抽鏁版嵁
     * @param numFrames 甯ф暟
     * @return 瀹為檯鍐欏叆鐨勫抚鏁*/
    int32_t write(const void* data, int32_t numFrames);
    
    /**
     * @brief 璁剧疆闊抽噺
     * @param volume 闊抽噺锛?.0-1.0锛*/
    void setVolume(float volume);

    /**
     * @brief 更新目标音量但保留当前淡入/淡出包络。
     *
     * setVolume() 是硬切音量；此接口用于 flush() 已把 currentVolume_
     * 归零并武装自动淡入后，只更新淡入目标，避免首包被重新拉到满幅。
     */
    void setTargetVolume(float volume);
    
    /**
     * @brief 鑾峰彇闊抽噺
     * @return 闊抽噺锛?.0-1.0锛*/
    float getVolume() const { return volume_; }
    
    /**
     * @brief 骞虫粦杩囨浮闊抽噺锛堟贰鍏ユ贰鍑猴級
     * @param targetVolume 鐩爣闊抽噺
     * @param durationMs 杩囨浮鏃堕暱锛堟绉掞紝榛樿100锛*/
    void fadeToVolume(float targetVolume, int32_t durationMs = 100);
    
    /**
     * @brief 娣″嚭锛堢敤浜庨煶杞?澹伴亾鍒囨崲鍓嶏級
     * @param durationMs 娣″嚭鏃堕暱锛堟绉掞紝榛樿80锛*/
    void fadeOut(int32_t durationMs = 80);
    
    /**
     * @brief 娣″叆锛堢敤浜庨煶杞?澹伴亾鍒囨崲鍚庯級
     * @param durationMs 娣″叆鏃堕暱锛堟绉掞紝榛樿80锛*/
    void fadeIn(int32_t durationMs = 80);
    
    /**
     * @brief 妫€鏌ユ贰鍏ユ贰鍑烘槸鍚﹀畬鎴* @return 鏄惁瀹屾垚
     */
    bool isFadeComplete() const { return fadeComplete_; }
    
    /**
     * @brief 绛夊緟娣″叆娣″嚭瀹屾垚
     */
    void waitForFadeComplete();
    
    /**
     * @brief 鑾峰彇鎾斁鐘舵€* @return 鏄惁姝ｅ湪鎾斁
     */
    bool isPlaying() const { return playing_; }
    
    /**
     * @brief 鑾峰彇缂撳啿鍖哄ぇ灏忥紙甯ф暟锛* @return 缂撳啿鍖哄ぇ灏忥紙甯ф暟锛*/
    int32_t getBufferSizeInFrames() const;
    
    /**
     * @brief 鑾峰彇闃熷垪涓緟鎾斁鐨勫抚鏁* @return 寰呮挱鏀惧抚鏁*/
    int32_t getPendingFrames() const;

    /**
     * @brief 获取 AAudio 已写入但尚未真正播出的总帧数（含 AAudio 内部缓冲 + HAL 链路）。
     *        使用 AAudioStream_getFramesWritten - AAudioStream_getFramesRead 计算，
     *        是估算"从现在写入到真正出声"延迟最准确的口径。
     * @return AAudio 流上 in-flight 帧数；流未启动或不可用返回 0。
     */
    int32_t getAAudioInFlightFrames() const;

    /**
     * @brief 通过 AAudioStream_getTimestamp() 获取硬件呈现时间戳。
     *        返回的 (framePosition, 时间Nanos) 含义为：framePosition 帧将在 时间Nanos
     *        (CLOCK_MONOTONIC 纳秒) 时刻从扬声器真正发声。可消除 HAL/DSP 输出延迟。
     * @return 取到合法时间戳返回 true；流未启动或抖动期返回 false（调用方应回退）。
     */
    bool getAAudioPresentationTimestamp(int64_t &framePosition,
                                        int64_t &timeNanos) const;

    /**
     * @brief AAudio 已经真正播出的总帧数（AAudioStream_getFramesRead）。
     *        用于判定 AAudio 是否已开始消费数据（>0 表示出声链路就绪）。
     * @return 已读出的总帧数；流未启动或不可用返回 0。
     */
    int64_t getAAudioFramesRead() const;

    /**
     * @brief AAudio 已写入的总帧数（AAudioStream_getFramesWritten）。
     *        与 getAAudioFramesRead 配对用于精确测算"本次起播以来"的播放进度。
     * @return 已写入的总帧数；流未启动或不可用返回 0。
     */
    int64_t getAAudioFramesWritten() const;

    /**
     * @brief 估算 AAudio 输出链路总延迟（写入 → 真正出声），单位秒。
     *        优先用 AAudioStream_getTimestamp() 计算 (framesWritten - hwFramePos)/sampleRate；
     *        硬件时间戳不可用时回退 (framesWritten - framesRead)/sampleRate + 固定 HDMI 余量。
     *        内部用 EMA 平滑（alpha=0.1）以吸收瞬时抖动。
     *        所有图层共享同一物理 AAudio 流，此 delay 全局复用。
     * @return 估算延迟（秒）；流未启动返回 0。
     */
    double estimatedOutputDelaySeconds() const;

    /**
     * @brief 璁剧疆闊抽鏁版嵁鍥炶皟锛堢敤浜庨煶棰戞晥鏋滆仈鍔級級
     * @param callback 鍥炶皟鍑芥暟锛屾帴鏀禤CM鏁版嵁鐢ㄤ簬棰戣氨鍒嗘瀽
     * 
     * 鍦╳rite()鏂规硶涓紝闊抽鏁版嵁浼氫紶閫掔粰姝ゅ洖璋冿紝鐢ㄤ簬闊抽鏁堟灉鑱斿姩绯荤粺銆* 鍙互灏嗘鍥炶皟杩炴帴鍒癆udioProcessor杩涜FFT鍒嗘瀽銆*/
    void setAudioDataCallback(AudioDataCallback callback);
    
    /**
     * @brief AAudio閿欒鍥炶皟鍑芥暟锛堥潤鎬侊級
     * @param stream AAudio娴* @param userData 鐢ㄦ埛鏁版嵁
     * @param error 閿欒鐮*/
    static void errorCallback(
        AAudioStream* stream,
        void* userData,
        aaudio_result_t error);
    
    /**
     * @brief AAudio鏁版嵁鍥炶皟鍑芥暟锛堥潤鎬侊級
     * @param stream AAudio娴* @param userData 鐢ㄦ埛鏁版嵁
     * @param audioData 闊抽鏁版嵁缂撳啿鍖* @param numFrames 甯ф暟
     * @return 鍥炶皟缁撴灉
     */
    static aaudio_data_callback_result_t dataCallback(
        AAudioStream* stream,
        void* userData,
        void* audioData,
        int32_t numFrames);
    
    /**
     * @brief 妫€鏌ユ槸鍚﹂渶瑕侀噸鏂板垵濮嬪寲
     * @return 鏄惁闇€瑕侀噸鏂板垵濮嬪寲
     */
    bool needsReinit() const { return needsReinit_.load(std::memory_order_acquire); }
    
    /**
     * @brief 娓呴櫎閲嶆柊鍒濆鍖栨爣蹇*/
    void clearReinitFlag() { needsReinit_.store(false, std::memory_order_release); }

    void markNeedsReinit(const char* reason);

private:
#ifdef __ANDROID__
    /**
     * @brief 闊抽鏁版嵁闃熷垪椤圭粨鏋*/
    struct AudioQueueItem {
        int16_t* data;              // 闊抽鏁版嵁鎸囬拡
    int32_t numFrames;          // 甯ф暟
    int32_t usedSamples;        // 宸蹭娇鐢ㄧ殑鏍锋湰鏁帮紙鐢ㄤ簬閮ㄥ垎浣跨敤鐨勬儏鍐碉級
        
        /**
         * @brief 鏋勯€犲嚱鏁* @param d 闊抽鏁版嵁鎸囬拡
         * @param frames 甯ф暟
         */
        AudioQueueItem(int16_t* d, int32_t frames) 
            : data(d), numFrames(frames), usedSamples(0) {}
        
        /**
         * @brief 鑾峰彇鍓╀綑鏍锋湰鏁* @param channelCount 澹伴亾鏁* @return 鍓╀綑鏍锋湰鏁*/
        int32_t getRemainingSamples(int32_t channelCount) const {
            return (numFrames * channelCount) - usedSamples;
        }
    };
    
    AAudioStream* stream_;                          // AAudio娴?
    std::queue<AudioQueueItem> audioQueue_;         // 闊抽鏁版嵁闃熷垪
    std::mutex queueMutex_;                         // 闃熷垪浜掓枼閿?
    std::condition_variable queueCondition_;        // 闃熷垪鏉′欢鍙橀噺
    std::atomic<int32_t> maxQueueItems_{8};
    std::atomic<int32_t> queueBackpressureWaitMs_{5};
    int32_t sampleRate_;                            // 閲囨牱鐜?
    int32_t channelCount_;                          // 澹伴亾鏁?
    int32_t format_;                                // 閲囨牱鏍煎紡
    int32_t bytesPerFrame_;                         // 姣忓抚瀛楄妭鏁?
    int32_t deviceId_;                              // 璁惧ID
    std::atomic<bool> playing_;                     // 鏄惁姝ｅ湪鎾斁
    std::atomic<bool> shuttingDown_{false};         // 姝ｅ湪鍏抽棴鏃跺洖璋冨彧杈撳嚭闈欓煶锛岄伩鍏嶆竻闃熷垪瀵艰嚧 use-after-free
    std::atomic<bool> needsReinit_{false};          // 鏄惁闇€瑕侀噸鏂板垵濮嬪寲锛圓udioFlinger宕╂簝鍚庯級
    // EMA 平滑后的输出链路延迟（纳秒），仅由 estimatedOutputDelaySeconds() 维护
    mutable std::atomic<int64_t> smoothedOutputDelayNs_{0};
    // flush() 时置位，下一次 write() 成功入队时自动启动淡入，防止切歌首包爆音
    std::atomic<bool> pendingFadeInAfterFlush_{false};
    std::atomic<int32_t> pendingFadeInDurationMs_{120};
    std::atomic<float> volume_;                     // 闊抽噺
    
    // 娣″叆娣″嚭鐩稿叧
    std::atomic<float> targetVolume_;               // 鐩爣闊抽噺
    std::atomic<float> currentVolume_;              // 褰撳墠瀹為檯闊抽噺锛堢敤浜庡钩婊戣繃娓★級
    std::atomic<float> volumeStep_;                  // 姣忓抚闊抽噺鍙樺寲姝ラ暱
    std::atomic<bool> fadeComplete_;                // 娣″叆娣″嚭鏄惁瀹屾垚
    float volumeBeforeFade_;                        // 娣″叆娣″嚭鍓嶇殑闊抽噺锛堢敤浜庢仮澶嶏級
    
    // 澹伴亾妯″紡
    std::atomic<AudioChannelMode> channelMode_;     // 褰撳墠澹伴亾妯″紡锛圫TEREO, LEFT, RIGHT锛// 闊抽鏁版嵁鍥炶皟锛堢敤浜庨煶棰戞晥鏋滆仈鍔級
    AudioDataCallback audioDataCallback_;           // 闊抽鏁版嵁鍥炶皟鍑芥暟
    std::mutex audioCallbackMutex_;                 // 鍥炶皟浜掓枼閿// 鍏ㄥ眬鍐欏叆浜掓枼閿侊紙淇濇姢 write() 鏂规硶锛岄伩鍏嶅弻瑙ｇ爜鍣ㄥ悓鏃跺啓鍏ワ級
    std::mutex writeMutex_;                         // 鍐欏叆浜掓枼閿
    
    /**
     * @brief 浠庨槦鍒椾腑璇诲彇鏁版嵁骞跺啓鍏Audio娴* @param audioData 闊抽鏁版嵁缂撳啿鍖* @param numFrames 甯ф暟
     * @return 鍥炶皟缁撴灉
     */
    aaudio_data_callback_result_t onAudioReady(void* audioData, int32_t numFrames);
#endif
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_AUDIO_PLAYER_H
