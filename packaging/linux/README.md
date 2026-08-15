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

## Disable Steam Input for the physical controller, not the virtual pad

With the daemon running and Steam open, Steam sees **two** controllers:

- the **physical Steam Controller** — the real hidraw device this daemon
  also has open, to disable lizard mode and forward its input; and
- the **virtual Xbox 360 pad** this daemon creates via `uinput`
  (`045e:028e`) — a synthetic joystick device with no real hardware behind
  it, built to be seen as an ordinary Xbox controller by Steam and every
  game.

hidraw has no exclusive-open concept, so nothing stops both this daemon
and Steam's own Steam Input from opening and writing to the physical
controller at the same time — each sending its own feature reports and
rumble commands to the same device. `ControllerManager` deliberately does
not refuse to run just because Steam is present (there is nothing on
Linux to escalate to if it did — see `IDeviceReclaimer`), so avoiding that
conflict is on Steam's own per-controller setting, not something this
daemon can arbitrate from its side.

**In Steam: Settings → Controller → General Controller Settings**, turn
**off** "Steam Input" for the physical Steam Controller entry (listed
under its own vendor id, `28de`) — that's what stops Steam from opening
the same hidraw node this daemon holds. Leave Steam Input for the Xbox
360 entry (`045e:028e`) as you prefer; it isn't touching real hardware
either way, so it can't conflict with anything.
