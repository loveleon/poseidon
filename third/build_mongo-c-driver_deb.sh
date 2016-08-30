#!/bin/bash

set -e

pkgname="libmongoc"
pkgversion="1.4.0"
pkglicense="Apache"
pkggroup="http://mongoc.org/"
pkgsource="https://github.com/mongodb/mongo-c-driver/releases/download/${pkgversion}/mongo-c-driver-${pkgversion}.tar.gz"
maintainer="lh_mouse"
provides="libmongoc-1.0,libbson-1.0"

dstdir="$(pwd)"
tempdir="$(mktemp -d)"

if [[ $EUID -ne 0 ]]; then
	echo "You must run this script as root."
	exit 1
fi

[[ -z "${tempdir}" ]] || rm -rf "${tempdir}"
mkdir -p "${tempdir}"
trap "rm -rf \"${tempdir}\"" EXIT
cd "${tempdir}"

_archive="$(basename -- ${pkgsource})"
wget -O "${_archive}" "${pkgsource}"
tar -xzvf "${_archive}"

cd "$(basename "${_archive}" ".tar.gz")"
CFLAGS='-O3' ./configure --disable-automatic-init-and-cleanup --prefix=/usr/local
make -j4
checkinstall -y --pkgname="${pkgname}" --pkgversion="${pkgversion}" --pkglicense="${pkglicense}" --pkggroup="${pkggroup}"	\
	--pkgsource="${pkgsource}" --maintainer="${maintainer}" --provides="${provides}"
mv *.deb "${dstdir}"/
