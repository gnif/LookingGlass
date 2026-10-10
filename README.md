# Looking Glass for Dual-GPU Laptops

A practical implementation guide and reference configuration for running a Windows 11 KVM guest on a hybrid-GPU laptop (Intel / AMD iGPU + NVIDIA / AMD dGPU) with low-latency display streaming via Looking Glass B7.

All working automation scripts, libvirt hooks, domain XMLs, and Windows provisioning batch files are provided in the [`laptop-setup/`](./laptop-setup/) directory.

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

## 1. Hardware Adaptation

Dual-GPU laptops route internal display panels through the integrated GPU (iGPU). When the dedicated GPU (dGPU) is passed to a VM, it has no physical display attached. Looking Glass uses an Indirect Display Driver (IDD) on the dGPU to capture frames directly into shared memory.

### A. Enable IOMMU in Bootloader

Append the appropriate parameter for your CPU to your bootloader configuration (e.g. `/etc/default/grub` under `GRUB_CMDLINE_LINUX`):

* **Intel CPUs:**
  ```text
  intel_iommu=on iommu=pt
  ```
* **AMD CPUs:**
  ```text
  amd_iommu=on iommu=pt
  ```

Regenerate your bootloader configuration:
* **Fedora / RHEL:** `sudo grub2-mkconfig -o /boot/grub2/grub.cfg` (or use `grubby`)
* **Arch Linux:** `sudo grub-mkconfig -o /boot/grub/grub.cfg`
* **Debian / Ubuntu:** `sudo update-grub`

### B. Verify IOMMU Group Isolation

Your dedicated GPU and its accompanying audio controller must reside in an isolated IOMMU group (or alone with its PCIe bridge). Run this snippet:

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

Identify:
1. **PCI Slot Addresses:** e.g., `01:00.0` (VGA) and `01:00.1` (Audio).
2. **Vendor & Device IDs:** e.g., `10de:28e0` (NVIDIA dGPU) and `10de:22be` (NVIDIA Audio).

Update `laptop-setup/host/hooks/qemu` and `laptop-setup/host/libvirt/win11.xml` to match your hardware addresses.

---

## 2. Linux Distribution Setup

### A. Fedora / RHEL

1. **Install Virtualization Stack:**
   ```bash
   sudo dnf install -y qemu-kvm libvirt edk2-ovmf virt-manager
   sudo systemctl enable --now libvirtd virtqemud
   ```

2. **User Permissions (Passwordless VM Control):**
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
   sudo cp laptop-setup/host/systemd/10-looking-glass.conf /etc/tmpfiles.d/
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

3. **Shared Memory Tmpfiles:**
   Place `10-looking-glass.conf` in `/etc/tmpfiles.d/` with the appropriate user and group:
   ```text
   f /dev/shm/looking-glass 0660 <your-username> kvm -
   ```
   Apply with `sudo systemd-tmpfiles --create /etc/tmpfiles.d/10-looking-glass.conf`. No SELinux relabeling is required on default Arch installations.

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
   Allow QEMU access to the shared memory file by adding `/dev/shm/looking-glass rw,` to `/etc/apparmor.d/local/abstractions/libvirt-qemu`:
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

## 3. Directory Layout

The [`laptop-setup/`](./laptop-setup/) folder contains production configurations:

```text
laptop-setup/
├── host/
│   ├── hooks/
│   │   └── qemu                     # Libvirt hook: dynamic unbind/bind of mobile dGPU
│   ├── systemd/
│   │   └── 10-looking-glass.conf    # tmpfiles.d shared memory definition
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

## 4. Libvirt Domain XML Highlights (`win11.xml`)

Key sections required in `win11.xml`:

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
   *Removing the VirtIO/QXL video adapter prevents Windows from detecting an emulated secondary display. All desktop rendering is forced onto the NVIDIA dGPU / Looking Glass IDD monitor, preventing dual-screen blanking.*

5. **PCI Hostdevs:**
   Attach both your dGPU VGA controller and Audio controller as `hostdev` entries matching your `lspci` bus numbers.

---

## 5. Windows 11 Guest Provisioning

Transfer the scripts in `laptop-setup/guest-windows/` into the VM (via a shared VirtIO-FS mount, USB drive, or network share):

1. **Install Virtual Display Driver (IDD):**
   Run the signed `looking-glass-idd-setup.exe` (must match the Looking Glass client build version). This creates the virtual display adapter on the dGPU.
2. **Install IVSHMEM Driver:**
   Run `install-ivshmem.bat` (Run as Administrator) to register `ivshmem.inf` with `pnputil`.
3. **Disable Sleep & Display Timeouts (Critical):**
   Run `disable-sleep.bat` (Run as Administrator). In KVM, if Windows enters display standby or sleep, QEMU maps ACPI sleep to shutdown, killing the VM and causing Looking Glass to disconnect.
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
* **`Scroll Lock`**: Alternative default escape key if configured.
