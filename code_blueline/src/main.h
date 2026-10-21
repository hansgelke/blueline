//
// Created by Hans Gelke on 21.06.2026.
//

#ifndef SRC_MAIN_H
#define SRC_MAIN_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* MCP23017 IODIR register values                                      */
/* Written to the IODIRA/IODIRB register to set pin directions        */
/* ------------------------------------------------------------------ */
#define MCP_ALL_OUTPUTS 0x00    /* all pins configured as outputs     */
#define MCP_ALL_INPUTS  0xFF    /* all pins configured as inputs      */

/* ------------------------------------------------------------------ */
/* Byte mask constants                                                 */
/* ------------------------------------------------------------------ */
#define LOWER_NIBBLE    0x0F    /* bits 3:0                           */
#define UPPER_NIBBLE    0xF0    /* bits 7:4                           */
#define BYTE_MASK       0xFF    /* all 8 bits                         */
#define MASK_HIGH       0x0F    /* upper nibble cleared, lower active */

#endif /* SRC_MAIN_H */