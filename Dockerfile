FROM debian:bullseye-slim AS src
LABEL Description="Tilemaker" Version="1.4.0"

ARG DEBIAN_FRONTEND=noninteractive

# install dependencies
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      build-essential \
      liblua5.1-0 \
      liblua5.1-0-dev \
      libprotobuf-dev \
      libsqlite3-dev \
      protobuf-compiler \
      shapelib \
      libshp-dev \
      libboost-program-options-dev \
      libboost-filesystem-dev \
      libboost-system-dev \
      libboost-iostreams-dev \
      rapidjson-dev \
      cmake

COPY CMakeLists.txt /
COPY cmake /cmake
COPY src /src
COPY include /include

WORKDIR /build

RUN cmake -DTILEMAKER_BUILD_STATIC=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ ..
RUN cmake --build .
RUN strip tilemaker

FROM debian:bullseye-slim
WORKDIR /
COPY --from=src /build/tilemaker .
COPY resources /resources
COPY process.lua .
COPY config.json .
RUN ln -s /tilemaker /usr/bin/tilemaker

ENTRYPOINT ["/tilemaker"]
