-----BEGIN PGP SIGNED MESSAGE-----
Hash: SHA256

Format: 3.0 (quilt)
Source: bzip2
Binary: libbz2-1.0, libbz2-dev, bzip2, bzip2-doc
Architecture: any all
Version: 1.0.8-4
Maintainer: Anibal Monsalve Salazar <anibal@debian.org>
Uploaders: Santiago Ruano Rincón <santiago@debian.org>, Anthony Fok <foka@debian.org>
Homepage: https://sourceware.org/bzip2/
Standards-Version: 4.2.0
Vcs-Browser: https://salsa.debian.org/debian/bzip2
Vcs-Git: https://salsa.debian.org/debian/bzip2.git
Testsuite: autopkgtest
Testsuite-Triggers: @builddeps@
Build-Depends: dpkg-dev (>= 1.16.0), debhelper (>= 11)
Build-Depends-Indep: texinfo, docbook-xml, docbook2x, xsltproc
Package-List:
 bzip2 deb utils standard arch=any
 bzip2-doc deb doc optional arch=all
 libbz2-1.0 deb libs important arch=any
 libbz2-dev deb libdevel optional arch=any
Checksums-Sha1:
 bf7badf7e248e0ecf465d33c2f5aeec774209227 810029 bzip2_1.0.8.orig.tar.gz
 6b6a4519862bd4f9ee6cd292e8587f4e2ceba26a 26515 bzip2_1.0.8-4.debian.tar.bz2
Checksums-Sha256:
 ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269 810029 bzip2_1.0.8.orig.tar.gz
 3f3b26d83120260c7b2e69a5c89649bb818a79955b960fb34a5fae106f008a5d 26515 bzip2_1.0.8-4.debian.tar.bz2
Files:
 67e051268d0c475ea773822f7500d0e5 810029 bzip2_1.0.8.orig.tar.gz
 b0ae1a9955c92402562d5d06e2885be2 26515 bzip2_1.0.8-4.debian.tar.bz2

-----BEGIN PGP SIGNATURE-----

iIoEARYIADIWIQRZVjztY8b+Ty43oH1itBCJKh26HQUCXxWmFBQcc2FudGlhZ29A
ZGViaWFuLm9yZwAKCRBitBCJKh26Hcv1AP91j//UFglcv9+s2dJqYXMjAKPi35O/
JBfB9ocE127TtAEArJk1jIOd4033up6Tpif/2dDvgm6PJzotLosaKDABtQ8=
=ezi5
-----END PGP SIGNATURE-----
