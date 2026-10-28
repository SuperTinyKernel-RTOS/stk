/*
 * SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
 *
 * Source: https://github.com/SuperTinyKernel-RTOS
 *
 * Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
 * License: MIT License, see LICENSE for a full text.
 */

#ifndef STK_VERSION_H_
#define STK_VERSION_H_

/*! \file  stk_version.h
    \brief Version definitions of STK (Semantic Versioning 2.0.0).
    \note  The STK_VERSION_* macros are usable from both C and C++ sources, the stk::VersionInfo
           descriptor and stk::GetVersion() are C++ only (see stk_get_version() in stk_c.h for C).
    \note  Requires \c uint8_t / \c uint32_t to be declared before this header is included.
*/

// Version numbers are not overridable: a stray -D or an earlier #define must not change the version.
// Checked with defined() rather than by value, so a 0 (e.g. patch) is caught as well.
#if defined(STK_VERSION_MAJOR) || defined(STK_VERSION_MINOR) || defined(STK_VERSION_PATCH)
    #error "STK_VERSION_MAJOR, STK_VERSION_MINOR and STK_VERSION_PATCH must not be defined outside of stk_version.h."
#endif

/*! \def   STK_VERSION_MAJOR
    \brief Major version number.
    \note  Single source of truth, edit only here. Plain literal (no 'U' suffix) so it can be stringified.
    \note  Not overridable: a stray -D must not change the version.
    \note  Valid range: 0..255.
*/
#define STK_VERSION_MAJOR   1

/*! \def   STK_VERSION_MINOR
    \brief Minor version number.
    \note  Single source of truth, edit only here. Plain literal (no 'U' suffix) so it can be stringified.
    \note  Not overridable: a stray -D must not change the version.
    \note  Valid range: 0..255.
*/
#define STK_VERSION_MINOR   8

/*! \def   STK_VERSION_PATCH
    \brief Patch version number.
    \note  Single source of truth, edit only here. Plain literal (no 'U' suffix) so it can be stringified.
    \note  Not overridable: a stray -D must not change the version.
    \note  Valid range: 0..255.
*/
#define STK_VERSION_PATCH   0

/*! \def   STK_VERSION_PRERELEASE
    \brief Pre-release tag, e.g. "-rc1"; empty string for a release.
    \note  Overridable by the build system.
    \note  Default: "" (release).
*/
#ifndef STK_VERSION_PRERELEASE
    #define STK_VERSION_PRERELEASE  ""
#endif

/*! \def   STK_REVISION_HEADER
    \brief If defined, stk_revision.h is included to provide the build metadata (STK_VERSION_REVISION
           and STK_VERSION_GIT_HASH).
    \note  Defined by CMake (ENABLE_REVISION=ON). Build metadata does not take part in version ordering.
*/
#ifdef STK_REVISION_HEADER
    #include "stk_revision.h"
#endif

/*! \def   STK_VERSION_REVISION
    \brief Build metadata: git commit count.
    \note  Provided by the generated stk_revision.h.
    \note  Default: 0 (unknown), used if stk_revision.h is absent (tarball, other build systems,
           or ENABLE_REVISION=OFF).
*/
#ifndef STK_VERSION_REVISION
    #define STK_VERSION_REVISION    0
#endif

/*! \def   STK_VERSION_GIT_HASH
    \brief Build metadata: short git hash string.
    \note  Provided by the generated stk_revision.h. Has the "-dirty" suffix if the working tree is modified.
    \note  Default: "unknown", used if stk_revision.h is absent (tarball, other build systems,
           or ENABLE_REVISION=OFF).
*/
#ifndef STK_VERSION_GIT_HASH
    #define STK_VERSION_GIT_HASH    "unknown"
#endif

/*! \def       STK_VERSION_STR_
    \brief     Stringification helper (internal, do not use directly).
    \param[in] x: Token to stringify without macro expansion.
    \see       STK_VERSION_STR
*/
#define STK_VERSION_STR_(x) #x

/*! \def       STK_VERSION_STR
    \brief     Stringifies a value after expanding it if it is a macro.
    \param[in] x: Token or macro to expand and stringify.
*/
#define STK_VERSION_STR(x)  STK_VERSION_STR_(x)

/*! \def   STK_MINOR_STR
    \brief Minor version as a string literal (internal, derived value, do not edit).
    \note  Conditional formatting: adds a leading '0' if the minor version is less than 10.
    \see   STK_VERSION_STRING
*/
#if (STK_VERSION_MINOR < 10)
    #define STK_MINOR_STR "0" STK_VERSION_STR(STK_VERSION_MINOR)
#else
    #define STK_MINOR_STR STK_VERSION_STR(STK_VERSION_MINOR)
#endif

/*! \def   STK_VERSION_STRING
    \brief Version string literal: "1.8.0" or "1.8.0-rc1" (SemVer, no leading zeros).
    \note  Derived value, do not edit.
    \see   STK_VERSION_PRERELEASE, STK_MINOR_STR
*/
#define STK_VERSION_STRING  STK_VERSION_STR(STK_VERSION_MAJOR) "." \
                            STK_MINOR_STR "." \
                            STK_VERSION_STR(STK_VERSION_PATCH) \
                            STK_VERSION_PRERELEASE

/*! \def       STK_VERSION_ENCODE
    \brief     Packs a version into 0x00MMmmpp.
    \param[in] MAJ: Major version number (0..255).
    \param[in] MIN: Minor version number (0..255).
    \param[in] PAT: Patch version number (0..255).
    \note      Usable in \c #if and in C/C++ expressions, safe on 16-bit int targets (unsigned long arithmetic).
    \see       STK_VERSION_CODE
*/
#define STK_VERSION_ENCODE(MAJ, MIN, PAT) \
    ((((MAJ) & 0xFFUL) << 16) | (((MIN) & 0xFFUL) << 8) | ((PAT) & 0xFFUL))

/*! \def   STK_VERSION_CODE
    \brief Packed version, CMSIS-Core style (major in the upper halfword): 0x00MMmmpp.
    \note  Packing format is frozen once released: applications compare against it.
    \see   STK_VERSION_ENCODE, STK_VERSION_AT_LEAST
*/
#define STK_VERSION_CODE \
    STK_VERSION_ENCODE(STK_VERSION_MAJOR, STK_VERSION_MINOR, STK_VERSION_PATCH)

/*! \def   STK_VERSION_CMSIS_RTOS2
    \brief Packed version, CMSIS-RTOS2 osVersion_t style: major*10,000,000 + minor*10,000 + patch.
*/
#define STK_VERSION_CMSIS_RTOS2 \
    ((STK_VERSION_MAJOR * 10000000UL) + (STK_VERSION_MINOR * 10000UL) + STK_VERSION_PATCH)

/*! \def       STK_VERSION_AT_LEAST
    \brief     Tests whether the STK version is equal to or newer than the given one.
    \param[in] MAJ: Major version number (0..255).
    \param[in] MIN: Minor version number (0..255).
    \param[in] PAT: Patch version number (0..255).
    \note      Usage: \c #if STK_VERSION_AT_LEAST(1, 8, 0)
    \see       STK_VERSION_CODE, STK_VERSION_ENCODE
*/
#define STK_VERSION_AT_LEAST(MAJ, MIN, PAT) \
    (STK_VERSION_CODE >= STK_VERSION_ENCODE(MAJ, MIN, PAT))

// Each field must fit its byte of the packed code.
#if (STK_VERSION_MAJOR > 255) || (STK_VERSION_MINOR > 255) || (STK_VERSION_PATCH > 255)
    #error "STK version fields must be in the range 0..255."
#endif

#ifdef __cplusplus

/*! \namespace stk
    \brief     Namespace of STK package.
*/
namespace stk {

/*! \struct VersionInfo
    \brief  Version information descriptor.
*/
struct VersionInfo
{
    uint8_t     major;     //!< Major version number.
    uint8_t     minor;     //!< Minor version number.
    uint8_t     patch;     //!< Patch version number.
    const char *string;    //!< Version string: "1.8.0" or "1.8.0-rc1".
    uint32_t    code;      //!< Packed version, STK_VERSION_CODE (0x00MMmmpp).
    uint32_t    revision;  //!< Build metadata: git commit count, 0 if unknown.
    const char *git_hash;  //!< Build metadata: short git hash, "<hash>-dirty" if modified, "unknown" if unavailable.
};

/*! \brief  Get version information.
    \return VersionInfo structure containing major, minor, patch, version string, packed code, revision and git hash.
*/
static constexpr VersionInfo GetVersion()
{
    return
    {
        STK_VERSION_MAJOR,
        STK_VERSION_MINOR,
        STK_VERSION_PATCH,
        STK_VERSION_STRING,
        STK_VERSION_CODE,
        STK_VERSION_REVISION,
        STK_VERSION_GIT_HASH
    };
}

} // namespace stk

#endif // __cplusplus

#endif /* STK_VERSION_H_ */
