FROM debian:bookworm-slim AS src
LABEL Description="Tilemaker" Version="1.4.0"

ARG BUILD_DEBUG=

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential \
    liblua5.1-0-dev \
    libsqlite3-dev \
    libshp-dev \
    libboost-program-options-dev \
    libboost-filesystem-dev \
    libboost-system-dev \
    luarocks \
    rapidjson-dev \
    cmake && \
    rm -rf /var/lib/apt/lists/* && \
    luarocks install luaflock

WORKDIR /usr/src/app

COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY src ./src
COPY include ./include
COPY server ./server

RUN mkdir build && \
    cd build && \
    if [ -z "$BUILD_DEBUG" ]; then \
        cmake -DCMAKE_BUILD_TYPE=Release -DBoost_USE_DEBUG_RUNTIME=OFF ..; \
    else \
        cmake -DCMAKE_BUILD_TYPE=Debug ..; \
    fi; \
    cmake --build . --parallel $(nproc) && \
    if [ -z "$BUILD_DEBUG" ]; then \
        strip tilemaker && \
        strip tilemaker-server; \
    fi

ENV PATH="/usr/src/app/build:$PATH"

FROM debian:bookworm-slim

ARG BUILD_DEBUG=

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    liblua5.1-0 \
    shapelib \
    libsqlite3-0 \
    lua-sql-sqlite3 \
    libboost-filesystem1.74.0 \
    libboost-program-options1.74.0 && \
    if [ -n "$BUILD_DEBUG" ]; then \
        DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends gdb; \
    fi; \
    rm -rf /var/lib/apt/lists/*

WORKDIR /usr/src/app
COPY --from=src /usr/src/app/build/tilemaker .
COPY --from=src /usr/src/app/build/tilemaker-server .
COPY --from=src /usr/local/lib/lua/5.1/flock.so /usr/local/lib/lua/5.1/flock.so
COPY resources ./resources
COPY process.lua ./
COPY config.json ./

ENV PATH="/usr/src/app/build:$PATH"

# Entrypoint for docker, wrapped with /bin/sh to remove requirement for executable permissions on script
ENTRYPOINT ["/bin/sh", "/usr/src/app/resources/docker-entrypoint.sh"]
CMD ["--help"]
