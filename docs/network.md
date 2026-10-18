# Network setup

The camera ships with `192.168.2.204/24` (printed on its label). The computer running the
driver needs an address in the same subnet, or the camera gets a new one.

## Option 1: an extra address on the computer

Quick test, lost on reboot:

```bash
sudo ip addr add 192.168.2.10/24 dev eth0      # your interface: ip -br link
ping -c 3 192.168.2.204
```

Permanent, with netplan (`/etc/netplan/60-hertzino.yaml`), keeping the interface's other
addresses:

```yaml
network:
  version: 2
  ethernets:
    eth0:
      addresses:
        - 192.168.2.10/24
```

```bash
sudo netplan apply
```

## Option 2: move the camera into your subnet

1. Reach it once with option 1 and open `http://192.168.2.204` (user `admin`, password
   `123456` unless changed).
2. *System Configuration → Network Settings*: set IP, mask and gateway, press **Set**. The
   page logs out; continue at the new address.
3. Launch with `host:=<new address>`.

## Ports

| Port | Use |
|---|---|
| 80 | HTTP API and WebSocket streams (`port:=`, `ws_port`) |
| 443 | RTSP video (`rtsp_port:=`; the camera's `video_port`) |

## Check before launching

```bash
curl -s -X POST -H 'Content-Type: application/json' http://192.168.2.204/login \
  -d "{\"username\": \"admin\", \"password\": \"$(echo -n '123456lfxs' | md5sum | cut -d' ' -f1)\"}"
# {"token": "..."}
```

Replace `123456` with your password. A 401 means a wrong password; no answer means the
address or the subnet is wrong.
