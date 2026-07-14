package com.hsvj.engine;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.ImageFormat;
import android.hardware.HardwareBuffer;
import android.media.Image;
import android.media.ImageReader;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Surface;

import com.huoshan.mirror.impl.LympServer;
import com.huoshan.mirror.base.IBaseServer;
import com.huoshan.mirror.base.INotifyListener;

import java.util.concurrent.atomic.AtomicBoolean;
import java.util.Random;

/**
 * 投屏管理器 - 纯净数据源版
 * 仅负责 Lymp SDK 的运行和 HardwareBuffer 的推送
 * 所有的渲染、切片、同步逻辑均由 C++ 层的 LayerMirror (LayerVideo 子类) 负责
 */
public class MirrorManager {
    private static final String TAG = "MirrorManager";
    private static final long HARDWARE_BUFFER_USAGE_COMPOSER_OVERLAY = 1L << 11;
    
    private final Context mContext;
    private final int mLayerId;
    
    private IBaseServer mServer;
    private ImageReader mImageReader;
    private Surface mSurface;
    private int mReaderWidth;
    private int mReaderHeight;
    
    private HandlerThread mHandlerThread;
    private Handler mHandler;
    
    private final AtomicBoolean mIsStarted = new AtomicBoolean(false);
    private final AtomicBoolean mAppleServerStarted = new AtomicBoolean(false);
    private final AtomicBoolean mIsConnected = new AtomicBoolean(false);
    private volatile long mConnectedIp = 0;
    private volatile int mPinCode = 8888;
    private volatile String mDeviceName = "";

    public MirrorManager(Context context, int layerId) {
        mContext = context;
        mLayerId = layerId;
    }

    public void start(String deviceName, int pinCode) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) {
            android.util.Log.w(TAG, "Mirror requires Android 9+ HardwareBuffer support");
            HSVJEngine.setMirroringState(false);
            return;
        }
        if (mIsStarted.get()) return;
        mDeviceName = deviceName;
        mPinCode = pinCode;
        
        mHandlerThread = new HandlerThread("MirrorFrameThread");
        mHandlerThread.start();
        mHandler = new Handler(mHandlerThread.getLooper());

        // [核心策略] 始终使用 1080p PRIVATE(0x22) 格式，作为 C++ 引擎的稳定视频源
        recreateImageReader(1920, 1080);

        mServer = new LympServer(mContext);
        mServer.setDeviceName(deviceName);
        mServer.setPinCode(pinCode);
        mServer.setSurface(mSurface);
        
        // 同步 PIN 码等元数据到 C++ 层
        HSVJEngine.setMirrorPin(mLayerId, pinCode);
        
        mServer.setNotifyListener(new INotifyListener() {
            @Override
            public void onConnectedDevice(long ip) {
                android.util.Log.i(TAG, "Mirror Connected: " + ip);
                mConnectedIp = ip;
                mIsConnected.set(true);
            }

            @Override
            public void onDisconnectedDevice(long ip) {
                android.util.Log.i(TAG, "Mirror Disconnected: " + ip);
                if (mConnectedIp == ip) {
                    mConnectedIp = 0;
                    mIsConnected.set(false);
                }
            }

            @Override
            public void onResolutionChanged(long ip, int width, int height) {
                android.util.Log.i(TAG, "Source resolution change: " + width + "x" + height);
                if (mIsStarted.get()) {
                    HSVJEngine.updateMirrorSourceInfo(mLayerId, 0, 0, width, height);
                }
            }
        });

        mServer.startServer();
        mAppleServerStarted.set(true);
        mIsStarted.set(true);
        HSVJEngine.setMirroringState(true);
        android.util.Log.i(TAG, "Mirror Data Feeder started successfully");
    }

    public Surface prepareFrameSurface() {
        return prepareFrameSurface(1920, 1080);
    }

    public Surface prepareFrameSurface(int width, int height) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) {
            android.util.Log.w(TAG, "Mirror frame surface requires Android 9+ HardwareBuffer support");
            return null;
        }
        width = Math.max(16, width);
        height = Math.max(16, height);
        if (mImageReader != null && mSurface != null
                && mReaderWidth == width && mReaderHeight == height) {
            return mSurface;
        }
        if (mHandlerThread == null) {
            mHandlerThread = new HandlerThread("MirrorFrameThread");
            mHandlerThread.start();
            mHandler = new Handler(mHandlerThread.getLooper());
        }
        recreateImageReader(width, height);
        return mSurface;
    }

    public void startAndroidSurfaceSink() {
        if (mIsStarted.get()) return;
        mIsStarted.set(true);
        HSVJEngine.setMirroringState(true);
    }

    @SuppressLint("WrongConstant")
    private void recreateImageReader(int width, int height) {
        if (mImageReader != null) {
            mImageReader.close();
        }
        mReaderWidth = width;
        mReaderHeight = height;
        
        // 格式锁定为 PRIVATE，这是投屏 Surface 到 C++ 外部视频源的稳定路径。
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            long usage = HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE
                    | HardwareBuffer.USAGE_GPU_COLOR_OUTPUT
                    | HARDWARE_BUFFER_USAGE_COMPOSER_OVERLAY;
            mImageReader = ImageReader.newInstance(width, height, ImageFormat.PRIVATE, 3, usage);
        } else {
            mImageReader = ImageReader.newInstance(width, height, ImageFormat.PRIVATE, 3);
        }

        mImageReader.setOnImageAvailableListener(reader -> {
            Image image = null;
            try {
                image = reader.acquireLatestImage();
                if (image != null) {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                        HardwareBuffer buffer = image.getHardwareBuffer();
                        if (buffer != null && mIsStarted.get()) {
                            // HardwareBuffer 可能被解码器按 stride 对齐，visible 尺寸来自 ImageReader。
                            HSVJEngine.pushMirrorFrame(mLayerId, buffer,
                                    buffer.getWidth(), buffer.getHeight(),
                                    image.getWidth(), image.getHeight());
                        }
                    }
                    image.close();
                }
            } catch (Exception e) {
                if (image != null) image.close();
            }
        }, mHandler);

        mSurface = mImageReader.getSurface();
        android.util.Log.i(TAG, "Mirror frame surface ready: " + width + "x" + height);
    }

    public void stop() {
        if (!mIsStarted.get() && mServer == null && mImageReader == null && mHandlerThread == null) return;
        
        mIsStarted.set(false);
        if (mServer != null) {
            mServer.stopServer();
            mServer = null;
        }
        mAppleServerStarted.set(false);
        
        if (mImageReader != null) {
            mImageReader.close();
            mImageReader = null;
        }
        mSurface = null;
        mReaderWidth = 0;
        mReaderHeight = 0;
        
        if (mHandlerThread != null) {
            mHandlerThread.quitSafely();
            mHandlerThread = null;
        }
        
        HSVJEngine.setMirroringState(false);
        mConnectedIp = 0;
        mIsConnected.set(false);
    }
    
    public boolean isStarted() {
        return mIsStarted.get();
    }

    public boolean isAppleServerStarted() {
        return mAppleServerStarted.get();
    }

    public boolean isConnected() {
        return mIsConnected.get();
    }

    public int getPinCode() {
        return mPinCode;
    }

    public String getDeviceName() {
        return mDeviceName;
    }

    public long getConnectedIp() {
        return mConnectedIp;
    }

    public int resetPinCode() {
        mPinCode = 1000 + new Random().nextInt(9000);
        if (mServer != null) {
            mServer.setPinCode(mPinCode);
        }
        HSVJEngine.setMirrorPin(mLayerId, mPinCode);
        return mPinCode;
    }
}
