#ifndef BRANDING_H
#define BRANDING_H

// Product identity shown on the LCD (boot logo, title bar of the home page).
// Change it here; nothing else in the firmware depends on these texts.
//
// PRODUCT_NAME    : title bar of the home page, name in the binary (8 characters).
// LOGO_LEFT/_BOX  : the two parts of the name in the boot logo, double size; the
//                   second one is drawn in a filled box (8 characters together).
// PRODUCT_TAGLINE : up to 13 characters, boot logo.
// FW_VERSION      : up to 8 characters.
//
// Only ASCII 32..126 can be displayed.
#define PRODUCT_NAME     "DiskIIEM"
#define LOGO_LEFT        "DiskII"
#define LOGO_BOX         "EM"
#define PRODUCT_TAGLINE  "DISK EMULATOR"
#define FW_VERSION       "v1.2.1"

#endif // BRANDING_H
