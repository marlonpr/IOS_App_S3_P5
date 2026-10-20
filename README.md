ESP-IDF template app
====================

This is a template application to be used with [Espressif IoT Development Framework](https://github.com/espressif/esp-idf).

Please check [ESP-IDF docs](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/index.html) for getting started instructions.

*Code in this repository is in the Public Domain (or CC0 licensed, at your option.)
Unless required by applicable law or agreed to in writing, this
software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied.*
## Configurable Wi-Fi DHCP acceptance policy

Bad-IP rejection is enabled for Wi-Fi. The accepted IPv4 network, netmask, and
gateway can be changed under `idf.py menuconfig -> Clock Network Configuration`.
Defaults are `192.168.0.0`, `255.255.0.0`, and `192.168.0.1`. See
`BAD_IP_REJECTION_S3.md` for details.


## Network IP diagnostic screen

Hold **DOWN** for 10 seconds to display the current Wi-Fi/Ethernet IPv4 address for up to 20 seconds. Press **DOWN** once on that screen to change the saved network boot mode; press **MENU** or **UP**, or wait 20 seconds, to exit without changing network mode. See `NETWORK_IP_DIAGNOSTIC.md` for details.
