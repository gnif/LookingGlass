# Looking Glass

An extremely low latency KVMFR (KVM FrameRelay) implementation for guests with
VGA PCI Passthrough.

* Project Website: https://looking-glass.io
* Documentation: https://looking-glass.io/docs

## Documentation

❕❕❕ **IMPORTANT** ❕❕❕

This project contains submodules that must be checked out if building from the
git repository! If you are not a developer and just want to compile Looking
Glass, please download the source archive from the website instead:

https://looking-glass.io/downloads

Source code for the documentation can be found in the `/doc` directory.

You may view this locally as HTML by running `make html` with `python3-sphinx`
and `python3-sphinx-rtd-theme` installed.

## Development

```shell
podman build --target app --build-arg DISTRO=debian --build-arg DISTRO_VERSION=13 .
podman build --target app --build-arg DISTRO=ubuntu --build-arg DISTRO_VERSION=25.10 .
```

### Debug

For debugging, you need to change `--target` to `builder` & run your built image:

```shell
IMAGE_ID=$(podman build --target builder --build-arg DISTRO=debian --build-arg DISTRO_VERSION=13 . | awk 'END{print}')
podman run --rm -it -v .:/srv/app -w /srv/app ${IMAGE_ID} bash
```
