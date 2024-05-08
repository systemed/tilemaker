FROM debian:bookworm-slim AS src
LABEL Description="Tilemaker" Version="1.4.0"

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential \
    liblua5.1-0-dev \
    libsqlite3-dev \
    libshp-dev \
    libboost-program-options-dev \
    libboost-filesystem-dev \
    libboost-system-dev \
    libboost-iostreams-dev \
    rapidjson-dev \
    cmake \
    zlib1g-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /usr/src/app

COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY src ./src
COPY include ./include
COPY server ./server

RUN mkdir build && \
    cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    cmake --build . --parallel $(nproc) && \
    strip tilemaker && \
    strip tilemaker-server

ENV PATH="/usr/src/app/build:$PATH"

FROM debian:bookworm-slim
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    liblua5.1-0 \
    shapelib \
    libsqlite3-0 \
    libboost-filesystem1.74.0 \
    libboost-program-options1.74.0 \
    libboost-iostreams1.74.0 && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /usr/src/app
COPY --from=src /usr/src/app/build/tilemaker .
COPY --from=src /usr/src/app/build/tilemaker-server .
COPY resources ./resources
COPY process.lua ./
COPY config.json ./

ENV PATH="/usr/src/app/build:$PATH"

# Entrypoint for docker, wrapped with /bin/sh to remove requirement for executable permissions on script
ENTRYPOINT ["/bin/sh", "/usr/src/app/resources/docker-entrypoint.sh"]
CMD ["--help"]
