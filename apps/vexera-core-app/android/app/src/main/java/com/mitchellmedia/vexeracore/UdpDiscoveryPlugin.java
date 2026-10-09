package com.mitchellmedia.vexeracore;

import com.getcapacitor.JSArray;
import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.CapacitorPlugin;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Sends a UDP broadcast and collects every reply for a short window. */
@CapacitorPlugin(name = "UdpDiscovery")
public class UdpDiscoveryPlugin extends Plugin {
    private static final int DEFAULT_PORT = 4210;
    private static final int DEFAULT_TIMEOUT_MS = 3000;
    private static final int MAX_TIMEOUT_MS = 10000;
    private static final int RESEND_INTERVAL_MS = 1000;
    private static final int MAX_REPLIES = 32;

    @PluginMethod
    public void discover(final PluginCall call) {
        final String payload = call.getString("payload", "");
        final int port = call.getInt("port", DEFAULT_PORT);
        final int timeoutMs = Math.max(200, Math.min(call.getInt("timeoutMs", DEFAULT_TIMEOUT_MS), MAX_TIMEOUT_MS));
        if (payload == null || payload.isEmpty() || port < 1 || port > 65535) {
            call.reject("payload and a valid port are required");
            return;
        }

        new Thread(() -> {
            // Keyed by sender IP so a trainer answering a resend is only reported once.
            Map<String, String> replies = new LinkedHashMap<>();
            try (DatagramSocket socket = new DatagramSocket(0)) {
                socket.setBroadcast(true);
                socket.setSoTimeout(150);
                byte[] request = payload.getBytes(StandardCharsets.UTF_8);
                List<InetAddress> targets = broadcastTargets();
                long start = System.currentTimeMillis();
                long deadline = start + timeoutMs;
                long nextSend = start;
                byte[] buffer = new byte[1024];

                while (System.currentTimeMillis() < deadline && replies.size() < MAX_REPLIES) {
                    long now = System.currentTimeMillis();
                    if (now >= nextSend) {
                        for (InetAddress target : targets) {
                            try {
                                socket.send(new DatagramPacket(request, request.length, target, port));
                            } catch (Exception ignored) {
                                // One unreachable interface must not stop discovery on the others.
                            }
                        }
                        nextSend = now + RESEND_INTERVAL_MS;
                    }
                    try {
                        DatagramPacket packet = new DatagramPacket(buffer, buffer.length);
                        socket.receive(packet);
                        String ip = packet.getAddress().getHostAddress();
                        replies.put(ip, new String(packet.getData(), 0, packet.getLength(), StandardCharsets.UTF_8));
                    } catch (java.net.SocketTimeoutException ignored) {
                        // Poll again until the deadline.
                    }
                }
            } catch (Exception error) {
                call.reject("UDP discovery failed: " + error.getMessage());
                return;
            }

            JSArray servers = new JSArray();
            for (Map.Entry<String, String> entry : replies.entrySet()) {
                JSObject item = new JSObject();
                item.put("ip", entry.getKey());
                item.put("payload", entry.getValue());
                servers.put(item);
            }
            JSObject result = new JSObject();
            result.put("servers", servers);
            call.resolve(result);
        }, "udp-discovery").start();
    }

    /** Limited broadcast plus each active interface's own subnet broadcast (some phones drop 255.255.255.255). */
    private static List<InetAddress> broadcastTargets() {
        List<InetAddress> targets = new ArrayList<>();
        try {
            targets.add(InetAddress.getByName("255.255.255.255"));
            Enumeration<NetworkInterface> interfaces = NetworkInterface.getNetworkInterfaces();
            while (interfaces != null && interfaces.hasMoreElements()) {
                NetworkInterface networkInterface = interfaces.nextElement();
                if (!networkInterface.isUp() || networkInterface.isLoopback()) continue;
                for (InterfaceAddress address : networkInterface.getInterfaceAddresses()) {
                    InetAddress broadcast = address.getBroadcast();
                    if (broadcast != null && !targets.contains(broadcast)) targets.add(broadcast);
                }
            }
        } catch (Exception ignored) {
            // Fall back to whatever targets were collected.
        }
        return targets;
    }
}
