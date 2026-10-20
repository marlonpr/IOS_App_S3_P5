# ESP32-S3 Wi-Fi bad-IP rejection with menuconfig policy

The Wi-Fi DHCP acceptance rule is configurable from ESP-IDF `menuconfig`.
The default policy remains the validated factory LAN:

- Network: `192.168.0.0`
- Netmask: `255.255.0.0`
- Gateway: `192.168.0.1`

Open:

```text
idf.py menuconfig
  -> Clock Network Configuration
     -> Accepted Wi-Fi IPv4 network
     -> Accepted Wi-Fi IPv4 netmask
     -> Accepted Wi-Fi IPv4 gateway
```

For example, if a future valid LAN becomes `10.20.0.0/16` with gateway
`10.20.0.1`, set:

```text
Accepted Wi-Fi IPv4 network = 10.20.0.0
Accepted Wi-Fi IPv4 netmask = 255.255.0.0
Accepted Wi-Fi IPv4 gateway = 10.20.0.1
```

A DHCP lease is accepted only when all three conditions hold:

1. The lease netmask exactly matches the configured netmask.
2. The lease gateway exactly matches the configured gateway.
3. `(lease_ip & configured_netmask) == configured_network`.

If the lease is rejected, the existing network services remain disabled, the
firmware waits about one second, restarts the DHCP client, and retries until a
valid lease is received. Ethernet behavior is unchanged.

The firmware validates the three menuconfig strings during Wi-Fi startup. If a
value is not a valid IPv4 address, the configured network is not a proper
network base for the configured mask, or the gateway is outside that network,
Wi-Fi startup fails with a clear `CLOCK_WIFI` error rather than silently
accepting an unsafe policy.
