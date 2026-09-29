FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ninja-build ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake --preset release && cmake --build --preset release -j2

FROM ubuntu:24.04
RUN useradd --create-home --uid 10001 keyforge \
    && mkdir -p /var/lib/keyforge \
    && chown keyforge:keyforge /var/lib/keyforge
COPY --from=build /src/build/release/keyforge-server /usr/local/bin/keyforge-server
COPY --from=build /src/build/release/keyforge-cli /usr/local/bin/keyforge-cli
USER keyforge
WORKDIR /var/lib/keyforge
EXPOSE 6380
ENTRYPOINT ["keyforge-server"]
CMD ["--bind", "0.0.0.0", "--port", "6380", "--data-dir", "/var/lib/keyforge", "--appendonly", "yes", "--appendfsync", "everysec"]

