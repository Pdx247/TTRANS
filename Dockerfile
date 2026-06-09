FROM gcc:13-bookworm AS build

WORKDIR /src
COPY include ./include
COPY src ./src

RUN g++ -std=c++14 -O2 -Wall -Wextra -pthread -I include \
    src/main.cpp src/protocol.cpp src/socket.cpp src/transfer.cpp src/web_gui.cpp \
    -o /out/ttrans

FROM debian:bookworm-slim

ENV TTRANS_HTTP_BIND=0.0.0.0
WORKDIR /data

COPY --from=build /out/ttrans /usr/local/bin/ttrans

EXPOSE 47880/tcp
EXPOSE 44777/udp

ENTRYPOINT ["ttrans"]
CMD ["gui", "--http-port", "47880"]
