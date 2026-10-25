ARG BASE_IMAGE
FROM ${BASE_IMAGE}

ARG DEBIAN_FRONTEND=noninteractive
ARG PYTHON_PACKAGE
ARG PYTHON_INTERP_PACKAGE
ARG GO_VERSION
ARG GO_SHA256
ARG RUSTUP_VERSION
ARG RUSTUP_SHA256
ARG RUST_TOOLCHAIN
ARG RUFF_VERSION
ARG RUFF_SHA256
ARG STATICCHECK_VERSION

## gcc is only cargo's linker and cgo's compiler here; C's pinned compilers are in C.Dockerfile.
RUN apt-get update \
	&& apt-get install -y --no-install-recommends \
		bash ca-certificates curl git gcc libc6-dev python3=${PYTHON_PACKAGE} python3.14=${PYTHON_INTERP_PACKAGE} \
	&& rm -rf /var/lib/apt/lists/*

ENV RUSTUP_HOME=/usr/local/rustup PATH=/usr/local/go/bin:/usr/local/cargo/bin:/opt/tools/bin:${PATH}

RUN curl -fsSL -o /tmp/go.tgz "https://go.dev/dl/${GO_VERSION}.linux-amd64.tar.gz" \
	&& echo "${GO_SHA256}  /tmp/go.tgz" | sha256sum -c - \
	&& tar -C /usr/local -xzf /tmp/go.tgz \
	&& rm /tmp/go.tgz \
	&& mkdir -p /opt/tools/bin \
	&& GOBIN=/opt/tools/bin GOPATH=/tmp/gopath GOCACHE=/tmp/gocache GOFLAGS=-modcacherw \
		go install "honnef.co/go/tools/cmd/staticcheck@${STATICCHECK_VERSION}" \
	&& rm -rf /tmp/gopath /tmp/gocache

RUN curl -fsSL -o /tmp/rustup-init "https://static.rust-lang.org/rustup/archive/${RUSTUP_VERSION}/x86_64-unknown-linux-gnu/rustup-init" \
	&& echo "${RUSTUP_SHA256}  /tmp/rustup-init" | sha256sum -c - \
	&& chmod +x /tmp/rustup-init \
	&& CARGO_HOME=/usr/local/cargo /tmp/rustup-init -y --no-modify-path --profile minimal \
		--default-toolchain "${RUST_TOOLCHAIN}" --component rustfmt,clippy \
	&& rm /tmp/rustup-init \
	&& chmod -R a+rX /usr/local/rustup /usr/local/cargo

RUN whl="ruff-${RUFF_VERSION}-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl" \
	&& curl -fsSL -o "/tmp/${whl}" "https://files.pythonhosted.org/packages/py3/r/ruff/${whl}" \
	&& echo "${RUFF_SHA256}  /tmp/${whl}" | sha256sum -c - \
	&& python3 -m zipfile -e "/tmp/${whl}" /tmp/ruff \
	&& install -m 755 "/tmp/ruff/ruff-${RUFF_VERSION}.data/scripts/ruff" /opt/tools/bin/ruff \
	&& rm -rf /tmp/ruff "/tmp/${whl}"

WORKDIR /src
