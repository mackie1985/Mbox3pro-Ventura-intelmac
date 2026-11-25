Mbox 3 Pro driver for macOS 13 Ventura (Intel Mac; Apple Silicon is a BETA that has not been tested yet)

An unofficial driver for the Avid Mbox 3 Pro (FireWire). It is not made, endorsed or supported by Avid.

This installer puts these in place:
  - the Audio Server plug-in  /Library/Audio/Plug-Ins/HAL/Mbox3.driver
  - the daemon                /usr/local/libexec/mbox3d  (a LaunchDaemon that starts at boot)
  - the control panel page    /usr/local/share/mbox3pro/ui/index.html
  - the uninstall script      /usr/local/share/mbox3pro/uninstall.sh

The sound system (coreaudiod) is restarted at the end, so audio stops for a moment. Quit your audio apps first.

After the installation:
  - Choose "Mbox 3 Pro" as the output / input device in System Settings > Sound or in your DAW.
  - The control panel is at http://127.0.0.1:47310/ (open it in a browser on this Mac).
  - Log file: /var/log/mbox3d.log

To remove it:  sudo /usr/local/share/mbox3pro/uninstall.sh   (then power-cycle the Mbox 3 Pro)
