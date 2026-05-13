#pragma once
// ============================================================
//  version.h — Quanta OS version string (single source of truth)
//
//  The Makefile can override with -DQUANTA_VERSION=\"x.y.z\"
//  All source files should #include this instead of hard-coding.
// ============================================================

#ifndef QUANTA_VERSION
#define QUANTA_VERSION "2.5.0"
#endif

#ifndef QUANTA_ARCH
#define QUANTA_ARCH "x86_64"
#endif

#ifndef QUANTA_CODENAME
#define QUANTA_CODENAME "Foundation"
#endif
