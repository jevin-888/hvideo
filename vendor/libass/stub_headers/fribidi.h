/*
 * Stub fribidi header for libass compilation
 * This is a minimal header to allow libass to compile without fribidi
 */

#ifndef FRIBIDI_H
#define FRIBIDI_H

#include <stdint.h>

/* Minimal fribidi types and constants needed by libass */
typedef uint32_t FriBidiChar;
typedef int FriBidiStrIndex;
typedef unsigned char FriBidiCharType;
typedef unsigned char FriBidiLevel;
typedef unsigned char FriBidiBracketType;
typedef unsigned char FriBidiParType;

/* Constants */
#define FRIBIDI_TYPE_LTR 0
#define FRIBIDI_TYPE_RTL 1
#define FRIBIDI_PAR_ON 0
#define FRIBIDI_PAR_LTR 0
#define FRIBIDI_PAR_RTL 1

/* Version info */
#define FRIBIDI_MAJOR_VERSION 1
#define FRIBIDI_MINOR_VERSION 0
#define FRIBIDI_MICRO_VERSION 0
#define FRIBIDI_VERSION "1.0.0"

#endif /* FRIBIDI_H */
