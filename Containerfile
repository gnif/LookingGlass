ARG DISTRO=debian
ARG DISTRO_VERSION=13

FROM docker.io/debian:${DISTRO_VERSION} AS builder_debian

ENV PATH="${PATH}:/srv/app/bin"

RUN apt-get update && apt-get upgrade -y; \
    DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC apt-get install -y \
        binutils-dev cmake fonts-dejavu-core libfontconfig-dev \
        gcc g++ pkg-config libegl-dev libgl-dev libgles-dev libspice-protocol-dev \
        nettle-dev libx11-dev libxcursor-dev libxi-dev libxinerama-dev \
        libxpresent-dev libxss-dev libxkbcommon-dev libwayland-dev wayland-protocols \
        libpipewire-0.3-dev libpulse-dev libsamplerate0-dev \
    ;


FROM docker.io/ubuntu:${DISTRO_VERSION} AS builder_ubuntu

ENV PATH="${PATH}:/srv/app/bin"

RUN apt-get update && apt-get upgrade -y; \
    DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC apt-get install -y \
        binutils-dev cmake fonts-dejavu-core libfontconfig-dev \
        gcc g++ pkg-config libegl-dev libgl-dev libgles-dev libspice-protocol-dev \
        nettle-dev libx11-dev libxcursor-dev libxi-dev libxinerama-dev \
        libxpresent-dev libxss-dev libxkbcommon-dev libwayland-dev wayland-protocols \
        libpipewire-0.3-dev libpulse-dev libsamplerate0-dev \
    ;


FROM builder_${DISTRO} AS builder

WORKDIR /srv/app
VOLUME /srv/app/client/build

COPY . /srv/app

RUN cd client/build; \
    cmake ../; \
    make

VOLUME /srv/build

# TODO: Support `INSTALL_ROOT` as destination prefix:
# `make install INSTALL_ROOT=/srv/build`
# After that change while-loop to following:
# `(ldd /srv/build/usr/local/bin/looking-glass-client | awk 'NR>1 {print $3}') | while read -r FILE; do`
RUN cd client/build; \
    make install; \
    TARGET=/srv/build; \
    (echo /usr/local/bin/looking-glass-client && ldd /usr/local/bin/looking-glass-client | awk 'NR>1 {print $3}') | while read -r FILE; do \
        [ -z "${FILE}" ] && continue; \
        DIR=$(dirname "${FILE}"); \
        mkdir -p "${TARGET}${DIR}"; \
        cp -a "${FILE}" "${TARGET}${FILE}"; \
        LINK=$(readlink -f "${FILE}"); \
        [ "${LINK}" != "${FILE}" ] && cp -a "${LINK}" "${TARGET}${LINK}"; \
    done


FROM scratch AS app

COPY --from=builder /srv/build /
