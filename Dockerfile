FROM debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential cmake gdb binutils procps iproute2 tcpdump ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
ENV LD_LIBRARY_PATH=/workspace/util:/workspace/build/lib:/workspace/lib
CMD ["bash"]
