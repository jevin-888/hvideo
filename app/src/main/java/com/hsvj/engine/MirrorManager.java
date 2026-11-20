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
import com.hsvj.engine.airplay.AirPlayPlayerInjector;
import com.hsvj.engine.airplay.AirPlayRkmppPlayer;

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
    private static final String SERVICE_APPLE = "苹果";
    private static final String SERVICE_ANDROID = "安卓";
    
    private final Context mContext;
    private final int mLayerId;
    
    private IBaseServer mServer;
    private ImageReader mImageReader;
    private Surface mSurface;
    private int mReaderWidth;
    private int mReaderHeight;
    // Decoder buffers may be stride-aligned (for example 498 -> 512). These
    // fields describe only the valid source picture, never the layer size.
    private volatile int mSourceWidth;
    private volatile int mSourceHeight;
    private int mLastLoggedBufferWidth;
    private int mLastLoggedBufferHeight;
    private int mLastLoggedVisibleWidth;
    private int mLastLoggedVisibleHeight;
    
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
        if (mIsStarted.get()) return;
        mDeviceName = deviceName;
        mPinCode = pinCode;
        resetSourceGeometry();

        LympServer lympServer = new LympServer(mContext);
        AirPlayRkmppPlayer player = new AirPlayRkmppPlayer(mLayerId);
        if (!AirPlayPlayerInjector.inject(lympServer, player)) {
            player.releasePlayer();
            android.util.Log.e(TAG, "AirPlay service not started: RKMPP player injection failed");
            HSVJEngine.setMirroringState(false);
            syncServiceState("", false, false);
            return;
        }

        lympServer.setDeviceName(deviceName);
        lympServer.setPinCode(pinCode);
        lympServer.setNotifyListener(new INotifyListener() {
            @Override
            public void onConnectedDevice(long ip) {
                android.util.Log.i(TAG, "Mirror Connected: " + ip);
                mConnectedIp = ip;
                mIsConnected.set(true);
                syncServiceState(SERVICE_APPLE, true, true);
            }

            @Override
            public void onDisconnectedDevice(long ip) {
                android.util.Log.i(TAG, "Mirror Disconnected: " + ip);
                if (mConnectedIp == ip) {
                    mConnectedIp = 0;
                    mIsConnected.set(false);
                    resetSourceGeometry();
                    syncServiceState(SERVICE_APPLE, true, false);
                }
            }

            @Override
            public void onResolutionChanged(long ip, int width, int height) {
                if (width <= 0 || height <= 0) {
                    android.util.Log.w(TAG, "Ignoring invalid source resolution: "
                            + width + "x" + height);
                    return;
                }
                mSourceWidth = width;
                mSourceHeight = height;
                android.util.Log.i(TAG, "Source resolution change: " + width + "x" + height);
                if (mIsStarted.get()) {
                    HSVJEngine.updateMirrorSourceInfo(mLayerId, 0, 0, width, height);
                }
            }
        });

        mServer = lympServer;
        mIsStarted.set(true);
        mAppleServerStarted.set(true);
        try {
            lympServer.startServer();
        } catch (RuntimeException exception) {
            android.util.Log.e(TAG, "AirPlay server start failed", exception);
            mServer = null;
            mIsStarted.set(false);
            mAppleServerStarted.set(false);
            player.releasePlayer();
            syncServiceState("", false, false);
            HSVJEngine.setMirroringState(false);
            return;
        }

        syncServiceState(SERVICE_APPLE, true, false);
        HSVJEngine.setMirroringState(true);
        android.util.Log.i(TAG, "AirPlay service started; video output uses native RKMPP");
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
        if (mDeviceName.isEmpty()) {
            mDeviceName = defaultDeviceName();
        }
        mIsStarted.set(true);
        syncServiceState(SERVICE_ANDROID, true, false);
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
                            final int bufferWidth = buffer.getWidth();
                            final int bufferHeight = buffer.getHeight();
                            final int visibleWidth = resolveVisibleDimension(
                                    mSourceWidth, image.getWidth(), bufferWidth);
                            final int visibleHeight = resolveVisibleDimension(
                                    mSourceHeight, image.getHeight(), bufferHeight);
                            logFrameGeometryIfChanged(bufferWidth, bufferHeight,
                                    visibleWidth, visibleHeight);
                            HSVJEngine.pushMirrorFrame(mLayerId, buffer,
                                    bufferWidth, bufferHeight, visibleWidth, visibleHeight);
                        }
                    }
                    image.close();
                }
            } catch (Exception e) {
                if (image != null) image.close();
                android.util.Log.w(TAG, "Failed to forward mirror frame", e);
            }
        }, mHandler);

        mSurface = mImageReader.getSurface();
        android.util.Log.i(TAG, "Mirror frame surface ready: " + width + "x" + height);
    }

    public void stop() {
        if (!mIsStarted.get() && mServer == null && mImageReader == null && mHandlerThread == null) {
            syncServiceState("", false, false);
            return;
        }
        
        mIsStarted.set(false);
        if (mServer != null) {
            mServer.stopServer();
            mServer = null;
        }
        // stopServer() is asynchronous in the SDK; this is the deterministic
        // native-side lifecycle boundary and is safe when no AirPlay session exists.
        HSVJEngine.stopNativeAirPlayMirror();
        mAppleServerStarted.set(false);
        
        if (mImageReader != null) {
            mImageReader.close();
            mImageReader = null;
        }
        mSurface = null;
        mReaderWidth = 0;
        mReaderHeight = 0;
        resetSourceGeometry();

        Handler handler = mHandler;
        if (handler != null) {
            handler.removeCallbacksAndMessages(null);
        }
        
        HandlerThread handlerThread = mHandlerThread;
        mHandlerThread = null;
        if (handlerThread != null) {
            handlerThread.quitSafely();
            if (handlerThread != Thread.currentThread()) {
                try {
                    handlerThread.join(1000L);
                } catch (InterruptedException error) {
                    Thread.currentThread().interrupt();
                }
            }
        }
        mHandler = null;
        
        HSVJEngine.setMirroringState(false);
        mConnectedIp = 0;
        mIsConnected.set(false);
        syncServiceState("", false, false);
    }
    
    public void syncAndroidReadyState(boolean started, boolean connected) {
        syncServiceState(SERVICE_ANDROID, started, connected);
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
        if (mAppleServerStarted.get()) {
            syncServiceState(SERVICE_APPLE, true, mIsConnected.get());
        } else {
            HSVJEngine.setMirrorPin(mLayerId, mPinCode);
        }
        return mPinCode;
    }

    private static int resolveVisibleDimension(int sourceDimension,
                                               int imageDimension,
                                               int bufferDimension) {
        if (bufferDimension <= 0) {
            return 0;
        }
        if (sourceDimension > 0) {
            return Math.min(sourceDimension, bufferDimension);
        }
        if (imageDimension > 0) {
            return Math.min(imageDimension, bufferDimension);
        }
        return bufferDimension;
    }

    private void logFrameGeometryIfChanged(int bufferWidth, int bufferHeight,
                                           int visibleWidth, int visibleHeight) {
        if (bufferWidth == mLastLoggedBufferWidth
                && bufferHeight == mLastLoggedBufferHeight
                && visibleWidth == mLastLoggedVisibleWidth
                && visibleHeight == mLastLoggedVisibleHeight) {
            return;
        }
        mLastLoggedBufferWidth = bufferWidth;
        mLastLoggedBufferHeight = bufferHeight;
        mLastLoggedVisibleWidth = visibleWidth;
        mLastLoggedVisibleHeight = visibleHeight;
        android.util.Log.i(TAG, "Mirror frame geometry: buffer="
                + bufferWidth + "x" + bufferHeight
                + " visible=" + visibleWidth + "x" + visibleHeight
                + " source=" + mSourceWidth + "x" + mSourceHeight
                + " reader=" + mReaderWidth + "x" + mReaderHeight);
    }

    private void resetSourceGeometry() {
        mSourceWidth = 0;
        mSourceHeight = 0;
        mLastLoggedBufferWidth = 0;
        mLastLoggedBufferHeight = 0;
        mLastLoggedVisibleWidth = 0;
        mLastLoggedVisibleHeight = 0;
    }

    private String defaultDeviceName() {
        return Build.MODEL;
    }

    private void syncServiceState(String serviceName, boolean started, boolean connected) {
        HSVJEngine.setMirrorServiceState(mLayerId, serviceName, started, connected,
                mDeviceName.isEmpty() ? defaultDeviceName() : mDeviceName, mPinCode);
    }
}
