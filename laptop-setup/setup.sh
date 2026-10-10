#!/usr/bin/env bash
#
# Interactive setup script for Dual-GPU Laptop VFIO + Looking Glass
# Detects hardware (CPU, iGPU, dGPU), distro package manager, and security modules.
#

set -e

# Terminal colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

info()  { echo -e "${BLUE}[INFO]${NC} $*"; }
ok()    { echo -e "${GREEN}[OK]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; }

if [ "$EUID" -eq 0 ]; then
    error "Do not run this script directly as root. Run as your standard user; sudo will be invoked when needed."
    exit 1
fi

CURRENT_USER="$USER"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=========================================================="
echo " Dual-GPU Laptop VFIO + Looking Glass Automated Setup"
echo "=========================================================="
echo

# 1. Distro Detection
info "Detecting Linux distribution..."
if [ -f /etc/os-release ]; then
    . /etc/os-release
    DISTRO_ID="$ID"
    DISTRO_LIKE="$ID_LIKE"
else
    DISTRO_ID="unknown"
fi

case "$DISTRO_ID" in
    fedora|rhel|centos)
        PKG_MGR="dnf"
        QEMU_GROUP="qemu"
        ;;
    arch|manjaro|endeavouros)
        PKG_MGR="pacman"
        QEMU_GROUP="kvm"
        ;;
    ubuntu|debian|pop)
        PKG_MGR="apt"
        QEMU_GROUP="kvm"
        ;;
    opensuse*)
        PKG_MGR="zypper"
        QEMU_GROUP="kvm"
        ;;
    *)
        if [[ "$DISTRO_LIKE" =~ (rhel|fedora) ]]; then
            PKG_MGR="dnf"
            QEMU_GROUP="qemu"
        elif [[ "$DISTRO_LIKE" =~ (debian|ubuntu) ]]; then
            PKG_MGR="apt"
            QEMU_GROUP="kvm"
        elif [[ "$DISTRO_LIKE" =~ arch ]]; then
            PKG_MGR="pacman"
            QEMU_GROUP="kvm"
        else
            PKG_MGR="unknown"
            QEMU_GROUP="kvm"
        fi
        ;;
esac
ok "Distribution identified: $PRETTY_NAME (Package manager: $PKG_MGR)"

# 2. CPU Architecture & IOMMU Check
info "Checking CPU architecture and IOMMU..."
CPU_VENDOR=$(grep -m1 'vendor_id' /proc/cpuinfo | awk '{print $3}')
CMDLINE=$(cat /proc/cmdline)

case "$CPU_VENDOR" in
    GenuineIntel)
        IOMMU_PARAM="intel_iommu=on iommu=pt"
        if echo "$CMDLINE" | grep -q "intel_iommu=on"; then
            ok "Intel CPU detected with IOMMU enabled."
        else
            warn "Intel CPU detected, but 'intel_iommu=on' is missing in kernel parameters."
            echo "       Add '$IOMMU_PARAM' to your bootloader (e.g. GRUB_CMDLINE_LINUX)."
        fi
        ;;
    AuthenticAMD)
        IOMMU_PARAM="amd_iommu=on iommu=pt"
        if echo "$CMDLINE" | grep -q "amd_iommu=on"; then
            ok "AMD CPU detected with IOMMU enabled."
        else
            warn "AMD CPU detected, but 'amd_iommu=on' is missing in kernel parameters."
            echo "       Add '$IOMMU_PARAM' to your bootloader (e.g. GRUB_CMDLINE_LINUX)."
        fi
        ;;
    *)
        warn "Unknown CPU vendor ($CPU_VENDOR). Verify IOMMU manually."
        ;;
esac

# 3. GPU Detection
info "Scanning PCI devices for GPUs..."
VGA_DEVS=$(lspci -nn | grep -E "VGA compatible controller|3D controller")
echo "$VGA_DEVS" | sed 's/^/  /'
echo

GPU_COUNT=$(echo "$VGA_DEVS" | wc -l)
if [ "$GPU_COUNT" -lt 2 ]; then
    warn "Only $GPU_COUNT display controller detected. Looking Glass requires an iGPU + dedicated dGPU."
    read -rp "Do you want to continue anyway? [y/N] " continue_single
    [[ "$continue_single" =~ ^[Yy]$ ]] || exit 1
fi

# Detect dedicated GPU (usually 3D controller or secondary VGA)
DGPU_LINE=$(echo "$VGA_DEVS" | grep -iE "NVIDIA|GeForce|Radeon RX|Navi" | tail -n1)
if [ -z "$DGPU_LINE" ]; then
    DGPU_LINE=$(echo "$VGA_DEVS" | tail -n1)
fi

DGPU_PCI_SHORT=$(echo "$DGPU_LINE" | awk '{print $1}')
DGPU_PCI="0000:$DGPU_PCI_SHORT"
DGPU_IDS=$(echo "$DGPU_LINE" | grep -oP '\[\K[0-9a-fA-F]{4}:[0-9a-fA-F]{4}(?=\])' | tail -n1 | tr ':' ' ')

# Find matching audio controller on the same bus or function 1
AUDIO_LINE=$(lspci -nn | grep -i "Audio" | grep "${DGPU_PCI_SHORT%.*}" | head -n1 || true)
if [ -n "$AUDIO_LINE" ]; then
    AUDIO_PCI_SHORT=$(echo "$AUDIO_LINE" | awk '{print $1}')
    AUDIO_PCI="0000:$AUDIO_PCI_SHORT"
    AUDIO_IDS=$(echo "$AUDIO_LINE" | grep -oP '\[\K[0-9a-fA-F]{4}:[0-9a-fA-F]{4}(?=\])' | tail -n1 | tr ':' ' ')
else
    AUDIO_PCI=""
    AUDIO_IDS=""
fi

echo "Detected target passthrough devices:"
echo "  dGPU  : $DGPU_PCI ($DGPU_IDS) -> $(echo "$DGPU_LINE" | cut -d: -f3-)"
if [ -n "$AUDIO_PCI" ]; then
    echo "  Audio : $AUDIO_PCI ($AUDIO_IDS) -> $(echo "$AUDIO_LINE" | cut -d: -f3-)"
fi
echo

# 4. Check IOMMU Group Isolation
if [ -d "/sys/bus/pci/devices/$DGPU_PCI/iommu_group" ]; then
    GROUP_PATH=$(readlink "/sys/bus/pci/devices/$DGPU_PCI/iommu_group")
    GROUP_ID=$(basename "$GROUP_PATH")
    info "dGPU is assigned to IOMMU Group $GROUP_ID:"
    for dev in "/sys/kernel/iommu_groups/$GROUP_ID/devices/"*; do
        [ -e "$dev" ] && lspci -nms "$(basename "$dev")" | sed 's/^/    /'
    done
    ok "IOMMU group verified."
else
    warn "Cannot resolve IOMMU group for $DGPU_PCI. Ensure IOMMU is enabled in BIOS/UEFI."
fi
echo

# 5. User Group Permissions
info "Checking user group membership..."
NEED_LOGOUT=0
for grp in libvirt kvm; do
    if getent group "$grp" >/dev/null 2>&1; then
        if ! id -nG "$CURRENT_USER" | grep -qw "$grp"; then
            info "Adding $CURRENT_USER to $grp group..."
            sudo usermod -aG "$grp" "$CURRENT_USER"
            NEED_LOGOUT=1
        else
            ok "$CURRENT_USER is already in $grp."
        fi
    fi
done

# 6. Configure Shared Memory (/dev/shm/looking-glass)
info "Configuring IVSHMEM shared memory device..."
TMPFILES_CONF="/etc/tmpfiles.d/10-looking-glass.conf"
sudo tee "$TMPFILES_CONF" >/dev/null <<EOF
f /dev/shm/looking-glass 0660 $CURRENT_USER $QEMU_GROUP -
EOF
sudo systemd-tmpfiles --create "$TMPFILES_CONF"
ok "Created $TMPFILES_CONF and initialized /dev/shm/looking-glass."

# Security Modules: SELinux vs AppArmor
if command -v getenforce >/dev/null 2>&1 && [ "$(getenforce)" != "Disabled" ]; then
    info "SELinux is active ($(getenforce)). Applying svirt_image_t label..."
    if command -v semanage >/dev/null 2>&1; then
        sudo semanage fcontext -a -t svirt_image_t '/dev/shm/looking-glass' 2>/dev/null || \
        sudo semanage fcontext -m -t svirt_image_t '/dev/shm/looking-glass'
        sudo restorecon -v /dev/shm/looking-glass
        ok "SELinux label applied."
    else
        warn "semanage command not found. Run 'sudo chcon -t svirt_image_t /dev/shm/looking-glass'."
    fi
elif [ -d /etc/apparmor.d ]; then
    info "Configuring AppArmor permissions..."
    APPARMOR_LOCAL="/etc/apparmor.d/local/abstractions/libvirt-qemu"
    if [ -f "$APPARMOR_LOCAL" ]; then
        if ! grep -q "/dev/shm/looking-glass" "$APPARMOR_LOCAL"; then
            echo "/dev/shm/looking-glass rw," | sudo tee -a "$APPARMOR_LOCAL" >/dev/null
            sudo systemctl restart apparmor 2>/dev/null || true
            ok "AppArmor rule updated."
        fi
    fi
fi

# 7. Generate Customized Libvirt Hook
info "Generating dynamic GPU switcher hook (/etc/libvirt/hooks/qemu)..."
sudo mkdir -p /etc/libvirt/hooks
HOOK_DEST="/etc/libvirt/hooks/qemu"

sudo tee "$HOOK_DEST" >/dev/null <<EOF
#!/usr/bin/env bash
# Automatically generated Libvirt QEMU hook for dGPU passthrough
guest="\$1"
action="\$2"
subaction="\$3"

# Adjust domain name if different from win11
[ "\$guest" = "win11" ] || exit 0

gpu="$DGPU_PCI"
audio="$AUDIO_PCI"

gpu_ids="$DGPU_IDS"
audio_ids="$AUDIO_IDS"

case "\$action/\$subaction" in
    prepare/begin)
        modprobe vfio-pci

        for dev in "\$gpu" "\$audio"; do
            [ -n "\$dev" ] && [ -e "/sys/bus/pci/devices/\$dev/driver/unbind" ] && \
                echo "\$dev" > "/sys/bus/pci/devices/\$dev/driver/unbind"
        done

        for ids in "\$gpu_ids" "\$audio_ids"; do
            [ -n "\$ids" ] && echo "\$ids" > /sys/bus/pci/drivers/vfio-pci/new_id 2>/dev/null
        done

        for dev in "\$gpu" "\$audio"; do
            [ -n "\$dev" ] && echo "\$dev" > /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null
        done
        ;;

    release/end)
        for dev in "\$gpu" "\$audio"; do
            [ -n "\$dev" ] && echo "\$dev" > /sys/bus/pci/drivers/vfio-pci/unbind 2>/dev/null
        done

        modprobe nvidia 2>/dev/null || modprobe amdgpu 2>/dev/null || true
        modprobe nvidia_drm 2>/dev/null || true
        modprobe snd_hda_intel 2>/dev/null || true

        [ -n "\$gpu" ] && [ -d /sys/bus/pci/drivers/nvidia ] && echo "\$gpu" > /sys/bus/pci/drivers/nvidia/bind 2>/dev/null
        [ -n "\$gpu" ] && [ -d /sys/bus/pci/drivers/amdgpu ] && echo "\$gpu" > /sys/bus/pci/drivers/amdgpu/bind 2>/dev/null
        [ -n "\$audio" ] && echo "\$audio" > /sys/bus/pci/drivers/snd_hda_intel/bind 2>/dev/null
        ;;
esac
EOF
sudo chmod +x "$HOOK_DEST"
ok "Hook installed to $HOOK_DEST."

# 8. Client Configuration & Desktop Launcher
info "Installing client configuration..."
mkdir -p "$HOME/.config/looking-glass" "$HOME/.local/bin" "$HOME/.local/share/applications"

cat > "$HOME/.looking-glass-client.ini" <<EOF
[input]
escapeKey=KEY_F12

[spice]
enable=yes
host=127.0.0.1
port=5900

[win]
autoResize=no
setGuestRes=no
allowResize=yes
keepAspect=yes
size=1920x1080

[egl]
doubleBuffer=yes
vsync=no
EOF
ok "Client configuration written to ~/.looking-glass-client.ini."

cat > "$HOME/.local/share/applications/looking-glass-client.desktop" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=Looking Glass Client
Comment=Looking Glass KVMFR client
Icon=looking-glass
Exec=$HOME/.local/bin/looking-glass-client
Terminal=false
Categories=System;
StartupWMClass=looking-glass-client
SingleMainWindow=true
EOF
update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true
ok "Desktop launcher installed."

echo
echo "=========================================================="
echo " Setup Completed Successfully!"
echo "=========================================================="
echo
echo "Summary of actions taken:"
echo "  1. Configured /dev/shm/looking-glass with ownership $CURRENT_USER:$QEMU_GROUP."
echo "  2. Configured security policies (SELinux / AppArmor)."
echo "  3. Installed dynamic GPU switcher hook for $DGPU_PCI ($DGPU_IDS)."
echo "  4. Set up client configuration in ~/.looking-glass-client.ini."
echo "  5. Registered desktop launcher in ~/.local/share/applications/."
echo
if [ "$NEED_LOGOUT" -eq 1 ]; then
    warn "You were added to the libvirt/kvm group. You MUST log out and log back in before launching VMs."
fi
echo
echo "Next Steps:"
echo "  A. Ensure your Windows 11 VM XML contains:"
echo "       - <shmem name='looking-glass'><model type='ivshmem-plain'/><size unit='M'>32</size></shmem>"
echo "       - <graphics type='spice' port='5900' autoport='no'><listen type='address' address='127.0.0.1'/></graphics>"
echo "       - <video><model type='none'/></video>"
echo "       - Hostdevs for $DGPU_PCI and $([ -n "$AUDIO_PCI" ] && echo "$AUDIO_PCI" || echo "Audio")."
echo "  B. In Windows 11 guest:"
echo "       - Install Looking Glass IDD Driver (looking-glass-idd-setup.exe)."
echo "       - Run laptop-setup/guest-windows/install-ivshmem.bat."
echo "       - Run laptop-setup/guest-windows/disable-sleep.bat."
echo "  C. Launch Looking Glass from terminal or application launcher: looking-glass-client."
echo
