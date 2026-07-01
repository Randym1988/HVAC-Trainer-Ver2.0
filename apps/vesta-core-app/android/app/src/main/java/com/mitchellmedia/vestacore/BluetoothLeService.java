package com.mitchellmedia.vestacore;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothProfile;
import android.content.Intent;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Binder;
import android.os.Looper;
import androidx.core.app.NotificationCompat;
import java.util.UUID;
import com.getcapacitor.Bridge;
import com.getcapacitor.JSObject;

public class BluetoothLeService extends Service {
    private BluetoothAdapter mBluetoothAdapter;
    private BluetoothGatt mBluetoothGatt;
    private String mBluetoothDeviceAddress;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private int mReconnectAttempts = 0;
    private static final long RECONNECT_BACKOFF_MS = 1000;
    private static final long MAX_RECONNECT_BACKOFF_MS = 30000;

    private static final String CHANNEL_ID = "REVELATION_CORE_BLE_CHANNEL";
    private static final int NOTIFICATION_ID = 888;

    // UUIDs mapping exactly to Mitchell's ESP32 Firmware
    public static final UUID SERVICE_UUID = UUID.fromString("12a59900-173d-465c-914a-7a49c32c4438");
    public static final UUID TX_UUID = UUID.fromString("12a59901-173d-465c-914a-7a49c32c4438");
    public static final UUID RX_UUID = UUID.fromString("12a59902-173d-465c-914a-7a49c32c4438");
    public static final UUID CLIENT_CHARACTERISTIC_CONFIG = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    private final IBinder binder = new LocalBinder();
    public static Bridge bridge;

    public class LocalBinder extends Binder {
        BluetoothLeService getService() {
            return BluetoothLeService.this;
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        // Initialize BluetoothAdapter
        mBluetoothAdapter = BluetoothAdapter.getDefaultAdapter();
        
        createNotificationChannel();
        Notification notification = new NotificationCompat.Builder(this, CHANNEL_ID)
                .setContentTitle("Vesta Core Active")
                .setContentText("Maintaining live telemetry stream with HVAC Trainer...")
                .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
                .setPriority(NotificationCompat.PRIORITY_LOW)
                .build();

        // Safe foreground service initialization for Android 14 (API 34) and newer
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(NOTIFICATION_ID, notification, android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }
        
        if (intent != null) {
            String address = intent.getStringExtra("DEVICE_ADDRESS");
            if (address != null) {
                connect(address);
            }
        }
        
        return START_STICKY;
    }

    private final BluetoothGattCallback mGattCallback = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt gatt, int status, int newState) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                mReconnectAttempts = 0; // Reset on successful connection
                mBluetoothGatt.discoverServices();
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                // Keep retrying with capped backoff so temporary RF loss does not kill the service.
                mReconnectAttempts++;
                long backoffDelay = Math.min(
                        RECONNECT_BACKOFF_MS * (1L << Math.min(mReconnectAttempts - 1, 5)),
                        MAX_RECONNECT_BACKOFF_MS
                );
                mHandler.postDelayed(() -> {
                    if (mBluetoothGatt != null) {
                        mBluetoothGatt.connect();
                    }
                }, backoffDelay);
            }
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt gatt, int status) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                // Configure Bi-Directional Stream Channels
                BluetoothGattCharacteristic txChar = gatt.getService(SERVICE_UUID).getCharacteristic(TX_UUID);
                gatt.setCharacteristicNotification(txChar, true);

                // Enable hardware notification descriptors
                BluetoothGattDescriptor descriptor = txChar.getDescriptor(CLIENT_CHARACTERISTIC_CONFIG);
                descriptor.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                gatt.writeDescriptor(descriptor);
            }
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic) {
            if (TX_UUID.equals(characteristic.getUuid())) {
                String telemetryPayload = characteristic.getStringValue(0);
                emitTelemetryEvent(telemetryPayload);
            }
        }
    };

    public boolean connect(final String address) {
        if (mBluetoothAdapter == null || address == null) return false;
        final BluetoothDevice device = mBluetoothAdapter.getRemoteDevice(address);
        mBluetoothGatt = device.connectGatt(this, true, mGattCallback); // autoConnect set to true
        mBluetoothDeviceAddress = address;
        return true;
    }

    public void writeRXCharacteristic(byte[] value) {
        if (mBluetoothGatt == null) return;

        BluetoothGattService service = mBluetoothGatt.getService(SERVICE_UUID);
        if (service == null) return;

        BluetoothGattCharacteristic rxChar = service.getCharacteristic(RX_UUID);
        if (rxChar == null) return;

        rxChar.setValue(value);
        mBluetoothGatt.writeCharacteristic(rxChar);
    }

    private void emitTelemetryEvent(String data) {
        if (bridge != null) {
            JSObject ret = new JSObject();
            ret.put("value", data);
            bridge.triggerWindowJSEvent("telemetryData", ret.toString());
        }
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel serviceChannel = new NotificationChannel(
                    CHANNEL_ID, "HVAC Telemetry Service Channel", NotificationManager.IMPORTANCE_LOW);
            NotificationManager manager = getSystemService(NotificationManager.class);
            if (manager != null) manager.createNotificationChannel(serviceChannel);
        }
    }

    @Override
    public IBinder onBind(Intent intent) {
        return binder;
    }
}
