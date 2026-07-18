# Rockbox ARM Linux (sonynwz) toolchain + build environment
# Builds the arm-rockbox-linux-gnueabi gcc 9.5.0 toolchain via rockboxdev.sh
FROM debian:12

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    perl \
    curl \
    wget \
    xz-utils \
    bzip2 \
    gzip \
    zip \
    patch \
    texinfo \
    flex \
    bison \
    automake \
    libtool \
    libtool-bin \
    autoconf \
    libmpc-dev \
    libncurses5-dev \
    gawk \
    python3 \
    rsync \
    ccache \
    && rm -rf /var/lib/apt/lists/*

# build the toolchain (this is the long step, cached as its own layer)
COPY tools/rockboxdev.sh /tmp/rockboxdev.sh
COPY tools/toolchain-patches /tmp/toolchain-patches
# keep rockboxdev.sh and the patches around, the hard-float toolchain below
# needs them too
RUN chmod +x /tmp/rockboxdev.sh && \
    /tmp/rockboxdev.sh --target=x --prefix=/opt/rbtoolchain --makeflags=-j$(nproc) && \
    rm -rf /tmp/rb /tmp/build* 2>/dev/null || true

ENV PATH=/opt/rbtoolchain/bin:/opt/rbtoolchain-hf/bin:$PATH

# alsa-lib for the hosted sonynwz targets: must live in the toolchain sysroot.
# rockboxdev.sh is supposed to build it as part of --target=x but that step
# failed silently, so do it explicitly here.
RUN cd /tmp && \
    wget -q https://www.alsa-project.org/files/pub/lib/alsa-lib-1.0.19.tar.bz2 && \
    tar xf alsa-lib-1.0.19.tar.bz2 && \
    cd alsa-lib-1.0.19 && \
    # the bundled config.guess/config.sub are too old to know about aarch64 \
    cp /usr/share/automake-1.16/config.guess /usr/share/automake-1.16/config.sub . && \
    ./configure --host=arm-rockbox-linux-gnueabi --prefix=/usr --disable-python && \
    make -j$(nproc) && \
    make install DESTDIR=/opt/rbtoolchain/arm-rockbox-linux-gnueabi/sysroot && \
    cd / && rm -rf /tmp/alsa-lib*

# Hard-float ARMv7 toolchain for the Sony NW-A30 (Hagoromo). Mandatory: the
# device ships /lib/ld-linux-armhf.so.3, so a soft-float binary from the
# toolchain above names an interpreter that does not exist there and cannot be
# executed at all (the kernel fails the exec, so it looks like a boot loop).
RUN /tmp/rockboxdev.sh --target=h --prefix=/opt/rbtoolchain-hf --makeflags=-j$(nproc) && \
    rm -rf /tmp/rb /tmp/build* 2>/dev/null || true

# alsa-lib for the hard-float toolchain (same config.guess workaround)
RUN cd /tmp && \
    wget -q https://www.alsa-project.org/files/pub/lib/alsa-lib-1.0.19.tar.bz2 && \
    tar xf alsa-lib-1.0.19.tar.bz2 && \
    cd alsa-lib-1.0.19 && \
    cp /usr/share/automake-1.16/config.guess /usr/share/automake-1.16/config.sub . && \
    ./configure --host=arm-rockbox-linux-gnueabihf --prefix=/usr --disable-python && \
    make -j$(nproc) && \
    make install DESTDIR=/opt/rbtoolchain-hf/arm-rockbox-linux-gnueabihf/sysroot && \
    cd / && rm -rf /tmp/alsa-lib*

WORKDIR /rockbox
CMD ["/bin/bash"]
