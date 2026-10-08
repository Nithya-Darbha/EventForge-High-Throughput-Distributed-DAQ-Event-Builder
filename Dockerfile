# Linux build env for macOS / Windows. Everything (builder, frontends, tests, experiments) runs inside.
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        g++ cmake make git ca-certificates python3 python3-yaml python3-matplotlib python3-numpy \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j
CMD ["bash"]
