FROM ubuntu:16.04
LABEL Description="Tilemaker" Version="1.4.0"

# INSTALL DEPENDENCIES
RUN apt-get update
RUN apt-get install -y build-essential liblua5.1-0 liblua5.1-0-dev libprotobuf-dev libsqlite3-dev protobuf-compiler shapelib libshp-dev libboost-all-dev

# BUILD
COPY . /
WORKDIR /

RUN make
RUN make install

ENTRYPOINT ["tilemaker"]
