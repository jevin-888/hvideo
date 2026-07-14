// AOSP 系统服务状态实现
// 部署路径: frameworks/base/core/java/android/wfd_sink/WfdSinkStatus.java
package android.wfd_sink;

import android.os.Parcel;
import android.os.Parcelable;

/**
 * WFD Sink 服务运行状态快照。
 * 字段与原 com.hsvj.wfd.WfdMiracast管理器.Status 对齐，新增 deviceName。
 */
public final class WfdSinkStatus implements Parcelable {
    public final boolean serviceStarted;
    public final boolean p2pEnabled;
    public final boolean wfdEnabled;
    public final boolean connected;
    public final boolean sinkRunning;
    public final String deviceAddress;
    public final String peerInfo;
    public final String message;
    public final String deviceName;

    public WfdSinkStatus(boolean serviceStarted, boolean p2pEnabled, boolean wfdEnabled,
                         boolean connected, boolean sinkRunning, String deviceAddress,
                         String peerInfo, String message, String deviceName) {
        this.serviceStarted = serviceStarted;
        this.p2pEnabled = p2pEnabled;
        this.wfdEnabled = wfdEnabled;
        this.connected = connected;
        this.sinkRunning = sinkRunning;
        this.deviceAddress = deviceAddress != null ? deviceAddress : "";
        this.peerInfo = peerInfo != null ? peerInfo : "";
        this.message = message != null ? message : "";
        this.deviceName = deviceName != null ? deviceName : "";
    }

    protected WfdSinkStatus(Parcel in) {
        serviceStarted = in.readByte() != 0;
        p2pEnabled = in.readByte() != 0;
        wfdEnabled = in.readByte() != 0;
        connected = in.readByte() != 0;
        sinkRunning = in.readByte() != 0;
        deviceAddress = in.readString();
        peerInfo = in.readString();
        message = in.readString();
        deviceName = in.readString();
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        dest.writeByte((byte) (serviceStarted ? 1 : 0));
        dest.writeByte((byte) (p2pEnabled ? 1 : 0));
        dest.writeByte((byte) (wfdEnabled ? 1 : 0));
        dest.writeByte((byte) (connected ? 1 : 0));
        dest.writeByte((byte) (sinkRunning ? 1 : 0));
        dest.writeString(deviceAddress);
        dest.writeString(peerInfo);
        dest.writeString(message);
        dest.writeString(deviceName);
    }

    @Override
    public int describeContents() {
        return 0;
    }

    public static final Creator<WfdSinkStatus> CREATOR = new Creator<WfdSinkStatus>() {
        @Override
        public WfdSinkStatus createFromParcel(Parcel in) {
            return new WfdSinkStatus(in);
        }

        @Override
        public WfdSinkStatus[] newArray(int size) {
            return new WfdSinkStatus[size];
        }
    };

    @Override
    public String toString() {
        return "WfdSinkStatus{started=" + serviceStarted
                + ", p2p=" + p2pEnabled
                + ", wfd=" + wfdEnabled
                + ", connected=" + connected
                + ", sink=" + sinkRunning
                + ", addr=" + deviceAddress
                + ", peer=" + peerInfo
                + ", msg=" + message
                + ", name=" + deviceName + "}";
    }
}
