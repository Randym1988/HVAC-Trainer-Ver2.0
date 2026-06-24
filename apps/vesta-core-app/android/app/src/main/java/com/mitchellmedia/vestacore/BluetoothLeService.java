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
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothProfile;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import androidx.core.app.NotificationCompat;
import java.util.UUID;

public class BluetoothLeService extends Service {
    private BluetoothAdapter mBluetoothAdapter;
    private BluetoothGatt mBluetoothGatt;
    private String mBluetoothDeviceAddress;

    private static final String CHANNEL_ID = "REVELATION_CORE_BLE_CHANNEL";
    private static final int NOTIFICATION_ID = 888;

    // UUIDs mapping exactly to Mitchell's ESP32 Firmware
    public static final UUID SERVICE_UUID = UUID.fromString("12a59900-173d-465c-914a-7a49c32c4438");
    public static final UUID TX_UUID = UUID.fromString("12a59901-173d-465c-914a-7a49c32c4438");
    public static final UUID RX_UUID = UUID.fromString("12a59902-173d-465c-914a-7a49c32c4438");
    public static final UUID CLIENT_CHARACTERISTIC_CONFIG = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        // Initialize BluetoothAdapter
        mBluetoothAdapter = BluetoothAdapter.getDefaultAdapter();
        
        createNotificationChannel();
        Notification notification = new NotificationCompat.Builder(this, CHANNEL_ID)
                .setContentTitle("Revelation Core Active")
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
                mBluetoothGatt.discoverServices();
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                // AUTO-RECONNECT ENGINE: Retry connection instantly if line drops
                if (mBluetoothGatt != null) {
                    mBluetoothGatt.connect();
                }
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
                // Broadcast telemetryPayload to your HTML/UI Layer view wrapper here
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

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel serviceChannel = new NotificationChannel(
                    CHANNEL_ID, "HVAC Telemetry Service Channel", NotificationManager.IMPORTANCE_LOW);
            NotificationManager manager = getSystemService(NotificationManager.class);
            if (manager != null) manager.createNotificationChannel(serviceChannel);
        }
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
