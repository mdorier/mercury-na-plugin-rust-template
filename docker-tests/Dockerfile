# Multi-stage Dockerfile for Mercury NAT integration tests.
# Build context: /home/ubuntu/mercury-custom-na/ (parent of mercury-na-plugin-libp2p/)

# ============================================================
# Stage: base — minimal runtime with networking tools
# ============================================================
FROM ubuntu:24.04 AS base
RUN apt-get update && apt-get install -y \
    iptables iproute2 ca-certificates && \
    rm -rf /var/lib/apt/lists/*

# ============================================================
# Stage: builder — compile Mercury, plugin, relay, and tests
# ============================================================
FROM base AS builder
RUN apt-get update && apt-get install -y \
    build-essential cmake pkg-config libclang-dev curl git && \
    rm -rf /var/lib/apt/lists/*
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | \
    sh -s -- -y --default-toolchain stable
ENV PATH="/root/.cargo/bin:${PATH}"

# Build Mercury from source
COPY mercury/ /src/mercury/
RUN cmake -S /src/mercury -B /build/mercury \
    -DCMAKE_INSTALL_PREFIX=/opt/mercury \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF \
    -DMERCURY_USE_BOOST_PP=ON -DMERCURY_USE_CHECKSUMS=OFF \
    -DNA_USE_DYNAMIC_PLUGINS=ON -DNA_USE_SM=OFF \
    -DNA_USE_OFI=OFF -DNA_USE_MPI=OFF && \
    cmake --build /build/mercury -j$(nproc) && \
    cmake --install /build/mercury

# Build plugin + relay + test programs
COPY rust-libp2p/ /src/rust-libp2p/
COPY mercury-na-plugin-libp2p/ /src/mercury-na-plugin-libp2p/
RUN cmake -S /src/mercury-na-plugin-libp2p -B /build/plugin \
    -Dmercury_DIR=/opt/mercury/share/cmake/mercury && \
    cmake --build /build/plugin -j$(nproc)

# ============================================================
# Stage: runtime — lean image with only binaries and libraries
# ============================================================
FROM base AS runtime
COPY --from=builder /opt/mercury/lib/ /opt/mercury/lib/
COPY --from=builder /build/plugin/cargo-build/release/libna_plugin_libp2p.so /opt/plugin/
COPY --from=builder /build/plugin/cargo-build/release/mercury-na-relay-server /opt/bin/
COPY --from=builder /build/plugin/test/test_hg_server /opt/test/
COPY --from=builder /build/plugin/test/test_rpc /opt/test/
COPY --from=builder /build/plugin/test/test_bulk /opt/test/
COPY --from=builder /build/plugin/test/test_nat_hg_server /opt/test/
COPY --from=builder /build/plugin/test/test_nat_hg_rpc_client /opt/test/
COPY --from=builder /build/plugin/test/test_nat_hg_dcutr_client /opt/test/
ENV LD_LIBRARY_PATH=/opt/mercury/lib
ENV NA_PLUGIN_PATH=/opt/plugin

# ============================================================
# Stage: router — NAT gateway container
# ============================================================
FROM base AS router
COPY mercury-na-plugin-libp2p/docker-tests/router/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh
ENTRYPOINT ["/entrypoint.sh"]
