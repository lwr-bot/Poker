FROM ubuntu:24.04 AS muduo-builder

ARG DEBIAN_FRONTEND=noninteractive
ARG MUDUO_VERSION=v2.0.2

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git libboost-all-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch "${MUDUO_VERSION}" https://github.com/chenshuo/muduo.git /tmp/muduo \
    && cmake -S /tmp/muduo -B /tmp/muduo/build -DCMAKE_BUILD_TYPE=Release -DMUDUO_BUILD_EXAMPLES=OFF \
    && cmake --build /tmp/muduo/build --parallel \
    && cmake --install /tmp/muduo/build

FROM ubuntu:24.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build libboost-all-dev \
    libprotobuf-dev protobuf-compiler default-libmysqlclient-dev \
    libhiredis-dev libsodium-dev \
    && rm -rf /var/lib/apt/lists/*

COPY --from=muduo-builder /usr/local/ /usr/local/
WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DPOKER_BUILD_TESTS=ON \
      -DPOKER_BUILD_SERVER=ON \
    && cmake --build build --parallel \
    && ctest --test-dir build --output-on-failure

FROM ubuntu:24.04 AS runtime

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    libboost-all-dev libprotobuf-dev default-libmysqlclient-dev \
    libhiredis-dev libsodium-dev ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/lib/libmuduo_* /usr/local/lib/
COPY --from=builder /src/build/apps/server/poker_server /usr/local/bin/poker_server
RUN ldconfig && useradd --system --uid 10001 --no-create-home poker
USER poker
EXPOSE 7000 9100
ENTRYPOINT ["/usr/local/bin/poker_server"]
