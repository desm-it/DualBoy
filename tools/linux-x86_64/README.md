# Linux x86-64 toolchain

The Dockerfile pins the Debian image digest and every directly requested package
version. `make linux-x86_64` also pins the requested platform to `linux/amd64`.
If Debian retires a security-package version, update the image digest and package
manifest together after a complete rebuild and record the change in
`docs/status.md`.
