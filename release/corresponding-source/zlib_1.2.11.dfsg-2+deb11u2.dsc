-----BEGIN PGP SIGNED MESSAGE-----
Hash: SHA512

Format: 3.0 (quilt)
Source: zlib
Binary: zlib1g, zlib1g-dev, zlib1g-udeb, lib64z1, lib64z1-dev, lib32z1, lib32z1-dev, libn32z1, libn32z1-dev
Architecture: any
Version: 1:1.2.11.dfsg-2+deb11u2
Maintainer: Mark Brown <broonie@debian.org>
Homepage: http://zlib.net/
Standards-Version: 3.9.8
Build-Depends: debhelper (>= 8.1.3~), gcc-multilib [amd64 i386 kfreebsd-amd64 mips mipsel powerpc ppc64 s390 sparc s390x mipsn32 mipsn32el mipsr6 mipsr6el mipsn32r6 mipsn32r6el mips64 mips64el mips64r6 mips64r6el x32] <!nobiarch>, dpkg-dev (>= 1.16.1)
Package-List:
 lib32z1 deb libs optional arch=amd64,ppc64,kfreebsd-amd64,s390x profile=!nobiarch
 lib32z1-dev deb libdevel optional arch=amd64,ppc64,kfreebsd-amd64,s390x profile=!nobiarch
 lib64z1 deb libs optional arch=sparc,s390,i386,powerpc,mips,mipsel,mipsn32,mipsn32el,mipsr6,mipsr6el,mipsn32r6,mipsn32r6el,x32 profile=!nobiarch
 lib64z1-dev deb libdevel optional arch=sparc,s390,i386,powerpc,mips,mipsel,mipsn32,mipsn32el,mipsr6,mipsr6el,mipsn32r6,mipsn32r6el,x32 profile=!nobiarch
 libn32z1 deb libs optional arch=mips,mipsel profile=!nobiarch
 libn32z1-dev deb libdevel optional arch=mips,mipsel profile=!nobiarch
 zlib1g deb libs required arch=any
 zlib1g-dev deb libdevel optional arch=any
 zlib1g-udeb udeb debian-installer optional arch=any
Checksums-Sha1:
 1b7f6963ccfb7262a6c9d88894d3a30ff2bf2e23 370248 zlib_1.2.11.dfsg.orig.tar.gz
 4dfc28544156eb98473cb4830859cd48e20e4a5b 23984 zlib_1.2.11.dfsg-2+deb11u2.debian.tar.xz
Checksums-Sha256:
 80c481411a4fe8463aeb8270149a0e80bb9eaf7da44132b6e16f2b5af01bc899 370248 zlib_1.2.11.dfsg.orig.tar.gz
 c19794df214f0c2571b19f7dea853c066410232abe9f0ddad77231fabccde0da 23984 zlib_1.2.11.dfsg-2+deb11u2.debian.tar.xz
Files:
 2950b229ed4a5e556ad6581580e4ab2c 370248 zlib_1.2.11.dfsg.orig.tar.gz
 8f9d254c887cd8490d022895001c030f 23984 zlib_1.2.11.dfsg-2+deb11u2.debian.tar.xz

-----BEGIN PGP SIGNATURE-----

iQKmBAEBCgCQFiEERkRAmAjBceBVMd3uBUy48xNDz0QFAmMFJFpfFIAAAAAALgAo
aXNzdWVyLWZwckBub3RhdGlvbnMub3BlbnBncC5maWZ0aGhvcnNlbWFuLm5ldDQ2
NDQ0MDk4MDhDMTcxRTA1NTMxRERFRTA1NENCOEYzMTM0M0NGNDQSHGNhcm5pbEBk
ZWJpYW4ub3JnAAoJEAVMuPMTQ89EXAUP/0G1h94kKX6Fjc7oUY0zHr8qTZN/EJxY
e/OQg5LHpLefhn8N7C7NBk65eruBgdr9C6dnA+VVyOXQ3lmGss1G/E/NuNQ3aooD
93zQdF17+tmSA1P6U5sCmDJ29cCau7uYxM7I+u5aCriZjoxKnsZ+sgNvBiXT6jZ8
ilUdyBSh+MzO2TBgYZKzmYM8OgyoJGq+nHY+I5ARukLFYNIh1bACbOWJGi5eUCec
uP2KmI01ZLp7J9chtReYZa5kbpPvqrGGJX/Re/2SktprUtr2G5ZIWu38kZQ+bi8Q
q5beOtFFbUO/KoDEtqlicm+f9gNuvtObyyrm4u/K4EqEC8SzPIucL7DoS9NFQe8S
v2XNomLXWV67oYPYel1o7NUyTSNEmlo1zYRgnrEjfyAFzHUgSNcQ4dq9yvVMbm/G
nYwCZv/Udy9pTtHhmbCVJLcjic4aTYcMdPqbKxKN8HQtStCzGT/4ve9O/bvF3Z4p
bMStLQffFPkkqXPO42sG9TV/5jn3LbXrnkn8gpRo0JkDrRHPeWlQUHB+M9AMu9of
Njbba8hm9J0VtG1gR+yipNog3rFVtUDstpVzHz97yO1xE+gsiwJIH0YKL8oH5XZx
Dq2pTH+BhTTByMHkDhym2NNAigRZ4Md10uzlI2/ZZzGDqpvKC7ZHByz9fX/pqnLW
rLJHRnVt2Nbo
=mxMe
-----END PGP SIGNATURE-----
