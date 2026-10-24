ARG BASE_IMAGE
FROM ${BASE_IMAGE}

ARG DEBIAN_FRONTEND=noninteractive
ARG GCC15_PACKAGE=15.3.0-4
ARG CLANG_PACKAGE=1:22.1.8-3
ARG HEAPTRACK_PACKAGE=1.5.0+dfsg1+git20260219.f16e8d3-2+b2
ARG MINGW_PACKAGE=16.2.0-1+29
ARG MINGW_HEADERS_PACKAGE=14.0.0-1

RUN apt-get update \
	&& apt-get install -y --no-install-recommends \
		bash ca-certificates curl git jq make python3 \
		gcc-15=${GCC15_PACKAGE} g++-15=${GCC15_PACKAGE} \
		clang-22=${CLANG_PACKAGE} clang-tools-22=${CLANG_PACKAGE} clang-tidy-22=${CLANG_PACKAGE} clang-format-22=${CLANG_PACKAGE} \
		heaptrack=${HEAPTRACK_PACKAGE} \
		gcc-mingw-w64-x86-64-win32=${MINGW_PACKAGE} mingw-w64-x86-64-dev=${MINGW_HEADERS_PACKAGE} \
	&& rm -rf /var/lib/apt/lists/*

ENV CC=gcc-15 CXX=g++-15 CC15=gcc-15 CLANG=clang-22 CLANG_TIDY=clang-tidy-22 MINGW_CC=x86_64-w64-mingw32-gcc-win32
WORKDIR /src
