# The signalling server (netplay_server, host/src/netplay/net/signal_server.cpp), built from the netplay
# transport's folder. The context is the repository; Dockerfile.dockerignore leaves out all but that folder.
#
#   docker build -f tools/netplay/server/Dockerfile -t conker-netplay-server .

FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake ninja-build g++ git ca-certificates libprotobuf-dev protobuf-compiler libssl-dev \
    && rm -rf /var/lib/apt/lists/*
COPY host/src/netplay/net /src
RUN cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build --target netplay_server signal_packets_test turn_credentials_test server_status_test \
    && /build/signal_packets_test && /build/turn_credentials_test && /build/server_status_test

FROM debian:bookworm-slim
# nftables and iproute2 for compose.yaml's net service (as root), which uses this image too.
RUN apt-get update && apt-get install -y --no-install-recommends libprotobuf32 libssl3 nftables iproute2 \
    && rm -rf /var/lib/apt/lists/*
# Its kept numbers (--state, compose.yaml's data volume), which a new volume takes its owner from.
RUN mkdir -p /var/lib/conker-netplay && chown nobody:nogroup /var/lib/conker-netplay
COPY --from=build /build/netplay_server /usr/local/bin/netplay_server
USER nobody
EXPOSE 27100/udp
ENTRYPOINT ["netplay_server"]
CMD ["--port", "27100"]
