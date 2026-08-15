# Linux packaging

Manual install (until this is wrapped in a real deb/rpm/AUR package):

```sh
cmake --preset linux-release
cmake --build --preset linux-release
sudo install -Dm755 build/release/src/daemon/steamlesscontrollerd /usr/bin/steamlesscontrollerd
sudo install -Dm755 build/release/src/cli/steamlessctl            /usr/bin/steamlessctl

sudo install -Dm644 packaging/linux/70-steamlesscontroller.rules /usr/lib/udev/rules.d/70-steamlesscontroller.rules
sudo install -Dm644 packaging/linux/steamlesscontroller-modules.conf /usr/lib/modules-load.d/steamlesscontroller.conf
sudo install -Dm644 packaging/linux/steamlesscontroller.service /usr/lib/systemd/user/steamlesscontroller.service

sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=hidraw --subsystem-match=misc
sudo modprobe uinput

systemctl --user daemon-reload
systemctl --user enable --now steamlesscontroller
steamlessctl status
```

If `doctor` reports `uinputAvailable: false` or the hidraw nodes aren't
accessible, the udev rules likely haven't taken effect yet for an
already-plugged controller — re-run the `udevadm trigger` line above (no
replug or reboot needed).
