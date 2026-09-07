FROM debian:bookworm-slim@sha256:88200866dfff7ea7f5cbcb6ec7c8a701889efe6fe859fe64d6990e4b07ea4171

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      binutils=2.40-2 \
      build-essential=12.9 \
      ca-certificates=20250419~deb12u1 \
      cmake=3.25.1-1 \
      file=1:5.44-3 \
      ninja-build=1.11.1-2~deb12u1 \
      pkg-config=1.8.1-1 \
      python3=3.11.2-1+b1 \
 && rm -rf /var/lib/apt/lists/*

ENV LC_ALL=C.UTF-8
