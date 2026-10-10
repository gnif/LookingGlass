# Laptop Dual-GPU Windows 11 VFIO + Looking Glass B7

Practical configuration, scripts, and troubleshooting guide for running a Windows 11 KVM guest on a hybrid/dual-GPU laptop with Looking Glass B7 display streaming.

---

## Architecture Overview

```text
+----------------------------------------------------------------------------+
| Linux Host Desktop (Intel / AMD iGPU)                                     |
|                                                                            |
|  +---------------------------+       +----------------------------------+  |
|  | Looking Glass Client      | <---  | /dev/shm/looking-glass (IVSHMEM) |  |
|  | (Rendering via EGL/Wayland|       | (Shared 32MB RAM buffer)         |  |
|  +---------------------------+       +----------------------------------+  |
|               | (SPICE Input: Mouse & Keys)           ^                    |
|               v                                       | Frame stream       |
|  +----------------------------------------------------+-----------------+  |
|  | QEMU / KVM Virtual Machine                                           |  |
|  |                                                                      |  |
|  |  [ Windows 11 Guest ]                                                |  |
|  |  +----------------------------------------------------------------+  |  |
|  |  | NVIDIA / AMD dGPU (Passed through via VFIO)                     |  |  |
|  |  |  -> Looking Glass IDD Driver (Virtual Monitor on dGPU)          |  |  |
|  |  |  -> Looking Glass Host / IDD writes frames to IVSHMEM          |  |  |
|  |  +----------------------------------------------------------------+  |  |
|  |  | Emulated USB Tablet / SPICE input (receives host cursor)        |  |  |
|  |  | No virtual video card (video model = none; prevents black screen) |  |
|  +--+-------------------------------------------------------------------+--+
+----------------------------------------------------------------------------+
```

---

## ⚡ Quick Start: Automated Setup Script

An interactive setup script is provided to detect your specific hardware and Linux distribution automatically:

```bash
git clone https://github.com/julsanjh/LookingGlass-fordualGPU-Laptop.git
cd LookingGlass-fordualGPU-Laptop
chmod +x setup.sh
./setup.sh
```

**What `setup.sh` does automatically:**
1. Detects your distribution (`Fedora`, `Arch`, `Ubuntu/Debian`, `openSUSE`) and package manager.
2. Identifies your CPU vendor and checks IOMMU boot parameters.
3. Scans PCI devices, detects your dedicated GPU & Audio IDs, and verifies IOMMU group isolation.
4. Adds your user account to the `libvirt` and `kvm` groups.
5. Configures `/dev/shm/looking-glass` tmpfiles and applies SELinux (`svirt_image_t`) or AppArmor rules.
6. Generates a customized `/etc/libvirt/hooks/qemu` GPU switcher hook matching your hardware PCI bus.
7. Installs the Looking Glass client configuration and application launcher.

---

## 1. Hardware Adaptation

Laptops differ by CPU architecture and PCI bus layout. Adjust the configurations below for your hardware before starting.

### A. Enable IOMMU in Bootloader

* **Intel CPUs:**
  ```text
  intel_iommu=on iommu=pt
  ```
* **AMD CPUs:**
  ```text
  amd_iommu=on iommu=pt
  ```

Apply these to your kernel command-line (e.g., `/etc/default/grub` under `GRUB_CMDLINE_LINUX` or your bootloader config) and regenerate your boot configuration:
* **Fedora / RHEL:** `sudo grub2-mkconfig -o /boot/grub2/grub.cfg` (or let `grubby` update it)
* **Arch Linux:** `sudo grub-mkconfig -o /boot/grub/grub.cfg`
* **Debian / Ubuntu:** `sudo update-grub`

### B. Verify IOMMU Group Isolation

Your dedicated GPU and its accompanying audio controller must reside in an isolated IOMMU group (or alone with its PCIe bridge). Run this snippet to inspect your groups:

```bash
for g in $(find /sys/kernel/iommu_groups/ -maxdepth 1 -mindepth 1 -type d | sort -V); do
    echo "=== IOMMU Group ${g##*/} ==="
    for d in "$g"/devices/*; do
        [ -e "$d" ] && lspci -nms "${d##*/}"
    done
done
```

### C. Identify PCI Bus Addresses & Vendor IDs

Run:
```bash
lspci -nnk | grep -E "VGA|3D|Audio"
```

Note:
1. **PCI Slot Addresses:** e.g., `01:00.0` (VGA) and `01:00.1` (Audio).
2. **Vendor & Device IDs:** e.g., `10de:28e0` (NVIDIA dGPU) and `10de:22be` (NVIDIA Audio).

Update `host/hooks/qemu` and `host/libvirt/win11.xml` with your specific addresses and IDs.

---

## 2. Linux Distribution Differences

### A. Fedora / RHEL

1. **Install Virtualization Packages:**
   ```bash
   sudo dnf install -y qemu-kvm libvirt edk2-ovmf virt-manager
   sudo systemctl enable --now libvirtd virtqemud
   ```

2. **User Permissions (Passwordless VM Management):**
   ```bash
   sudo usermod -aG libvirt,kvm $USER
   ```
   *Log out and log back in for group membership to take effect.*

3. **SELinux Permissions (Mandatory on Fedora/RHEL):**
   QEMU will be denied access to `/dev/shm/looking-glass` unless labeled with `svirt_image_t`:
   ```bash
   sudo semanage fcontext -a -t svirt_image_t '/dev/shm/looking-glass'
   sudo restorecon -v /dev/shm/looking-glass
   ```

4. **Shared Memory Tmpfiles:**
   ```bash
   sudo cp host/systemd/10-looking-glass.conf /etc/tmpfiles.d/
   sudo systemd-tmpfiles --create /etc/tmpfiles.d/10-looking-glass.conf
   ```

---

### B. Arch Linux / Manjaro / EndeavourOS

1. **Install Packages:**
   ```bash
   sudo pacman -S qemu-desktop libvirt edk2-ovmf virt-manager dnsmasq iptables-nft
   sudo systemctl enable --now libvirtd
   ```

2. **User Permissions:**
   ```bash
   sudo usermod -aG libvirt,kvm $USER
   ```

3. **SELinux:** Not present by default on Arch. Standard tmpfiles permission (`0660` with owner `$USER:kvm`) is sufficient.

4. **Shared Memory Tmpfiles:**
   Place `10-looking-glass.conf` in `/etc/tmpfiles.d/` with the appropriate user and group:
   ```text
   f /dev/shm/looking-glass 0660 <your-username> kvm -
   ```
   Apply with `sudo systemd-tmpfiles --create /etc/tmpfiles.d/10-looking-glass.conf`.

---

### C. Ubuntu / Debian / Pop!_OS

1. **Install Packages:**
   ```bash
   sudo apt update
   sudo apt install -y qemu-system-x86 libvirt-daemon-system ovmf virt-manager
   sudo systemctl enable --now libvirtd
   ```

2. **User Permissions:**
   ```bash
   sudo usermod -aG libvirt,kvm $USER
   ```

3. **AppArmor Permissions:**
   Ubuntu uses AppArmor to confine libvirt instances. Allow QEMU access to the shared memory file:
   Add `/dev/shm/looking-glass rw,` to `/etc/apparmor.d/local/abstractions/libvirt-qemu`:
   ```bash
   echo "/dev/shm/looking-glass rw," | sudo tee -a /etc/apparmor.d/local/abstractions/libvirt-qemu
   sudo systemctl restart apparmor
   ```

4. **Shared Memory Tmpfiles:**
   Set owner to `<your-username>:kvm` in `/etc/tmpfiles.d/10-looking-glass.conf`:
   ```text
   f /dev/shm/looking-glass 0660 <your-username> kvm -
   ```
   Apply with `sudo systemd-tmpfiles --create /etc/tmpfiles.d/10-looking-glass.conf`.

---

## 3. Directory Layout in this Repository

```text
laptop-setup/
├── host/
│   ├── hooks/
│   │   └── qemu                     # Libvirt hook: dynamically unbinds dGPU from host and binds to vfio-pci
│   ├── systemd/
│   │   └── 10-looking-glass.conf    # tmpfiles.d definition for /dev/shm/looking-glass
│   ├── config/
│   │   ├── looking-glass-client.ini # Production client config (SPICE input, locked res, double buffer)
│   │   └── looking-glass-client.desktop # Desktop entry launcher
│   ├── scripts/
│   │   └── looking-glass-client     # Wrapper: starts VM if offline, then execs client binary
│   └── libvirt/
│       └── win11.xml                # Domain XML (IVSHMEM, dGPU hostdevs, video=none, SPICE 5900)
└── guest-windows/
    ├── setup-host.bat               # Initial provisioning: installs host, enables RDP, disables sleep
    ├── disable-sleep.bat            # Completely disables sleep/hibernate/monitor timeouts
    ├── enable-autologin.bat         # Unhides netplwiz passwordless checkbox in Windows 11
    ├── install-ivshmem.bat          # Automated pnputil driver install for Red Hat IVSHMEM
    ├── reset-ivshmem.bat            # Power-cycles the IVSHMEM PnP device (fixes 0x00000224 error)
    └── host-diagnostics.bat         # Queries service status and exports %ProgramData% log
```

---

## 4. Libvirt Domain XML Requirements

When configuring your domain XML via `virsh edit <vm-name>`, ensure the following entries are present:

1. **Shared Memory Backing (Required for IVSHMEM):**
   ```xml
   <memoryBacking>
     <source type='memfd'/>
     <access mode='shared'/>
   </memoryBacking>
   ```

2. **IVSHMEM Device Definition:**
   ```xml
   <shmem name='looking-glass'>
     <model type='ivshmem-plain'/>
     <size unit='M'>32</size>
   </shmem>
   ```
   *(32 MB is sufficient for 1080p; use 64 MB for 1440p).*

3. **SPICE Port 5900 for Input Injection:**
   ```xml
   <graphics type='spice' port='5900' autoport='no'>
     <listen type='address' address='127.0.0.1'/>
     <image compression='off'/>
   </graphics>
   ```

4. **Disable Emulated Video Adapter (Crucial):**
   ```xml
   <video>
     <model type='none'/>
   </video>
   ```
   *Setting `model type='none'` prevents Windows from detecting an emulated secondary display. All desktop rendering is forced onto the NVIDIA dGPU / Looking Glass IDD monitor.*

5. **PCI Hostdevs:**
   Attach both your dGPU VGA controller and Audio controller as `hostdev` entries matching your `lspci` bus numbers.

---

## 5. Windows 11 Guest Provisioning

Transfer the scripts in `guest-windows/` into the VM (via a shared VirtIO-FS mount, USB drive, or network share):

1. **Install Virtual Display Driver (IDD):**
   Run the signed `looking-glass-idd-setup.exe` (must match the Looking Glass client build version). This creates the virtual display adapter on the dGPU.
2. **Install IVSHMEM Driver:**
   Run `install-ivshmem.bat` (Run as Administrator) to register `ivshmem.inf` with `pnputil`.
3. **Disable Sleep & Display Timeouts (Critical):**
   Run `disable-sleep.bat` (Run as Administrator). In KVM, if Windows enters display standby or sleep, QEMU maps ACPI sleep to shutdown, killing the VM and causing Looking Glass to go black.
4. **Enable Auto-Login:**
   Run `enable-autologin.bat` (Run as Administrator). Uncheck *"Users must enter a user name and password to use this computer"* in `netplwiz`, click Apply, and confirm your credentials.

---

## 6. Troubleshooting & Common Pitfalls

| Issue | Root Cause | Solution |
| :--- | :--- | :--- |
| **`Permission denied` opening `/dev/shm/looking-glass`** | SELinux or AppArmor blocking QEMU access. | Run `semanage fcontext -a -t svirt_image_t '/dev/shm/looking-glass'` (Fedora) or add AppArmor rule (Ubuntu). |
| **`DeviceIoControl Failed: 0x00000224`** | `ivshmem.sys` allows only one mapping. A previous or orphaned instance is still holding the device handle. | Run `guest-windows/reset-ivshmem.bat` in the guest to cycle the PnP device, or power-cycle the VM (`virsh destroy` + `virsh start`). |
| **Client says `"The host application seems to not be running"`** | Protocol version mismatch between the client binary and the Windows IDD/Host build. | Compile the client binary from the exact matching source commit as the Windows IDD driver artifact. |
| **Mouse cursor does not move despite window capture** | Looking Glass IDD driver only handles display; input requires SPICE. | Ensure SPICE is listening on `127.0.0.1:5900` in the XML, and `spice:enable=yes` is set in `~/.looking-glass-client.ini`. |
| **Display goes black after ~10 minutes of inactivity** | Windows idle timer triggered display sleep / standby, halting IDD frame generation. | Run `disable-sleep.bat` in the guest to disable standby, screen timeout, and hibernation permanently. |
| **Stuck at Windows Automatic Repair screen** | Forced VM shutdown triggered the Windows BCD dirty flag. | Press Enter in the Looking Glass window, select **Continue (Exit and continue to Windows 11)**. |

---

## 7. Controls & Hotkeys

* **`F12`**: Toggle mouse and keyboard capture between the Linux host desktop and the Windows 11 VM.
* **`Scroll Lock`** (alternative default escape key if configured): Release input capture.
