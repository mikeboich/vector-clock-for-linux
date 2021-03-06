/*

 Copyright (C) 2016-2021 Michael Boich

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 Routines to compile strings, menus, and other primitives into display lists


* Based on the excellent work of Aaron Stokes and David Forbes
* ========================================
*/

#include <string.h> 

#ifndef font_h 
#define font_h
//#include <device.h>
/* Define the vector font types:

    Each character is a series of segments, and each segment contains the following:
        XOffset     (offset from lower left box in which character is being rendered)
        YOffset     (likewise)
        XSize       magnitude of sinusoid in X
        YSize       likewise
        shape       0 for sin, -sin, 1 for sin, sin, and 2 for sin, cos
        blanking    8 bit mask for blanking (or not) each of the 8 segments of the cir


// I set the PSOC analog mux up so that mux0 = /, 1 = circle, and 2 = \, so some translating is required
typedef enum  shape {neg,circle,pos} shape_type;

*/

// NOTE: Without the "__attribute__((packed))" attribute below, the binary data layout will not coincide with the binary data on the
// baremetal side.  Don't lose this magic incantation!
typedef enum __attribute__((packed)) {cir,legacy_pos,legacy_neg,pos,neg,lissajou0,lissajou1, lissajou2,lissajou3,lissajou4,lissajou5} shape;

#define uint8 unsigned char



typedef  struct {
    uint8 x_offset;
    uint8 y_offset;
    uint8 x_size;
    uint8 y_size;
    shape arc_type;
    uint8 mask;
} vc_segment;

typedef  union{
        vc_segment seg_data;
        uint8   flag;
} seg_or_flag;

typedef seg_or_flag vector_font[];

//const int kerning=4;

extern seg_or_flag *system_font[128];

void init_font();
uint8 pin(int x);
int char_width(char c);
uint8 stringWidth(char s[],uint8 scale);

#endif

/* [] END OF FILE */
