# Network IP Diagnostic Screen

The 10-second DOWN-button hold no longer changes the saved network mode immediately.

## User flow

1. Hold **DOWN** for 10 seconds.
2. The clock opens a 20-second network diagnostic screen.
   - `WIFI IP` or `ETH IP` identifies the selected/live interface.
   - The IPv4 address is split across two lines so a full address fits a 64x32 panel.
   - `NO IP` is shown when no address is available.
3. While the diagnostic screen is open:
   - Press **DOWN once** to perform the previous Ethernet/Wi-Fi saved-mode switch. The existing `NET: ...` / `REBOOT TO APPLY` confirmation remains unchanged.
   - Press **MENU** or **UP** to leave the screen without changing the saved network mode.
   - Press nothing for 20 seconds to leave the screen without changing the saved network mode.

The press that exits the diagnostic screen is consumed, so MENU/UP do not also invoke their normal display/menu actions.

## Diagnostic IP behavior

For Wi-Fi, the diagnostic value tracks the latest DHCP lease, including a lease that is being rejected by the configured LAN-policy mechanism. This makes a persistent wrong-VLAN address visible to the user while the firmware retries DHCP. Once a good lease arrives, the screen updates to that good address.

For Ethernet, the screen shows the current W5500 IPv4 address.
