/*
gl_draw.c - orthogonal drawing stuff
Copyright (C) 2010 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "r_local.h"

/*
=============
R_GetImageParms
=============
*/
void R_GetTextureParms( int *w, int *h, int texnum )
{
	image_t *glt = R_GetTexture( texnum );

	if( w )
		*w = glt->srcWidth;
	if( h )
		*h = glt->srcHeight;
}

/*
=============
R_StretchRow

One row of R_DrawStretchPicImplementation, a loop for each case
=============
*/
static inline __attribute__(( always_inline )) void R_StretchRowCase( pixel_t *dest, const pixel_t *source, uint fstep, uint w,
	int alpha1, qboolean transparent, qboolean white, pixel_t color, int mode )
{
	uint f = 0;

	for( uint u = 0; u < w; u++ )
	{
		pixel_t src = source[f >> 16];
		int     alpha = alpha1;
		f += fstep;

		if( transparent )
		{
			alpha &= src >> ( 16 - 3 );
			src = src << 3;
		}

		if( alpha == 0 )
			continue;

		if( !white )
			src = vid.modmap[( src & 0xff00 ) | ( color >> 8 )] << 8 | ( src & color & 0xff ) | (( src & 0xff ) >> 3 );

		if( mode == kRenderTransAdd )
		{
			pixel_t screen = dest[u];
			dest[u] = vid.addmap[( src & 0xff00 ) | ( screen >> 8 )] << 8 | ( screen & 0xff ) | (( src & 0xff ) >> 0 );
		}
		else if( mode == kRenderScreenFadeModulate )
		{
			pixel_t screen = dest[u];
			dest[u] = BLEND_COLOR( screen, color );
		}
		else if( alpha < 7 )
		{
			pixel_t screen = dest[u];
			dest[u] = BLEND_ALPHA( alpha, src, screen );
		}
		else
			dest[u] = src;
	}
}

#define STRETCH_MODES( transparent, white ) \
	if( mode == kRenderTransAdd ) \
		R_StretchRowCase( dest, source, fstep, w, alpha1, transparent, white, color, kRenderTransAdd ); \
	else if( mode == kRenderScreenFadeModulate ) \
		R_StretchRowCase( dest, source, fstep, w, alpha1, transparent, white, color, kRenderScreenFadeModulate ); \
	else \
		R_StretchRowCase( dest, source, fstep, w, alpha1, transparent, white, color, kRenderNormal )

static void R_StretchRow( pixel_t *dest, const pixel_t *source, uint fstep, uint w, int alpha1, qboolean transparent, pixel_t color, int mode )
{
	if( transparent )
	{
		if( color == COLOR_WHITE )
			STRETCH_MODES( true, true );
		else
			STRETCH_MODES( true, false );
	}
	else
	{
		if( color == COLOR_WHITE )
			STRETCH_MODES( false, true );
		else
			STRETCH_MODES( false, false );
	}
}

#undef STRETCH_MODES

/*
=============
Draw_StretchPicImplementation
=============
*/
static void R_DrawStretchPicImplementation( int x, int y, int w, int h, int s1, int t1, int s2, int t2, image_t *pic )
{
	int      skip;
	qboolean transparent = false;
	pixel_t  *buffer;

	if( x < 0 )
	{
		s1 += ( -x ) * ( s2 - s1 ) / w;
		x = 0;
	}
	if( x + w > vid.width )
	{
		s2 -= ( x + w - vid.width ) * ( s2 - s1 ) / w;
		w = vid.width - x;
	}
	if( y + h > vid.height )
	{
		t2 -= ( y + h - vid.height ) * ( t2 - t1 ) / h;
		h = vid.height - y;
	}

	if( !pic->pixels[0] || s1 >= s2 || t1 >= t2 )
		return;

	// gEngfuncs.Con_Printf ("pixels is %p\n", pic->pixels[0] );

	unsigned int height = h;

	if( y < -h ) // out of display, out of bounds
		return;

	if( y < 0 )
	{
		skip = -y;
		height += y;
		y = 0;
	}
	else
		skip = 0;

	if( pic->alpha_pixels )
	{
		buffer = pic->alpha_pixels;
		transparent = true;
	}
	else
		buffer = pic->pixels[0];


	// The rows (MiSTer, 2026-10-28): the menu's 640x480 is a stretched
	// background (twelve pictures, 800x600 to 640x480) and text, about 380000
	// pixels every frame, and the loop read vid.color, vid.rendermode and
	// vid.alpha again for every pixel (a store to a pixel_t may change
	// vid.color) and divided once per row (the Cortex-A9 has no divide
	// instruction). Now the opaque rows are a copy from columns worked out
	// once, every other case has a loop of its own (R_StretchRow), and the
	// rows step without a division: the same pixels.
	{
		const int    alpha1 = vid.alpha;
		const pixel_t color = vid.color;
		const int    mode = vid.rendermode;
		const uint   fstep = (( s2 - s1 ) << 16 ) / w;
		const int    dt = t2 - t1;
		// sv = ( skip + v ) * dt / h + t1, stepped: sv_q the quotient, sv_r the remainder
		uint         sv_q = (uint)( skip * dt ) / (uint)h, sv_r = (uint)( skip * dt ) % (uint)h;
		const uint   dq = (uint)dt / (uint)h, dr = (uint)dt % (uint)h;

		// opaque: the picture's pixels as they are (the menu's background),
		// each row from the same columns, their byte offsets worked out once
		qboolean       opaque = !transparent && color == COLOR_WHITE && alpha1 >= 7
			&& mode != kRenderTransAdd && mode != kRenderScreenFadeModulate;
		static uint    *cols;
		static uint    cols_size;

		if( alpha1 == 0 )
			return; // every pixel skipped

		if( opaque && fstep != 1 << 16 )
		{
			uint f = 0;

			if( cols_size < (uint)w )
			{
				uint *more = realloc( cols, w * sizeof( *cols ));

				if( more )
				{
					cols = more;
					cols_size = w;
				}
			}
			if( cols_size < (uint)w )
				opaque = false; // R_StretchRow's loop does the same
			else
			{
				for( uint u = 0; u < w; u++ )
				{
					cols[u] = ( f >> 16 ) * sizeof( pixel_t );
					f += fstep;
				}
			}
		}

		for( uint v = 0; v < height; v++ )
		{
			pixel_t       *dest = vid.buffer + ( y + v ) * vid.rowbytes + x;
			const pixel_t *source = buffer + ( sv_q + t1 ) * pic->width + s1;

			if( !opaque )
				R_StretchRow( dest, source, fstep, w, alpha1, transparent, color, mode );
			else if( fstep == 1 << 16 )
				memcpy( dest, source, w * sizeof( pixel_t ));
			else
			{
				const byte *row = (const byte *)source;

				for( uint u = 0; u < w; u++ )
					dest[u] = *(const pixel_t *)( row + cols[u] );
			}
			sv_q += dq;
			sv_r += dr;
			if( sv_r >= (uint)h )
			{
				sv_q++;
				sv_r -= h;
			}
		}
	}
}


/*
=============
R_DrawStretchPic
=============
*/
void GAME_EXPORT R_DrawStretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, int texnum )
{
	image_t *pic = R_GetTexture( texnum );
	int     width = pic->width, height = pic->height;
//	GL_Bind( XASH_TEXTURE0, texnum );
	if( s2 > 1.0f || t2 > 1.0f )
		return;
	if( s1 < 0.0f || t1 < 0.0f )
		return;
	if( w < 1.0f || h < 1.0f )
		return;
	R_DrawStretchPicImplementation( x + vid.offset2d[0], y + vid.offset2d[1], w, h, width * s1, height * t1, width * s2, height * t2, pic );
}

void Draw_Fill( int x, int y, int w, int h )
{
	pixel_t src = vid.color;
	int     alpha = vid.alpha;

	if( x < 0 )
		x = 0;

	if( x + w > vid.width )
		w = vid.width - x;

	if( w <= 0 )
		return;

	if( y + h > vid.height )
		h = vid.height - y;

	if( h <= 0 )
		return;

	unsigned int height = h;
	if( y < 0 )
	{
		if( h <= -y )
			return;
		height += y;
		y = 0;
	}

	// the same for every pixel, decided once (MiSTer, 2026-10-28: the loop
	// read vid.rendermode again for each, as in R_DrawStretchPicImplementation)
	const int mode = vid.rendermode;

	if( alpha == 0 )
		return;

	for( uint v = 0; v < height; v++ )
	{
		pixel_t *dest = vid.buffer + ( y + v ) * vid.rowbytes + x;

		if( mode == kRenderTransAdd )
		{
			for( uint u = 0; u < w; u++ )
			{
				pixel_t screen = dest[u];
				dest[u] = vid.addmap[( src & 0xff00 ) | ( screen >> 8 )] << 8 | ( screen & 0xff ) | (( src & 0xff ) >> 0 );
			}
		}
		else if( alpha > 3 && alpha < 7 )
		{
			// BLEND_ALPHA( alpha, src, screen ), its part of the table for src
			const pixel_t *map = vid.alphamap + ((( 7 - 1 - alpha ) << 18 ) | ( src >> 6 ));

			for( uint u = 0; u < w; u++ )
				dest[u] = map[( dest[u] & 0xff00 ) << 2] | ( src & 0x3f );
		}
		else if( alpha < 7 ) // && (vid.rendermode == kRenderTransAlpha || vid.rendermode == kRenderTransTexture ) )
		{
			// the dialogs' darkened screen (alpha 2): BLEND_ALPHA( alpha, src, screen )
			const pixel_t *map = vid.alphamap + ((( alpha - 1 ) << 18 ) | (( src & 0xff00 ) << 2 ));

			for( uint u = 0; u < w; u++ )
			{
				pixel_t screen = dest[u];
				dest[u] = map[screen >> 6] | ( screen & 0x3f );
			}
		}
		else
		{
			for( uint u = 0; u < w; u++ )
				dest[u] = src;
		}
	}
}

/*
=============
GL_UpdateTexture
=============
*/
void GAME_EXPORT GL_UpdateTexture( int texnum, int cols, int rows, int width, int height, const byte *buffer, pixformat_t fmt )
{
}

/*
===============
R_Set2DMode
===============
*/
void GAME_EXPORT R_Set2DMode( qboolean enable )
{
	vid.color = COLOR_WHITE;
	vid.is2d = enable;
	vid.alpha = 7;

	if( enable )
	{
		RI.currententity = NULL;
		RI.currentmodel = NULL;
	}
}

/*
===============
R_Set2DOffset
===============
*/
void GAME_EXPORT R_Set2DOffset( float x, float y )
{
	Vector2Set( vid.offset2d, x, y );
}
