# Laptop Dual-GPU Windows 11 VFIO + Looking Glass B7 Setup

A practical configuration reference for running a Windows 11 KVM guest on a hybrid-GPU laptop (Intel iGPU + NVIDIA dGPU) with low-latency frame streaming via Looking Glass B7.

## Architecture

* **Host:** Fedora Linux / Wayland (GNOME Shell)
* **Hardware:** Lenovo laptop, Intel Core i7-13650HX (UHD Graphics driving the physical display) + NVIDIA GeForce RTX 4060 Mobile (`10de:28e0`, audio `10de:22be`)
* **Guest:** Windows 11 (OVMF Secure Boot, TPM 2.0 emulator)
* **Passthrough Strategy:** Dynamic unbind/bind of the NVIDIA mobile dGPU via libvirt hooks. Host iGPU keeps the Linux desktop running while dGPU is passed to the guest.
* **Display Mechanism:** Looking Glass IDD driver + IVSHMEM shared memory device. The virtual VirtIO/QXL video adapter is explicitly set to `none` to avoid dual-monitor synchronization traps.

## Directory Structure

```text
laptop-setup/
├── host/
│   ├── hooks/
│   │   └── qemu                     # Libvirt hook: dynamic unbind from nvidia and bind to vfio-pci
│   ├── systemd/
│   │   └── 10-looking-glass.conf    # tmpfiles.d definition for /dev/shm/looking-glass
│   ├── config/
│   │   ├── looking-glass-client.ini # Stable client config (SPICE input, locked resolution, double buffer)
│   │   └── looking-glass-client.desktop # Desktop entry launcher
│   ├── scripts/
│   │   └── looking-glass-client     # Wrapper: starts VM automatically if offline, then opens client
│   └── libvirt/
│       └── win11.xml                # Working libvirt domain XML
└── guest-windows/
    ├── setup-host.bat               # Initial provisioning: installs host, enables RDP, disables sleep
    ├── disable-sleep.bat            # Completely disables sleep/hibernate/monitor timeouts
    ├── enable-autologin.bat         # Unhides netplwiz passwordless checkbox in Windows 11
    ├── install-ivshmem.bat          # Automated pnputil driver install for Red Hat IVSHMEM
    ├── reset-ivshmem.bat            # Power-cycles the IVSHMEM PnP device (fixes 0x00000224 error)
    └── host-diagnostics.bat         # Queries service status and exports %ProgramData% log
```

## Setup Guide

### 1. Host Configuration

#### Kernel Parameters
Ensure IOMMU is enabled on your host bootloader:
```text
intel_iommu=on iommu=pt
```

#### User Permissions
Allow your standard user account to manage system libvirt domains without Polkit password prompts:
```bash
sudo usermod -aG libvirt,kvm $USER
```
*Note: A log out / log in cycle is required for group membership to take effect.*

#### Shared Memory & SELinux
Create the tmpfiles rule and set the persistent SELinux file context:
```bash
sudo cp host/systemd/10-looking-glass.conf /etc/tmpfiles.d/
sudo systemd-tmpfiles --create /etc/tmpfiles.d/10-looking-glass.conf

sudo semanage fcontext -a -t svirt_image_t '/dev/shm/looking-glass'
sudo restorecon -v /dev/shm/looking-glass
```

#### Dynamic GPU Switcher Hook
Install the QEMU hook script to handle unbinding the NVIDIA dGPU from `nvidia` and binding it to `vfio-pci` when the VM starts:
```bash
sudo mkdir -p /etc/libvirt/hooks
sudo cp host/hooks/qemu /etc/libvirt/hooks/
sudo chmod +x /etc/libvirt/hooks/qemu
```

#### Client Configuration & Auto-Start Wrapper
1. Copy the client configuration:
   ```bash
   cp host/config/looking-glass-client.ini ~/.looking-glass-client.ini
   ```
2. Place the auto-start wrapper and client binary:
   ```bash
   cp host/scripts/looking-glass-client ~/.local/bin/looking-glass-client
   chmod +x ~/.local/bin/looking-glass-client
   ```
3. Install the desktop launcher:
   ```bash
   cp host/config/looking-glass-client.desktop ~/.local/share/applications/
   update-desktop-database ~/.local/share/applications/
   ```

### 2. Libvirt Domain XML Highlights (`win11.xml`)

* **Shared Memory Backing:**
  ```xml
  <memoryBacking>
    <source type='memfd'/>
    <access mode='shared'/>
  </memoryBacking>
  ```
* **IVSHMEM Device:**
  ```xml
  <shmem name='looking-glass'>
    <model type='ivshmem-plain'/>
    <size unit='M'>32</size>
  </shmem>
  ```
* **SPICE Port 5900 (Input / Mouse Tablet / Clipboard):**
  ```xml
  <graphics type='spice' port='5900' autoport='no'>
    <listen type='address' address='127.0.0.1'/>
    <image compression='off'/>
  </graphics>
  ```
* **Zeroing the Virtual Video Adapter:**
  ```xml
  <video>
    <model type='none'/>
  </video>
  ```
  *Removing the VirtIO/QXL video adapter forces Windows to render exclusively to the NVIDIA GPU / Looking Glass IDD virtual monitor, preventing dual-screen blanking.*

### 3. Windows 11 Guest Provisioning

Run these from an elevated command prompt inside the guest:

1. **Install Looking Glass IDD Driver:** Run the signed `looking-glass-idd-setup.exe` to create the virtual monitor on the NVIDIA dGPU.
2. **Install IVSHMEM Driver:** Run `install-ivshmem.bat` to install `ivshmem.inf` using `pnputil`.
3. **Disable Sleep & Display Timeouts:** Run `disable-sleep.bat`. If Windows enters display standby or sleep, QEMU will treat ACPI sleep as a shutdown, terminating the session.
4. **Enable Auto-Login:** Run `enable-autologin.bat`, uncheck the password requirement box in `netplwiz`, and enter your account password.

## Hotkeys

* **`F12`**: Toggle mouse and keyboard capture between host Linux desktop and Windows 11 VM.
