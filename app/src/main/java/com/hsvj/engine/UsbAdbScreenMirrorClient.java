package com.hsvj.engine;

import android.content.Context;
import android.content.SharedPreferences;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.os.SystemClock;
import android.util.Base64;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.KeyFactory;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.PrivateKey;
import java.security.PublicKey;
import java.security.interfaces.RSAPublicKey;
import java.security.spec.PKCS8EncodedKeySpec;
import java.security.spec.X509EncodedKeySpec;
import java.util.HashMap;
import java.util.concurrent.atomic.AtomicBoolean;

import javax.crypto.Cipher;

/**
 * USB ADB phone mirror bootstrap.
 *
 * The media path is native-only: Java opens/claims the USB ADB interface and
 * performs the one-time ADB authorization handshake, then C++ owns ADB streams,
 * scrcpy H.264/video socket, scrcpy raw audio, RKMPP decode, and AudioPlayer output.
 */
public class UsbAdbScreenMirrorClient {
    private static final String TAG = "UsbAdbMirror";

    private static final int ADB_CLASS = 0xff;
    private static final int ADB_SUBCLASS = 0x42;
    private static final int ADB_PROTOCOL = 0x01;

    private static final int CMD_CNXN = command("CNXN");
    private static final int CMD_AUTH = command("AUTH");
    private static final int AUTH_TOKEN = 1;
    private static final int AUTH_SIGNATURE = 2;
    private static final int AUTH_RSAPUBLICKEY = 3;
    private static final int ADB_VERSION = 0x01000000;
    private static final int ADB_MAX_DATA = 256 * 1024;
    private static final int ADB_CONNECT_MAX_ATTEMPTS = 6;
    private static final long ADB_REENUMERATION_SETTLE_MS = 800L;
    private static final int USB_TIMEOUT_MS = 1000;

    private static final int RKMPP_H264_MAX_WIDTH = 4096;
    private static final int RKMPP_H264_MAX_HEIGHT = 4096;
    private static final String SCRCPY_SERVER_VERSION = "3.3.4";
    private static final String SCRCPY_SERVER_ASSET = "scrcpy/scrcpy-server-v3.3.4";

    private static final byte[] SHA1_DIGEST_INFO_PREFIX = new byte[] {
            0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e,
            0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14
    };

    private final Context context;
    private final UsbManager usbManager;
    private final int layerId;
    private final AtomicBoolean running = new AtomicBoolean(false);

    private volatile String lastMessage = "idle";
    private volatile boolean connected;
    private volatile boolean awaitingAuthorization;
    private volatile String deviceName = "";
    private volatile int videoWidth;
    private volatile int videoHeight;
    private volatile boolean nativeBackendActive;
    private volatile boolean foregroundAppMonitorEnabled;
    private volatile String foregroundPackage = "";
    private volatile String foregroundRawFocus = "";
    private volatile String foregroundLaunchPackage = "";

    private UsbDeviceConnection connection;
    private UsbInterface adbInterface;
    private UsbEndpoint bulkIn;
    private UsbEndpoint bulkOut;
    private String claimedDevicePath;
    private Thread nativeStatusThread;
    private AdbPacket pendingPacket;
    private int nextLocalId = 1;
    private volatile boolean nativeStartInProgress;
    private final AtomicBoolean unexpectedDisconnectNotified = new AtomicBoolean(false);
    private volatile ForegroundAppListener foregroundAppListener;
    private volatile UnexpectedDisconnectListener unexpectedDisconnectListener;

    public UsbAdbScreenMirrorClient(Context context, int layerId) {
        this.context = context.getApplicationContext();
        this.layerId = layerId;
        this.usbManager = (UsbManager) context.getSystemService(Context.USB_SERVICE);
    }

    public interface ForegroundAppListener {
        void onForegroundAppChanged(String packageName, String rawFocusLine, String launchPackage);
    }

    /** Called once after a physical USB/ADB session disappears without an explicit stop(). */
    public interface UnexpectedDisconnectListener {
        void onUnexpectedDisconnect();
    }

    public void setUnexpectedDisconnectListener(UnexpectedDisconnectListener listener) {
        unexpectedDisconnectListener = listener;
    }

    public void setForegroundAppListener(ForegroundAppListener listener) {
        foregroundAppListener = listener;
        if (listener == null) {
            setForegroundAppMonitorEnabled(false);
        }
    }

    public void setForegroundAppMonitorEnabled(boolean enabled) {
        foregroundAppMonitorEnabled = enabled;
        if (nativeBackendActive) {
            try {
                HSVJEngine.setNativeUsbAdbMirrorForegroundMonitorEnabled(enabled);
            } catch (Throwable t) {
                Log.w(TAG, "native foreground monitor toggle failed", t);
            }
        }
    }

    public synchronized boolean start(int preferredWidth, int preferredHeight) {
        if (running.get()) {
            lastMessage = nativeStartInProgress ? "native USB ADB mirror starting" : "already running";
            return true;
        }

        unexpectedDisconnectNotified.set(false);
        UsbSelection selection = findAdbUsbDevice();
        if (selection == null) {
            lastMessage = "未找到 USB ADB 设备";
            return false;
        }

        UsbDevice device = selection.device;
        deviceName = buildDeviceName(device);
        if (!usbManager.hasPermission(device)) {
            Log.w(TAG, "USB permission is not granted for " + deviceName
                    + ", trying openDevice because system app/root image may allow it");
        }

        UsbDeviceConnection opened = usbManager.openDevice(device);
        if (opened == null) {
            lastMessage = "无法打开 USB 设备，可能缺少 USB 权限";
            return false;
        }
        if (!opened.claimInterface(selection.usbInterface, true)) {
            opened.close();
            lastMessage = "无法 claim ADB Interface";
            return false;
        }

        connection = opened;
        adbInterface = selection.usbInterface;
        bulkIn = selection.bulkIn;
        bulkOut = selection.bulkOut;
        claimedDevicePath = device.getDeviceName();
        pendingPacket = null;
        nextLocalId = 1;

        int[] size = normalizeVideoSize(preferredWidth, preferredHeight);
        videoWidth = size[0];
        videoHeight = size[1];
        running.set(true);
        nativeStartInProgress = true;
        lastMessage = "native USB ADB mirror starting: " + deviceName;
        startNativeBackendThread(preferredWidth, preferredHeight);
        return true;
    }

    public synchronized void stop() {
        unexpectedDisconnectNotified.set(true);
        running.set(false);
        nativeStartInProgress = false;
        stopNativeBackend();
        closeUsb();
        connected = false;
        awaitingAuthorization = false;
        lastMessage = "stopped";
    }

    public boolean isRunning() {
        if (!running.get()) return false;
        if (!nativeBackendActive) return nativeStartInProgress;
        try {
            boolean nativeRunning = HSVJEngine.isNativeUsbAdbMirrorRunning();
            if (!nativeRunning) {
                handleUnexpectedNativeStop();
            }
            return nativeRunning;
        } catch (Throwable t) {
            return running.get();
        }
    }

    public boolean isConnected() {
        if (nativeBackendActive) {
            try {
                connected = HSVJEngine.isNativeUsbAdbMirrorConnected();
            } catch (Throwable ignored) {
            }
        }
        return connected;
    }

    public boolean isAwaitingAuthorization() {
        if (nativeBackendActive) {
            getLastMessage();
        }
        return awaitingAuthorization;
    }

    public String getLastMessage() {
        if (nativeBackendActive) {
            try {
                String message = HSVJEngine.getNativeUsbAdbMirrorLastMessage();
                if (message != null && !message.isEmpty()) {
                    lastMessage = message;
                }
            } catch (Throwable ignored) {
            }
        }
        return lastMessage;
    }

    public String getDeviceName() {
        return deviceName;
    }

    public int getVideoWidth() {
        return videoWidth;
    }

    public int getVideoHeight() {
        return videoHeight;
    }

    public String getForegroundPackage() {
        if (nativeBackendActive) {
            syncNativeBackendState();
        }
        return foregroundPackage;
    }

    public String getForegroundRawFocus() {
        if (nativeBackendActive) {
            syncNativeBackendState();
        }
        return foregroundRawFocus;
    }

    public String getForegroundLaunchPackage() {
        if (nativeBackendActive) {
            syncNativeBackendState();
        }
        return foregroundLaunchPackage;
    }

    public static int[] chooseMirrorSize(int physicalWidth, int physicalHeight) {
        if (physicalWidth <= 0 || physicalHeight <= 0) {
            return new int[] {1080, 1920};
        }
        double scale = Math.min(1.0, Math.min(
                (double) RKMPP_H264_MAX_WIDTH / (double) physicalWidth,
                (double) RKMPP_H264_MAX_HEIGHT / (double) physicalHeight));
        int width = alignToMacroblock((int) Math.round(physicalWidth * scale),
                RKMPP_H264_MAX_WIDTH);
        int height = alignToMacroblock((int) Math.round(physicalHeight * scale),
                RKMPP_H264_MAX_HEIGHT);
        return new int[] {width, height};
    }

    private boolean tryStartNativeBackend(int preferredWidth, int preferredHeight) {
        try {
            connectAdb();
            if (!running.get()) {
                return false;
            }
            int fd = connection != null ? connection.getFileDescriptor() : -1;
            if (fd < 0) {
                failNativeStart("invalid UsbDeviceConnection fd after ADB connect");
                return false;
            }
            String scrcpyServerPath = prepareScrcpyServerFile();
            String keyDir = context.getFilesDir() != null
                    ? context.getFilesDir().getAbsolutePath()
                    : context.getCacheDir().getAbsolutePath();
            boolean started = HSVJEngine.startNativeUsbAdbMirror(
                    layerId,
                    fd,
                    bulkIn.getAddress(),
                    bulkOut.getAddress(),
                    videoWidth > 0 ? videoWidth : preferredWidth,
                    videoHeight > 0 ? videoHeight : preferredHeight,
                    scrcpyServerPath,
                    keyDir,
                    true,
                    foregroundAppMonitorEnabled);
            if (!started) {
                failNativeStart("native USB mirror start returned false");
                return false;
            }
            nativeBackendActive = true;
            awaitingAuthorization = false;
            connected = HSVJEngine.isNativeUsbAdbMirrorConnected();
            startNativeStatusBridge();
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "native USB mirror start failed", t);
            try {
                HSVJEngine.stopNativeUsbAdbMirror();
            } catch (Throwable ignored) {
            }
            nativeBackendActive = false;
            failNativeStart(t.getMessage() != null
                    ? t.getMessage()
                    : t.getClass().getSimpleName());
            return false;
        }
    }

    private void failNativeStart(String reason) {
        lastMessage = reason != null && !reason.isEmpty()
                ? reason
                : "native USB ADB mirror start failed";
        connected = false;
        awaitingAuthorization = false;
        pendingPacket = null;
        nextLocalId = 1;
    }

    private String prepareScrcpyServerFile() throws IOException {
        File dir = new File(context.getCacheDir(), "scrcpy");
        if (!dir.exists() && !dir.mkdirs() && !dir.isDirectory()) {
            throw new IOException("Cannot create scrcpy cache dir: " + dir);
        }
        File out = new File(dir, "scrcpy-server-" + SCRCPY_SERVER_VERSION + ".jar");
        byte[] data = readAssetFully(SCRCPY_SERVER_ASSET);
        if (!out.exists() || out.length() != data.length) {
            try (FileOutputStream output = new FileOutputStream(out, false)) {
                output.write(data);
            }
        }
        return out.getAbsolutePath();
    }

    private byte[] readAssetFully(String assetPath) throws IOException {
        try (InputStream input = context.getAssets().open(assetPath);
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[16 * 1024];
            int read;
            while ((read = input.read(buffer)) >= 0) {
                if (read > 0) output.write(buffer, 0, read);
            }
            return output.toByteArray();
        }
    }

    private void startNativeBackendThread(int preferredWidth, int preferredHeight) {
        Thread thread = new Thread(() -> {
            try {
                android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND);
            } catch (Throwable ignored) {
            }
            boolean started = false;
            try {
                started = tryStartNativeBackend(preferredWidth, preferredHeight);
            } finally {
                nativeStartInProgress = false;
                if (!started) {
                    if (running.get()) {
                        running.set(false);
                    }
                    stopNativeBackend();
                    closeUsb();
                    connected = false;
                    awaitingAuthorization = false;
                    notifyUnexpectedDisconnect();
                }
            }
        }, "UsbAdbNativeStart");
        thread.start();
    }

    private void startNativeStatusBridge() {
        stopNativeStatusBridge();
        nativeStatusThread = new Thread(this::runNativeStatusLoop, "UsbAdbNativeStatus");
        nativeStatusThread.start();
    }

    private void stopNativeBackend() {
        boolean wasNative = nativeBackendActive;
        nativeBackendActive = false;
        if (wasNative) {
            try {
                HSVJEngine.stopNativeUsbAdbMirror();
            } catch (Throwable t) {
                Log.w(TAG, "stop native USB mirror failed", t);
            }
        }
        stopNativeStatusBridge();
    }

    private void handleUnexpectedNativeStop() {
        boolean wasNative = nativeBackendActive;
        boolean wasRunning = running.getAndSet(false);
        nativeBackendActive = false;
        nativeStartInProgress = false;
        connected = false;
        awaitingAuthorization = false;
        lastMessage = "USB ADB transport disconnected";
        if (wasNative) {
            try {
                HSVJEngine.stopNativeUsbAdbMirror();
            } catch (Throwable t) {
                Log.w(TAG, "stop disconnected native USB mirror failed", t);
            }
        }
        stopNativeStatusBridge();
        closeUsb();
        Log.i(TAG, "native USB mirror disconnected and released");
        if (wasNative || wasRunning) {
            notifyUnexpectedDisconnect();
        }
    }

    private void notifyUnexpectedDisconnect() {
        if (!unexpectedDisconnectNotified.compareAndSet(false, true)) {
            return;
        }
        UnexpectedDisconnectListener listener = unexpectedDisconnectListener;
        if (listener == null) {
            return;
        }
        try {
            listener.onUnexpectedDisconnect();
        } catch (Throwable t) {
            Log.w(TAG, "unexpected USB disconnect callback failed", t);
        }
    }

    private void stopNativeStatusBridge() {
        Thread thread = nativeStatusThread;
        nativeStatusThread = null;
        if (thread != null && thread != Thread.currentThread()) {
            try {
                thread.join(800);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
    }

    private void runNativeStatusLoop() {
        try {
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND);
        } catch (Throwable ignored) {
        }
        while (running.get() && nativeBackendActive) {
            syncNativeBackendState();
            boolean stillRunning = false;
            try {
                stillRunning = HSVJEngine.isNativeUsbAdbMirrorRunning();
            } catch (Throwable t) {
                Log.w(TAG, "native USB mirror status unavailable", t);
            }
            if (!stillRunning) {
                syncNativeBackendState();
                break;
            }
            SystemClock.sleep(foregroundAppMonitorEnabled ? 200L : 1000L);
        }
        if (nativeBackendActive && running.get()) {
            handleUnexpectedNativeStop();
        }
    }

    private void syncNativeBackendState() {
        if (!nativeBackendActive) return;
        try {
            connected = HSVJEngine.isNativeUsbAdbMirrorConnected();
            String message = HSVJEngine.getNativeUsbAdbMirrorLastMessage();
            if (message != null && !message.isEmpty()) {
                lastMessage = message;
                awaitingAuthorization = message.contains("allow USB debugging")
                        || message.contains("USB 调试");
            }
            String packageName = safeString(HSVJEngine.getNativeUsbAdbMirrorForegroundPackage());
            String rawFocus = safeString(HSVJEngine.getNativeUsbAdbMirrorForegroundRawFocus());
            String launchPackage = safeString(
                    HSVJEngine.getNativeUsbAdbMirrorForegroundLaunchPackage());
            boolean changed = !packageName.equals(foregroundPackage)
                    || !rawFocus.equals(foregroundRawFocus)
                    || !launchPackage.equals(foregroundLaunchPackage);
            foregroundPackage = packageName;
            foregroundRawFocus = rawFocus;
            foregroundLaunchPackage = launchPackage;
            if (changed && foregroundAppMonitorEnabled && foregroundAppListener != null) {
                foregroundAppListener.onForegroundAppChanged(packageName, rawFocus, launchPackage);
            }
        } catch (Throwable t) {
            Log.w(TAG, "sync native USB mirror state failed", t);
        }
    }

    private void connectAdb() throws Exception {
        KeyPair keyPair = getOrCreateKeyPair();
        Exception lastError = null;
        for (int attempt = 1; running.get() && attempt <= ADB_CONNECT_MAX_ATTEMPTS; attempt++) {
            awaitingAuthorization = false;
            boolean publicKeySent = false;
            long authDeadline = SystemClock.uptimeMillis() + 60000;
            long attemptStartedAt = SystemClock.uptimeMillis();
            try {
                Log.i(TAG, "ADB connect attempt " + attempt + " device=" + claimedDevicePath);
                sendPacket(CMD_CNXN, ADB_VERSION, ADB_MAX_DATA,
                        "host::hsvj-usb-mirror\0".getBytes(StandardCharsets.UTF_8));

                while (running.get() && SystemClock.uptimeMillis() < authDeadline) {
                    AdbPacket packet;
                    try {
                        packet = readPacket(1000);
                    } catch (UsbReadTimeoutException timeout) {
                        if (!isClaimedUsbDevicePresent()) {
                            throw new UsbDeviceDetachedException(
                                    "USB ADB device re-enumerated while waiting for CNXN");
                        }
                        if (!awaitingAuthorization
                                && SystemClock.uptimeMillis() - attemptStartedAt > 8000) {
                            throw new UsbHandshakeNoResponseException(
                                    "USB ADB handshake got no response from adbd");
                        }
                        if (awaitingAuthorization) {
                            lastMessage = "请在 vivo 手机上允许 USB 调试授权";
                        }
                        continue;
                    }
                    if (packet.command == CMD_CNXN) {
                        connected = true;
                        awaitingAuthorization = false;
                        lastMessage = "ADB connected: " + payloadString(packet.payload);
                        Log.i(TAG, "ADB connected: " + deviceName);
                        return;
                    }
                    if (packet.command == CMD_AUTH && packet.arg0 == AUTH_TOKEN) {
                        if (!publicKeySent) {
                            sendPacket(CMD_AUTH, AUTH_SIGNATURE, 0,
                                    signAdbToken(keyPair.getPrivate(), packet.payload));
                            publicKeySent = true;
                            lastMessage = "ADB auth signature sent";
                        } else {
                            awaitingAuthorization = true;
                            sendPacket(CMD_AUTH, AUTH_RSAPUBLICKEY, 0,
                                    encodeAdbPublicKey((RSAPublicKey) keyPair.getPublic()));
                            lastMessage = "请在 vivo 手机上允许 USB 调试授权";
                            Log.w(TAG, lastMessage);
                        }
                    }
                }
                throw new IllegalStateException("ADB 授权超时，请在 vivo 手机上点允许 USB 调试");
            } catch (UsbDeviceDetachedException e) {
                lastError = e;
                Log.w(TAG, "ADB USB device detached during connect, retrying: "
                        + e.getMessage());
                SystemClock.sleep(ADB_REENUMERATION_SETTLE_MS);
                if (!reopenAdbUsbDevice(8000)) {
                    break;
                }
            } catch (UsbHandshakeNoResponseException e) {
                lastError = e;
                Log.w(TAG, "ADB USB handshake produced no packets, reopening interface: "
                        + e.getMessage());
                if (!reopenAdbUsbDevice(8000)) {
                    break;
                }
            }
        }
        if (lastError != null) throw lastError;
        throw new IllegalStateException("ADB 授权超时，请在 vivo 手机上点允许 USB 调试");
    }

    private AdbPacket readPacket(long idleTimeoutMs) throws Exception {
        if (pendingPacket != null) {
            AdbPacket packet = pendingPacket;
            pendingPacket = null;
            return packet;
        }
        byte[] header = new byte[24];
        readFully(header, header.length, idleTimeoutMs);
        ByteBuffer buffer = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        int command = buffer.getInt();
        int arg0 = buffer.getInt();
        int arg1 = buffer.getInt();
        int length = buffer.getInt();
        int checksum = buffer.getInt();
        int magic = buffer.getInt();
        if ((command ^ 0xffffffff) != magic) {
            throw new IllegalStateException("Bad ADB packet magic");
        }
        if (length < 0 || length > ADB_MAX_DATA * 2) {
            throw new IllegalStateException("Bad ADB payload length: " + length);
        }
        byte[] payload = null;
        if (length > 0) {
            payload = new byte[length];
            readFully(payload, length, idleTimeoutMs);
            if (checksum(payload) != checksum) {
                throw new IllegalStateException("Bad ADB packet checksum");
            }
        }
        return new AdbPacket(command, arg0, arg1, payload);
    }

    private void sendPacket(int command, int arg0, int arg1, byte[] payload) throws Exception {
        int length = payload != null ? payload.length : 0;
        ByteBuffer header = ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN);
        header.putInt(command);
        header.putInt(arg0);
        header.putInt(arg1);
        header.putInt(length);
        header.putInt(payload != null ? checksum(payload) : 0);
        header.putInt(command ^ 0xffffffff);
        writeFully(header.array(), 24);
        if (payload != null && payload.length > 0) {
            writeFully(payload, payload.length);
        }
    }

    private void readFully(byte[] data, int length, long idleTimeoutMs) throws Exception {
        int offset = 0;
        long lastProgressMs = SystemClock.uptimeMillis();
        while (running.get() && offset < length) {
            int count = connection.bulkTransfer(bulkIn, data, offset, length - offset, USB_TIMEOUT_MS);
            if (count > 0) {
                offset += count;
                lastProgressMs = SystemClock.uptimeMillis();
            } else if (count < 0) {
                throw new UsbDeviceDetachedException("USB bulk read failed, device may be detached");
            } else if (SystemClock.uptimeMillis() - lastProgressMs > idleTimeoutMs) {
                if (!isClaimedUsbDevicePresent()) {
                    throw new UsbDeviceDetachedException("USB ADB device detached during read");
                }
                throw new UsbReadTimeoutException("USB bulk read timeout");
            }
        }
        if (offset < length) throw new IllegalStateException("USB read stopped");
    }

    private void writeFully(byte[] data, int length) throws Exception {
        int offset = 0;
        while (running.get() && offset < length) {
            int count = connection.bulkTransfer(bulkOut, data, offset, length - offset, USB_TIMEOUT_MS);
            if (count > 0) {
                offset += count;
            } else if (count < 0) {
                throw new UsbDeviceDetachedException("USB bulk write failed, device may be detached");
            }
        }
        if (offset < length) throw new IllegalStateException("USB write stopped");
    }

    private UsbSelection findAdbUsbDevice() {
        HashMap<String, UsbDevice> devices = usbManager.getDeviceList();
        for (UsbDevice device : devices.values()) {
            for (int i = 0; i < device.getInterfaceCount(); i++) {
                UsbInterface usbInterface = device.getInterface(i);
                if (usbInterface.getInterfaceClass() != ADB_CLASS
                        || usbInterface.getInterfaceSubclass() != ADB_SUBCLASS
                        || usbInterface.getInterfaceProtocol() != ADB_PROTOCOL) {
                    continue;
                }
                UsbEndpoint in = null;
                UsbEndpoint out = null;
                for (int ep = 0; ep < usbInterface.getEndpointCount(); ep++) {
                    UsbEndpoint endpoint = usbInterface.getEndpoint(ep);
                    if (endpoint.getType() != UsbConstants.USB_ENDPOINT_XFER_BULK) continue;
                    if (endpoint.getDirection() == UsbConstants.USB_DIR_IN) {
                        in = endpoint;
                    } else {
                        out = endpoint;
                    }
                }
                if (in != null && out != null) {
                    return new UsbSelection(device, usbInterface, in, out);
                }
            }
        }
        return null;
    }

    private boolean isClaimedUsbDevicePresent() {
        if (claimedDevicePath == null || claimedDevicePath.isEmpty()) {
            return connection != null;
        }
        HashMap<String, UsbDevice> devices = usbManager.getDeviceList();
        for (UsbDevice device : devices.values()) {
            if (claimedDevicePath.equals(device.getDeviceName())) {
                return true;
            }
        }
        return false;
    }

    private boolean reopenAdbUsbDevice(long timeoutMs) {
        closeUsb();
        long deadline = SystemClock.uptimeMillis() + timeoutMs;
        while (running.get() && SystemClock.uptimeMillis() < deadline) {
            UsbSelection selection = findAdbUsbDevice();
            if (selection == null) {
                SystemClock.sleep(250);
                continue;
            }
            UsbDevice device = selection.device;
            UsbDeviceConnection opened = usbManager.openDevice(device);
            if (opened == null) {
                lastMessage = "无法重新打开 USB ADB 设备";
                SystemClock.sleep(250);
                continue;
            }
            if (!opened.claimInterface(selection.usbInterface, true)) {
                opened.close();
                lastMessage = "无法重新 claim ADB Interface";
                SystemClock.sleep(250);
                continue;
            }
            connection = opened;
            adbInterface = selection.usbInterface;
            bulkIn = selection.bulkIn;
            bulkOut = selection.bulkOut;
            claimedDevicePath = device.getDeviceName();
            deviceName = buildDeviceName(device);
            connected = false;
            awaitingAuthorization = false;
            Log.i(TAG, "Reopened USB ADB device: " + claimedDevicePath + " " + deviceName);
            return true;
        }
        return false;
    }

    private void closeUsb() {
        try {
            if (connection != null && adbInterface != null) {
                connection.releaseInterface(adbInterface);
            }
        } catch (Throwable ignored) {
        }
        try {
            if (connection != null) connection.close();
        } catch (Throwable ignored) {
        }
        connection = null;
        adbInterface = null;
        bulkIn = null;
        bulkOut = null;
        claimedDevicePath = null;
        pendingPacket = null;
    }

    private KeyPair getOrCreateKeyPair() throws Exception {
        SharedPreferences prefs = context.getSharedPreferences("usb_adb_mirror", Context.MODE_PRIVATE);
        String privateEncoded = prefs.getString("private_key", null);
        String publicEncoded = prefs.getString("public_key", null);
        KeyFactory factory = KeyFactory.getInstance("RSA");
        if (privateEncoded != null && publicEncoded != null) {
            PrivateKey privateKey = factory.generatePrivate(new PKCS8EncodedKeySpec(
                    Base64.decode(privateEncoded, Base64.NO_WRAP)));
            PublicKey publicKey = factory.generatePublic(new X509EncodedKeySpec(
                    Base64.decode(publicEncoded, Base64.NO_WRAP)));
            return new KeyPair(publicKey, privateKey);
        }
        KeyPairGenerator generator = KeyPairGenerator.getInstance("RSA");
        generator.initialize(2048);
        KeyPair keyPair = generator.generateKeyPair();
        prefs.edit()
                .putString("private_key", Base64.encodeToString(
                        keyPair.getPrivate().getEncoded(), Base64.NO_WRAP))
                .putString("public_key", Base64.encodeToString(
                        keyPair.getPublic().getEncoded(), Base64.NO_WRAP))
                .apply();
        return keyPair;
    }

    private byte[] signAdbToken(PrivateKey privateKey, byte[] token) throws Exception {
        ByteArrayOutputStream digestInfo = new ByteArrayOutputStream();
        digestInfo.write(SHA1_DIGEST_INFO_PREFIX);
        digestInfo.write(token);
        Cipher cipher = Cipher.getInstance("RSA/ECB/PKCS1Padding");
        cipher.init(Cipher.ENCRYPT_MODE, privateKey);
        return cipher.doFinal(digestInfo.toByteArray());
    }

    private byte[] encodeAdbPublicKey(RSAPublicKey publicKey) {
        final int words = 64;
        BigInteger modulus = publicKey.getModulus();
        long[] n = toLittleEndianWords(modulus, words);
        long n0 = n[0] & 0xffffffffL;
        long inv = BigInteger.valueOf(n0).modInverse(BigInteger.ONE.shiftLeft(32)).longValue();
        long n0inv = (-inv) & 0xffffffffL;
        BigInteger rrValue = BigInteger.ONE.shiftLeft(words * 64).mod(modulus);
        long[] rr = toLittleEndianWords(rrValue, words);

        ByteBuffer raw = ByteBuffer.allocate(4 + 4 + words * 4 + words * 4 + 4)
                .order(ByteOrder.LITTLE_ENDIAN);
        raw.putInt(words);
        raw.putInt((int) n0inv);
        for (long word : n) raw.putInt((int) word);
        for (long word : rr) raw.putInt((int) word);
        raw.putInt(publicKey.getPublicExponent().intValue());

        String text = Base64.encodeToString(raw.array(), Base64.NO_WRAP) + " hsvj@rk3566";
        return (text + "\0").getBytes(StandardCharsets.UTF_8);
    }

    private long[] toLittleEndianWords(BigInteger value, int words) {
        long[] out = new long[words];
        byte[] bigEndian = value.toByteArray();
        for (int i = 0; i < words * 4; i++) {
            int source = bigEndian.length - 1 - i;
            int b = source >= 0 ? bigEndian[source] & 0xff : 0;
            out[i / 4] |= ((long) b) << ((i % 4) * 8);
        }
        return out;
    }

    private int[] normalizeVideoSize(int width, int height) {
        int[] chosen = chooseMirrorSize(width, height);
        if (chosen[0] <= 0 || chosen[1] <= 0) {
            return new int[] {1080, 1920};
        }
        return chosen;
    }

    private static int alignToMacroblock(int value, int maxValue) {
        int aligned = Math.max(16, ((value + 8) / 16) * 16);
        if (aligned > maxValue) {
            aligned = (maxValue / 16) * 16;
        }
        return Math.max(16, aligned);
    }

    private String payloadString(byte[] payload) {
        if (payload == null) return "";
        int len = payload.length;
        if (len > 0 && payload[len - 1] == 0) len--;
        return new String(payload, 0, len, StandardCharsets.UTF_8);
    }

    private static int command(String text) {
        byte[] bytes = text.getBytes(StandardCharsets.US_ASCII);
        return (bytes[0] & 0xff)
                | ((bytes[1] & 0xff) << 8)
                | ((bytes[2] & 0xff) << 16)
                | ((bytes[3] & 0xff) << 24);
    }

    private static int checksum(byte[] payload) {
        int sum = 0;
        for (byte b : payload) sum += b & 0xff;
        return sum;
    }

    private static String safeString(String value) {
        return value != null ? value : "";
    }

    private static String buildDeviceName(UsbDevice device) {
        String manufacturer = device.getManufacturerName();
        String product = device.getProductName();
        if (manufacturer == null) manufacturer = "";
        if (product == null) product = "";
        String name = (manufacturer + " " + product).trim();
        if (name.isEmpty()) {
            name = String.format("USB %04x:%04x", device.getVendorId(), device.getProductId());
        }
        return name;
    }

    private static final class UsbSelection {
        final UsbDevice device;
        final UsbInterface usbInterface;
        final UsbEndpoint bulkIn;
        final UsbEndpoint bulkOut;

        UsbSelection(UsbDevice device, UsbInterface usbInterface,
                UsbEndpoint bulkIn, UsbEndpoint bulkOut) {
            this.device = device;
            this.usbInterface = usbInterface;
            this.bulkIn = bulkIn;
            this.bulkOut = bulkOut;
        }
    }

    private static final class AdbPacket {
        final int command;
        final int arg0;
        final int arg1;
        final byte[] payload;

        AdbPacket(int command, int arg0, int arg1, byte[] payload) {
            this.command = command;
            this.arg0 = arg0;
            this.arg1 = arg1;
            this.payload = payload;
        }
    }

    private static final class UsbReadTimeoutException extends Exception {
        UsbReadTimeoutException(String message) {
            super(message);
        }
    }

    private static final class UsbDeviceDetachedException extends Exception {
        UsbDeviceDetachedException(String message) {
            super(message);
        }
    }

    private static final class UsbHandshakeNoResponseException extends Exception {
        UsbHandshakeNoResponseException(String message) {
            super(message);
        }
    }
}
