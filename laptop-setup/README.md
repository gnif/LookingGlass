# Fedora Windows 11 VFIO GPU Passthrough with Looking Glass B7

Panduan lengkap dan kumpulan konfigurasi/skrip untuk menjalankan **Windows 11 VM** di atas **Fedora Linux** dengan **GPU Passthrough (NVIDIA RTX 4060 Mobile)** dan penampil layar berperforma tinggi **Looking Glass B7**.

---

## 💻 Spesifikasi Sistem
* **Host OS:** Fedora Linux (Kernel 7.x, Wayland / GNOME Shell)
* **CPU:** Intel Core i7-13650HX (Intel Raptor Lake UHD Graphics untuk display Linux host)
* **dGPU:** NVIDIA GeForce RTX 4060 Laptop GPU (10de:28e0) & Audio (10de:22be)
* **Hypervisor:** QEMU/KVM + Libvirt (UEFI OVMF Secure Boot, TPM 2.0 CRB)
* **Guest OS:** Windows 11 Home / Pro

---

## 📁 Struktur Repositori

```text
├── README.md
├── host/
│   ├── hooks/
│   │   └── qemu                          # Libvirt hook: Dynamic GPU switcher (unbind/bind dGPU)
│   ├── systemd/
│   │   └── 10-looking-glass.conf         # tmpfiles.d untuk shared memory /dev/shm/looking-glass
│   ├── config/
│   │   ├── looking-glass-client.ini      # Konfigurasi Looking Glass Client (SPICE input, EGL double-buffer)
│   │   └── looking-glass-client.desktop  # Desktop entry launcher untuk GNOME/KDE
│   ├── scripts/
│   │   └── looking-glass-client          # Wrapper script: otomatis start VM saat aplikasi dibuka
│   └── libvirt/
│       └── win11.xml                     # XML domain VM (IVSHMEM, hostdev GPU, video none, SPICE 5900)
└── guest-windows/
    ├── setup_looking_glass.bat           # Skrip 1-klik: pasang host, optimalkan power, aktifkan RDP
    ├── matikan_sleep.bat                 # Nonaktifkan Sleep, Hibernasi, & Screen Timeout permanen
    ├── setup_autologin.bat               # Buka kunci centang Auto-Login di netplwiz Windows 11
    ├── install_ivshmem.bat               # Pasang driver resmi Red Hat IVSHMEM
    ├── reset_ivshmem.bat                 # Restart PnP device IVSHMEM jika terjadi error mapping 0x00000224
    └── cek_status_host.bat               # Diagnosa service host dan penyalin log ke shared folder
```

---

## 🚀 Panduan Pemasangan

### 1. Konfigurasi Host Linux

#### A. Kernel Command Line (IOMMU)
Pastikan parameter IOMMU aktif di bootloader:
```bash
# Tambahkan ke GRUB_CMDLINE_LINUX di /etc/default/grub:
intel_iommu=on iommu=pt
```

#### B. Hak Akses Libvirt Tanpa Password
Agar VM bisa di-start otomatis tanpa pop-up password:
```bash
sudo usermod -aG libvirt,kvm $USER
```
*(Wajib Log out dan Log in kembali agar perubahan grup aktif).*

#### C. Shared Memory & SELinux
1. Pasang konfigurasi tmpfiles:
   ```bash
   sudo cp host/systemd/10-looking-glass.conf /etc/tmpfiles.d/
   sudo systemd-tmpfiles --create /etc/tmpfiles.d/10-looking-glass.conf
   ```
2. Tetapkan izin SELinux permanen:
   ```bash
   sudo semanage fcontext -a -t svirt_image_t '/dev/shm/looking-glass'
   sudo restorecon -v /dev/shm/looking-glass
   ```

#### D. Dynamic GPU Switcher Hook (Opsional)
Pasang hook libvirt untuk melepas dGPU dari driver host ke vfio-pci saat VM nyala:
```bash
sudo mkdir -p /etc/libvirt/hooks
sudo cp host/hooks/qemu /etc/libvirt/hooks/
sudo chmod +x /etc/libvirt/hooks/qemu
```

#### E. Client Looking Glass & Launcher Otomatis
1. Salin konfigurasi INI:
   ```bash
   cp host/config/looking-glass-client.ini ~/.looking-glass-client.ini
   ```
2. Salin wrapper script & binary ke user PATH:
   ```bash
   cp host/scripts/looking-glass-client ~/.local/bin/looking-glass-client
   chmod +x ~/.local/bin/looking-glass-client
   ```
3. Salin desktop entry agar muncul di menu aplikasi:
   ```bash
   cp host/config/looking-glass-client.desktop ~/.local/share/applications/
   update-desktop-database ~/.local/share/applications/
   ```

---

### 2. Konfigurasi VM Libvirt (`win11.xml`)
Poin penting yang wajib ada di XML VM:
1. **Memory Backing:**
   ```xml
   <memoryBacking>
     <source type='memfd'/>
     <access mode='shared'/>
   </memoryBacking>
   ```
2. **IVSHMEM Device (Looking Glass):**
   ```xml
   <shmem name='looking-glass'>
     <model type='ivshmem-plain'/>
     <size unit='M'>32</size>
   </shmem>
   ```
3. **SPICE Port 5900 (Input & Clipboard):**
   ```xml
   <graphics type='spice' port='5900' autoport='no'>
     <listen type='address' address='127.0.0.1'/>
     <image compression='off'/>
   </graphics>
   ```
4. **Display Solusi Permanen (Tanpa Layar Virtual Ganda):**
   ```xml
   <video>
     <model type='none'/>
   </video>
   ```

---

### 3. Konfigurasi di Windows 11 Guest
Semua file `.bat` di folder `guest-windows/` bisa ditaruh di drive VirtIO-FS shared folder:

1. **Pasang Driver Virtual Display (IDD):**
   Unduh dan jalankan `looking-glass-idd-setup.exe` resmi (Build B7) dari Desktop Windows untuk membuat monitor virtual pada dGPU.
2. **Pasang Driver IVSHMEM:**
   Jalankan `install_ivshmem.bat` (Run as administrator).
3. **Matikan Sleep Permanen:**
   Jalankan `matikan_sleep.bat` (Run as administrator) agar Windows tidak pernah masuk mode sleep/timeout yang mematikan VM.
4. **Aktifkan Auto-Login:**
   Jalankan `setup_autologin.bat` (Run as administrator), hilangkan centang pada netplwiz, dan masukkan password akun Windows.

---

## 🎮 Shortcut Penting
* **Tombol `F12`:** Melepas atau mengunci (*grab/ungrab*) kursor mouse & keyboard antara Linux host dan Windows VM.
