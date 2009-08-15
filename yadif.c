/*
  Yadif C-plugin for Avisynth 2.5 - Yet Another DeInterlacing Filter
  Copyright (C)2007 Alexander G. Balakhnin aka Fizick  http://avisynth.org.ru
  Port of YADIF filter from MPlayer
  Copyright (C) 2006 Michael Niedermayer <michaelni@gmx.at>

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

  Avisynth_C plugin
  Assembler optimized for GNU C compiler

*/
#include <gst/avsynth/gstavsynth_sdk.h>
#include <stdlib.h>
#include <memory.h>

#ifndef MIN
#define MIN(a,b) ((a) > (b) ? (b) : (a))
#endif
#ifndef MAX
#define MAX(a,b) ((a) < (b) ? (b) : (a))
#endif
#ifndef ABS
#define ABS(a) ((a) > 0 ? (a) : (-(a)))
#endif

#define MIN3(a,b,c) MIN(MIN(a,b),c)
#define MAX3(a,b,c) MAX(MAX(a,b),c)

#define uint8_t unsigned char

typedef struct _AVS_Yadif AVS_Yadif;

struct _AVS_Yadif
{
  AVS_GenericVideoFilter parent;
  AVS_Clip *child;
  AVS_DestroyFunc destroy;

  int mode;
  int order;
  int planar_hack; // bool
  int cpu; // optimization
  int yheight;
  int ypitch;
  int uvpitch;
  int ywidth;
  int uvwidth;
  unsigned char *ysrc;
  unsigned char *usrc;
  unsigned char *vsrc;
  unsigned char *yprev;
  unsigned char *uprev;
  unsigned char *vprev;
  unsigned char *ynext;
  unsigned char *unext;
  unsigned char *vnext;
  unsigned char *ydest;
  unsigned char *udest;
  unsigned char *vdest;

  AVS_VideoInfo vi;
  AVS_ScriptEnvironment *env;
};

AVS_VideoFrame * AVSC_CC yadif_get_frame (AVS_Clip *_p, gint64 ndst);

void AVSC_CC
yadif_destroy(AVS_Clip *p, gboolean freeself)
{
  AVS_Yadif *yada = (AVS_Yadif *)p;
  
  if (AVS_IS_YUY2(&yada->parent.vi) && yada->planar_hack == 0)
  {
    free(yada->ysrc);
    free(yada->usrc);
    free(yada->vsrc);
    free(yada->yprev);
    free(yada->uprev);
    free(yada->vprev);
    free(yada->ynext);
    free(yada->unext);
    free(yada->vnext);
    free(yada->ydest);
    free(yada->udest);
    free(yada->vdest);
  }

  yada->parent.destroy ((gpointer) &yada->parent, FALSE);

  if (freeself)
    g_free (yada);
}

AVS_Yadif * AVSC_CC
yadif_construct(gpointer p, AVS_ScriptEnvironment *e, AVS_Value args)
{
  AVS_Yadif *yada;
  AVS_Clip *clip_self;
  AVS_Value tmp;
  AVS_Clip *next = e->avs_val_take_clip(AVS_ARRAY_ELT(args, 0));

  if (!p)
    yada = g_new0 (AVS_Yadif, 1);
  else
    yada = p;

  yada->destroy = yadif_destroy;
  yada->child = NULL;

  clip_self = (AVS_Clip *)yada;
  
  if (!clip_self->get_frame)
    clip_self->get_frame = yadif_get_frame;

  /* Call parent constructor chain. This eventually initializes the
   * class chain and gives yada refcount=1
   */
  e->avs_gvf_construct (&yada->parent, next);

  yada->parent.child = clip_self;
  /* _avs_gvf_construct ref'ed the "next" clip, we can release it now.
   * 0'th AVS_Value _still_ holds a ref to it, but that's not OUR problem.
   */
  e->avs_clip_release(next);

  yada->env = e;
  yada->vi = yada->parent.vi;

  if (AVS_IS_PLANAR(&yada->vi) || AVS_IS_YUY2(&yada->vi)) {
    int opt;
    //   get first argument
    tmp = AVS_ARRAY_ELT(args, 1); // argument mode
    if (AVS_DEFINED(tmp))
      yada->mode = AVS_AS_INT(tmp);
    else
      yada->mode = 0 ;  // set default value = 0 if no argument

    if (yada->mode & 1)
    {// bob mode, double number of frames and fps
      yada->vi.num_frames = yada->vi.num_frames * 2;
      if (yada->vi.fps_denominator & 1)
        yada->vi.fps_numerator = yada->vi.fps_numerator * 2;
      else
        yada->vi.fps_denominator >>= 1;
    }

    tmp = AVS_ARRAY_ELT(args, 2); // argument order
    if (AVS_DEFINED(tmp))
      yada->order = AVS_AS_INT(tmp);
    else
      yada->order = -1 ;  // set default value = -1 if no argument

    tmp = AVS_ARRAY_ELT(args, 3); // argument planar (SSETools ny Kassandro)
    if (AVS_DEFINED(tmp))
      yada->planar_hack = AVS_AS_BOOL(tmp);
    else
      yada->planar_hack = 0; // default false

    tmp = AVS_ARRAY_ELT(args, 4); // argument opt
    if (AVS_DEFINED(tmp))
      opt = AVS_AS_INT(tmp);
    else
      opt = -1 ;  // set default value = -1 (auto) if no argument

    yada->cpu = e->avs_se_get_cpu_flags(e);

    if (opt == 0)
      yada->cpu = 0;// pure C
    else if (opt == 1 )
      yada->cpu &= AVS_CPU_INTEGER_SSE;
    else if (opt == 2 )
      yada->cpu &= (AVS_CPU_INTEGER_SSE | AVS_CPU_SSE2);
    else if (opt == 3 )
      yada->cpu &= (AVS_CPU_INTEGER_SSE | AVS_CPU_SSE2 | AVS_CPU_SSE3);

  } else {
    /* unref yada, calls destructor chain internally. ALWAYS use base (clip)
     * destructor rather than your own destructor.
     * Calling your own derived destructor (yada_destroy) may appear to be
     * working, but it will break things if someone subclasses your filter,
     * and you will never know (although subclassing is unlikely unless
     * you've exposed all the necessary bits in the header and passed the
     * function pointers too)
     */
    e->avs_clip_release (clip_self);
    /* yada is now invalid */
    yada = NULL;
  }

  if (yada && AVS_IS_YUY2(&yada->vi) && yada->planar_hack == 0)
  {
    // create intermediate planar planes
    yada->yheight = yada->vi.height;
    yada->ywidth = yada->vi.width;
    yada->uvwidth = yada->ywidth/2;
    yada->ypitch = ((yada->ywidth)+15)/16*16;
    yada->uvpitch = ((yada->uvwidth)+15)/16*16;
    yada->ysrc = (guint8 *) malloc(yada->yheight*yada->ypitch);
    yada->usrc = (guint8 *) malloc(yada->yheight*yada->uvpitch);
    yada->vsrc = (guint8 *) malloc(yada->yheight*yada->uvpitch);
    yada->yprev = (guint8 *) malloc(yada->yheight*yada->ypitch);
    yada->uprev = (guint8 *) malloc(yada->yheight*yada->uvpitch);
    yada->vprev = (guint8 *) malloc(yada->yheight*yada->uvpitch);
    yada->ynext = (guint8 *) malloc(yada->yheight*yada->ypitch);
    yada->unext = (guint8 *) malloc(yada->yheight*yada->uvpitch);
    yada->vnext = (guint8 *) malloc(yada->yheight*yada->uvpitch);
    yada->ydest = (guint8 *) malloc(yada->yheight*yada->ypitch);
    yada->udest = (guint8 *) malloc(yada->yheight*yada->uvpitch);
    yada->vdest = (guint8 *) malloc(yada->yheight*yada->uvpitch);
  }

  return yada;
}

/* This is the callback type used by avs_add_function */
AVS_Value AVSC_CC
yadif_apply (AVS_ScriptEnvironment *env, AVS_Value args, gpointer user_data)
{
  AVS_Value v;
  AVS_Yadif *f = yadif_construct (NULL, env, args);
  
  if (f == NULL)
    v = env->avs_val_new_error ("Video must be YUV");
  else
  {
    v = env->avs_val_new_clip ((AVS_Clip*) f);
    /* avs_val_new_clip() refs yada once more. Unref it to keep the balance */
    env->avs_clip_release ((AVS_Clip*) f);
  }
  return v;
}

static void (*filter_line)(int mode, uint8_t *dst, const uint8_t *prev, const uint8_t *cur, const uint8_t *next, int w, int refs, int parity);

#ifdef __GNUC__
#define uint64_t unsigned __int64
#define LOAD4(mem,dst) \
            "movd      "mem", "#dst" \n\t"\
            "punpcklbw %%mm7, "#dst" \n\t"

#define PABS(tmp,dst) \
            "pxor     "#tmp", "#tmp" \n\t"\
            "psubw    "#dst", "#tmp" \n\t"\
            "pmaxsw   "#tmp", "#dst" \n\t"

#define CHECK(pj,mj) \
            "movq "#pj"(%[cur],%[mrefs]), %%mm2 \n\t" /* cur[x-refs-1+j] */\
            "movq "#mj"(%[cur],%[prefs]), %%mm3 \n\t" /* cur[x+refs-1-j] */\
            "movq      %%mm2, %%mm4 \n\t"\
            "movq      %%mm2, %%mm5 \n\t"\
            "pxor      %%mm3, %%mm4 \n\t"\
            "pavgb     %%mm3, %%mm5 \n\t"\
            "pand     %[pb1], %%mm4 \n\t"\
            "psubusb   %%mm4, %%mm5 \n\t"\
            "psrlq     $8,    %%mm5 \n\t"\
            "punpcklbw %%mm7, %%mm5 \n\t" /* (cur[x-refs+j] + cur[x+refs-j])>>1 */\
            "movq      %%mm2, %%mm4 \n\t"\
            "psubusb   %%mm3, %%mm2 \n\t"\
            "psubusb   %%mm4, %%mm3 \n\t"\
            "pmaxub    %%mm3, %%mm2 \n\t"\
            "movq      %%mm2, %%mm3 \n\t"\
            "movq      %%mm2, %%mm4 \n\t" /* ABS(cur[x-refs-1+j] - cur[x+refs-1-j]) */\
            "psrlq      $8,   %%mm3 \n\t" /* ABS(cur[x-refs  +j] - cur[x+refs  -j]) */\
            "psrlq     $16,   %%mm4 \n\t" /* ABS(cur[x-refs+1+j] - cur[x+refs+1-j]) */\
            "punpcklbw %%mm7, %%mm2 \n\t"\
            "punpcklbw %%mm7, %%mm3 \n\t"\
            "punpcklbw %%mm7, %%mm4 \n\t"\
            "paddw     %%mm3, %%mm2 \n\t"\
            "paddw     %%mm4, %%mm2 \n\t" /* score */

#define CHECK1 \
            "movq      %%mm0, %%mm3 \n\t"\
            "pcmpgtw   %%mm2, %%mm3 \n\t" /* if(score < spatial_score) */\
            "pminsw    %%mm2, %%mm0 \n\t" /* spatial_score= score; */\
            "movq      %%mm3, %%mm6 \n\t"\
            "pand      %%mm3, %%mm5 \n\t"\
            "pandn     %%mm1, %%mm3 \n\t"\
            "por       %%mm5, %%mm3 \n\t"\
            "movq      %%mm3, %%mm1 \n\t" /* spatial_pred= (cur[x-refs+j] + cur[x+refs-j])>>1; */

#define CHECK2 /* pretend not to have checked dir=2 if dir=1 was bad.\
                  hurts both quality and speed, but matches the C version. */\
            "paddw    %[pw1], %%mm6 \n\t"\
            "psllw     $14,   %%mm6 \n\t"\
            "paddsw    %%mm6, %%mm2 \n\t"\
            "movq      %%mm0, %%mm3 \n\t"\
            "pcmpgtw   %%mm2, %%mm3 \n\t"\
            "pminsw    %%mm2, %%mm0 \n\t"\
            "pand      %%mm3, %%mm5 \n\t"\
            "pandn     %%mm1, %%mm3 \n\t"\
            "por       %%mm5, %%mm3 \n\t"\
            "movq      %%mm3, %%mm1 \n\t"

static void filter_line_mmx2(int mode, uint8_t *dst, const uint8_t *prev, const uint8_t *cur, const uint8_t *next, int w, int refs, int parity){
    static const uint64_t pw_1 = 0x0001000100010001ULL;
    static const uint64_t pb_1 = 0x0101010101010101ULL;
//    const int mode = p->mode;
    uint64_t tmp0, tmp1, tmp2, tmp3;
    int x;

#define FILTER\
    for(x=0; x<w; x+=4){\
        asm volatile(\
            "pxor      %%mm7, %%mm7 \n\t"\
            LOAD4("(%[cur],%[mrefs])", %%mm0) /* c = cur[x-refs] */\
            LOAD4("(%[cur],%[prefs])", %%mm1) /* e = cur[x+refs] */\
            LOAD4("(%["prev2"])", %%mm2) /* prev2[x] */\
            LOAD4("(%["next2"])", %%mm3) /* next2[x] */\
            "movq      %%mm3, %%mm4 \n\t"\
            "paddw     %%mm2, %%mm3 \n\t"\
            "psraw     $1,    %%mm3 \n\t" /* d = (prev2[x] + next2[x])>>1 */\
            "movq      %%mm0, %[tmp0] \n\t" /* c */\
            "movq      %%mm3, %[tmp1] \n\t" /* d */\
            "movq      %%mm1, %[tmp2] \n\t" /* e */\
            "psubw     %%mm4, %%mm2 \n\t"\
            PABS(      %%mm4, %%mm2) /* temporal_diff0 */\
            LOAD4("(%[prev],%[mrefs])", %%mm3) /* prev[x-refs] */\
            LOAD4("(%[prev],%[prefs])", %%mm4) /* prev[x+refs] */\
            "psubw     %%mm0, %%mm3 \n\t"\
            "psubw     %%mm1, %%mm4 \n\t"\
            PABS(      %%mm5, %%mm3)\
            PABS(      %%mm5, %%mm4)\
            "paddw     %%mm4, %%mm3 \n\t" /* temporal_diff1 */\
            "psrlw     $1,    %%mm2 \n\t"\
            "psrlw     $1,    %%mm3 \n\t"\
            "pmaxsw    %%mm3, %%mm2 \n\t"\
            LOAD4("(%[next],%[mrefs])", %%mm3) /* next[x-refs] */\
            LOAD4("(%[next],%[prefs])", %%mm4) /* next[x+refs] */\
            "psubw     %%mm0, %%mm3 \n\t"\
            "psubw     %%mm1, %%mm4 \n\t"\
            PABS(      %%mm5, %%mm3)\
            PABS(      %%mm5, %%mm4)\
            "paddw     %%mm4, %%mm3 \n\t" /* temporal_diff2 */\
            "psrlw     $1,    %%mm3 \n\t"\
            "pmaxsw    %%mm3, %%mm2 \n\t"\
            "movq      %%mm2, %[tmp3] \n\t" /* diff */\
\
            "paddw     %%mm0, %%mm1 \n\t"\
            "paddw     %%mm0, %%mm0 \n\t"\
            "psubw     %%mm1, %%mm0 \n\t"\
            "psrlw     $1,    %%mm1 \n\t" /* spatial_pred */\
            PABS(      %%mm2, %%mm0)      /* ABS(c-e) */\
\
            "movq -1(%[cur],%[mrefs]), %%mm2 \n\t" /* cur[x-refs-1] */\
            "movq -1(%[cur],%[prefs]), %%mm3 \n\t" /* cur[x+refs-1] */\
            "movq      %%mm2, %%mm4 \n\t"\
            "psubusb   %%mm3, %%mm2 \n\t"\
            "psubusb   %%mm4, %%mm3 \n\t"\
            "pmaxub    %%mm3, %%mm2 \n\t"\
            /*"pshufw $9,%%mm2, %%mm3 \n\t"*/\
            "movq %%mm2, %%mm3 \n\t" /* replace for "pshufw $9,%%mm2, %%mm3" - Fizick */\
            "psrlq $16, %%mm3 \n\t"/* replace for "pshufw $9,%%mm2, %%mm3" - Fizick*/\
            "punpcklbw %%mm7, %%mm2 \n\t" /* ABS(cur[x-refs-1] - cur[x+refs-1]) */\
            "punpcklbw %%mm7, %%mm3 \n\t" /* ABS(cur[x-refs+1] - cur[x+refs+1]) */\
            "paddw     %%mm2, %%mm0 \n\t"\
            "paddw     %%mm3, %%mm0 \n\t"\
            "psubw    %[pw1], %%mm0 \n\t" /* spatial_score */\
\
            CHECK(-2,0)\
            CHECK1\
            CHECK(-3,1)\
            CHECK2\
            CHECK(0,-2)\
            CHECK1\
            CHECK(1,-3)\
            CHECK2\
\
            /* if(p->mode<2) ... */\
            "movq    %[tmp3], %%mm6 \n\t" /* diff */\
            "cmp       $2, %[mode] \n\t"\
            "jge       1f \n\t"\
            LOAD4("(%["prev2"],%[mrefs],2)", %%mm2) /* prev2[x-2*refs] */\
            LOAD4("(%["next2"],%[mrefs],2)", %%mm4) /* next2[x-2*refs] */\
            LOAD4("(%["prev2"],%[prefs],2)", %%mm3) /* prev2[x+2*refs] */\
            LOAD4("(%["next2"],%[prefs],2)", %%mm5) /* next2[x+2*refs] */\
            "paddw     %%mm4, %%mm2 \n\t"\
            "paddw     %%mm5, %%mm3 \n\t"\
            "psrlw     $1,    %%mm2 \n\t" /* b */\
            "psrlw     $1,    %%mm3 \n\t" /* f */\
            "movq    %[tmp0], %%mm4 \n\t" /* c */\
            "movq    %[tmp1], %%mm5 \n\t" /* d */\
            "movq    %[tmp2], %%mm7 \n\t" /* e */\
            "psubw     %%mm4, %%mm2 \n\t" /* b-c */\
            "psubw     %%mm7, %%mm3 \n\t" /* f-e */\
            "movq      %%mm5, %%mm0 \n\t"\
            "psubw     %%mm4, %%mm5 \n\t" /* d-c */\
            "psubw     %%mm7, %%mm0 \n\t" /* d-e */\
            "movq      %%mm2, %%mm4 \n\t"\
            "pminsw    %%mm3, %%mm2 \n\t"\
            "pmaxsw    %%mm4, %%mm3 \n\t"\
            "pmaxsw    %%mm5, %%mm2 \n\t"\
            "pminsw    %%mm5, %%mm3 \n\t"\
            "pmaxsw    %%mm0, %%mm2 \n\t" /* max */\
            "pminsw    %%mm0, %%mm3 \n\t" /* min */\
            "pxor      %%mm4, %%mm4 \n\t"\
            "pmaxsw    %%mm3, %%mm6 \n\t"\
            "psubw     %%mm2, %%mm4 \n\t" /* -max */\
            "pmaxsw    %%mm4, %%mm6 \n\t" /* diff= MAX3(diff, min, -max); */\
            "1: \n\t"\
\
            "movq    %[tmp1], %%mm2 \n\t" /* d */\
            "movq      %%mm2, %%mm3 \n\t"\
            "psubw     %%mm6, %%mm2 \n\t" /* d-diff */\
            "paddw     %%mm6, %%mm3 \n\t" /* d+diff */\
            "pmaxsw    %%mm2, %%mm1 \n\t"\
            "pminsw    %%mm3, %%mm1 \n\t" /* d = clip(spatial_pred, d-diff, d+diff); */\
            "packuswb  %%mm1, %%mm1 \n\t"\
\
            :[tmp0]"=m"(tmp0),\
             [tmp1]"=m"(tmp1),\
             [tmp2]"=m"(tmp2),\
             [tmp3]"=m"(tmp3)\
            :[prev] "r"(prev),\
             [cur]  "r"(cur),\
             [next] "r"(next),\
             [prefs]"r"((long)refs),\
             [mrefs]"r"((long)-refs),\
             [pw1]  "m"(pw_1),\
             [pb1]  "m"(pb_1),\
             [mode] "g"(mode)\
        );\
        asm volatile("movd %%mm1, %0" :"=m"(*dst));\
        dst += 4;\
        prev+= 4;\
        cur += 4;\
        next+= 4;\
    }

    if(parity){
#define prev2 "prev"
#define next2 "cur"
        FILTER
#undef prev2
#undef next2
    }else{
#define prev2 "cur"
#define next2 "next"
        FILTER
#undef prev2
#undef next2
    }
}
#undef LOAD4
#undef PABS
#undef CHECK
#undef CHECK1
#undef CHECK2
#undef FILTER

#ifndef attribute_align_arg
#if defined(__GNUC__) && (__GNUC__ > 4 || __GNUC__ == 4 && __GNUC_MINOR__>1)
#    define attribute_align_arg __attribute__((force_align_arg_pointer))
#else
#    define attribute_align_arg
#endif
#endif

// for proper alignment SSE2 we need in GCC 4.2 and above
#if (__GNUC__ > 4 || __GNUC__ == 4 && __GNUC_MINOR__>1)

#ifndef DECLARE_ALIGNED
#define DECLARE_ALIGNED(n,t,v)       t v __attribute__ ((aligned (n)))
#endif

// ================= SSE2 =================
#define PABS(tmp,dst) \
            "pxor     "#tmp", "#tmp" \n\t"\
            "psubw    "#dst", "#tmp" \n\t"\
            "pmaxsw   "#tmp", "#dst" \n\t"

#define FILTER_LINE_FUNC_NAME filter_line_sse2
#include "vf_yadif_template.h"

// ================ SSSE3 =================
#define PABS(tmp,dst) \
            "pabsw     "#dst", "#dst" \n\t"

#define FILTER_LINE_FUNC_NAME filter_line_ssse3
#include "vf_yadif_template.h"

#endif

#endif

static void filter_line_c(int mode, uint8_t *dst, const uint8_t *prev, const uint8_t *cur, const uint8_t *next, int w, int refs, int parity){
    int x;
    const uint8_t *prev2= parity ? prev : cur ;
    const uint8_t *next2= parity ? cur  : next;
    for(x=0; x<w; x++){
        int c= cur[-refs];
        int d= (prev2[0] + next2[0])>>1;
        int e= cur[+refs];
        int temporal_diff0= ABS(prev2[0] - next2[0]);
        int temporal_diff1=( ABS(prev[-refs] - c) + ABS(prev[+refs] - e) )>>1;
        int temporal_diff2=( ABS(next[-refs] - c) + ABS(next[+refs] - e) )>>1;
        int diff= MAX3(temporal_diff0>>1, temporal_diff1, temporal_diff2);
        int spatial_pred= (c+e)>>1;
        int spatial_score= ABS(cur[-refs-1] - cur[+refs-1]) + ABS(c-e)
                         + ABS(cur[-refs+1] - cur[+refs+1]) - 1;

#define CHECK(j)\
    {   int score= ABS(cur[-refs-1+ j] - cur[+refs-1- j])\
                 + ABS(cur[-refs  + j] - cur[+refs  - j])\
                 + ABS(cur[-refs+1+ j] - cur[+refs+1- j]);\
        if(score < spatial_score){\
            spatial_score= score;\
            spatial_pred= (cur[-refs  + j] + cur[+refs  - j])>>1;\

        CHECK(-1) CHECK(-2) }} }}
        CHECK( 1) CHECK( 2) }} }}

        if(mode<2){
            int b= (prev2[-2*refs] + next2[-2*refs])>>1;
            int f= (prev2[+2*refs] + next2[+2*refs])>>1;
#if 0
            int a= cur[-3*refs];
            int g= cur[+3*refs];
            int max= MAX3(d-e, d-c, MIN3(MAX(b-c,f-e),MAX(b-c,b-a),MAX(f-g,f-e)) );
            int min= MIN3(d-e, d-c, MAX3(MIN(b-c,f-e),MIN(b-c,b-a),MIN(f-g,f-e)) );
#else
            int max= MAX3(d-e, d-c, MIN(b-c, f-e));
            int min= MIN3(d-e, d-c, MAX(b-c, f-e));
#endif

            diff= MAX3(diff, min, -max);
        }

        if(spatial_pred > d + diff)
           spatial_pred = d + diff;
        else if(spatial_pred < d - diff)
           spatial_pred = d - diff;

        dst[0] = spatial_pred;

        dst++;
        cur++;
        prev++;
        next++;
        prev2++;
        next2++;
    }
}

static void interpolate(uint8_t *dst, const uint8_t *cur0,  const uint8_t *cur2, int w)
{
    int x;
    for (x=0; x<w; x++) {
        dst[x] = (cur0[x] + cur2[x] + 1)>>1; // simple average
    }
}

static void filter_plane(int mode, uint8_t *dst, int dst_stride, const uint8_t *prev0, const uint8_t *cur0, const uint8_t *next0, int refs, int w, int h, int parity, int tff, int cpu){

  int y;
  filter_line = filter_line_c;
#ifdef __GNUC__
#if (__GNUC__ > 4 || __GNUC__ == 4 && __GNUC_MINOR__>1)
  if (cpu & AVS_CPU_SSE3)
    filter_line = filter_line_ssse3;
  else if (cpu & AVS_CPU_SSE2)
    filter_line = filter_line_sse2;
  else
#endif
  if (cpu & AVS_CPU_INTEGER_SSE)
    filter_line = filter_line_mmx2;
#endif
        y=0;
        if(((y ^ parity) & 1)){
            memcpy(dst, cur0 + refs, w);// duplicate 1
        }else{
            memcpy(dst, cur0, w);
        }
        y=1;
        if(((y ^ parity) & 1)){
            interpolate(dst + dst_stride, cur0, cur0 + refs*2, w);   // interpolate 0 and 2
        }else{
            memcpy(dst + dst_stride, cur0 + refs, w); // copy original
        }
        for(y=2; y<h-2; y++){
            if(((y ^ parity) & 1)){
                const uint8_t *prev= prev0 + y*refs;
                const uint8_t *cur = cur0 + y*refs;
                const uint8_t *next= next0 + y*refs;
                uint8_t *dst2= dst + y*dst_stride;
                filter_line(mode, dst2, prev, cur, next, w, refs, (parity ^ tff));
            }else{
                memcpy(dst + y*dst_stride, cur0 + y*refs, w); // copy original
            }
        }
       y=h-2;
        if(((y ^ parity) & 1)){
            interpolate(dst + (h-2)*dst_stride, cur0 + (h-3)*refs, cur0 + (h-1)*refs, w);   // interpolate h-3 and h-1
        }else{
            memcpy(dst + (h-2)*dst_stride, cur0 + (h-2)*refs, w); // copy original
        }
        y=h-1;
        if(((y ^ parity) & 1)){
            memcpy(dst + (h-1)*dst_stride, cur0 + (h-2)*refs, w); // duplicate h-2
        }else{
            memcpy(dst + (h-1)*dst_stride, cur0 + (h-1)*refs, w); // copy original
        }

#ifdef __GNUC__
  if (cpu >= AVS_CPU_INTEGER_SSE)
    asm volatile("emms");
#endif
}

#ifdef __GNUC__
static attribute_align_arg void  YUY2ToPlanes_mmx(const unsigned char *srcYUY2, int pitch_yuy2, int width, int height,
                    unsigned char *py, int pitch_y,
                    unsigned char *pu, unsigned char *pv,  int pitch_uv)
{ /* process by 16 bytes (8 pixels), so width is assumed mod 8 */
    int widthdiv2 = width>>1;
//    static unsigned __int64 Ymask = 0x00FF00FF00FF00FFULL;
    int h;
    for (h=0; h<height; h++)
    {
        asm (\
        "pcmpeqb %%mm5, %%mm5 \n\t"  /* prepare Ymask FFFFFFFFFFFFFFFF */\
        "psrlw $8, %%mm5 \n\t" /* Ymask = 00FF00FF00FF00FF */\
        "xor %%eax, %%eax \n\t"\
        "xloop%= : \n\t"\
        "prefetchnta 0xc0(%%edi,%%eax,4) \n\t"\
        "movq (%%edi,%%eax,4), %%mm0 \n\t" /* src VYUYVYUY - 1 */\
        "movq 8(%%edi,%%eax,4), %%mm1 \n\t" /* src VYUYVYUY - 2 */\
        "movq %%mm0, %%mm2 \n\t" /* VYUYVYUY - 1 */\
        "movq %%mm1, %%mm3 \n\t" /* VYUYVYUY - 2 */\
        "pand %%mm5, %%mm0 \n\t" /* 0Y0Y0Y0Y - 1 */\
        "psrlw $8, %%mm2 \n\t" /* 0V0U0V0U - 1 */\
        "pand %%mm5, %%mm1 \n\t" /* 0Y0Y0Y0Y - 2 */\
        "psrlw $8, %%mm3 \n\t" /* 0V0U0V0U - 2 */\
        "packuswb %%mm1, %%mm0 \n\t" /* YYYYYYYY */\
        "packuswb %%mm3, %%mm2 \n\t" /* VUVUVUVU */\
        "movntq %%mm0, (%%ebx,%%eax,2) \n\t" /* store y */\
        "movq %%mm2, %%mm4 \n\t" /* VUVUVUVU */\
        "pand %%mm5, %%mm2 \n\t" /* 0U0U0U0U */\
        "psrlw $8, %%mm4 \n\t" /* 0V0V0V0V */\
        "packuswb %%mm2, %%mm2 \n\t" /* xxxxUUUU */\
        "packuswb %%mm4, %%mm4 \n\t" /* xxxxVVVV */\
        "movd %%mm2, (%%edx,%%eax) \n\t" /* store u */\
        "add $4, %%eax \n\t" \
        "cmp %%ecx, %%eax \n\t" \
        "movd %%mm4, -4(%%esi,%%eax) \n\t" /* store v */\
        "jl xloop%= \n\t"\
        : : "D"(srcYUY2), "b"(py), "d"(pu), "S"(pv), "c"(widthdiv2) : "%eax");

        srcYUY2 += pitch_yuy2;
        py += pitch_y;
        pu += pitch_uv;
        pv += pitch_uv;
    }
    asm ("sfence \n\t emms");
}

static attribute_align_arg void YUY2FromPlanes_mmx(unsigned char *dstYUY2, int pitch_yuy2, int width, int height,
                    const unsigned char *py, int pitch_y,
                    const unsigned char *pu, const unsigned char *pv,  int pitch_uv)
{
    int widthdiv2 = width >> 1;
    int h;
    for (h=0; h<height; h++)
    {
        asm (\
        "xor %%eax, %%eax \n\t"\
        "xloop%=: \n\t"\
        "movd (%%edx,%%eax), %%mm1 \n\t" /* 0000UUUU */\
        "movd (%%esi,%%eax), %%mm2 \n\t" /* 0000VVVV */\
        "movq (%%ebx,%%eax,2), %%mm0 \n\t" /* YYYYYYYY */\
        "punpcklbw %%mm2,%%mm1 \n\t" /* VUVUVUVU */\
        "movq %%mm0, %%mm3 \n\t" /* YYYYYYYY */\
        "punpcklbw %%mm1, %%mm0 \n\t" /* VYUYVYUY */\
        "add $4, %%eax \n\t"\
        "punpckhbw %%mm1, %%mm3 \n\t" /* VYUYVYUY */\
        "movntq %%mm0, -16(%%edi,%%eax,4) \n\t" /*store */\
        "movntq %%mm3, -8(%%edi,%%eax,4) \n\t" /*  store */\
        "cmp %%ecx, %%eax \n\t"\
        "jl xloop%= \n\t"\
        : : "b"(py), "d"(pu), "S"(pv), "D"(dstYUY2), "c"(widthdiv2) : "%eax");
        py += pitch_y;
        pu += pitch_uv;
        pv += pitch_uv;
        dstYUY2 += pitch_yuy2;
    }
    asm ("sfence \n\t emms");
}
#endif

//----------------------------------------------------------------------------------------------

void YUY2ToPlanes(const unsigned char *pSrcYUY2, int nSrcPitchYUY2, int nWidth, int nHeight,
                 unsigned char * pSrcY, int srcPitchY,
                 unsigned char * pSrcU,  unsigned char * pSrcV, int srcPitchUV, int cpu)
{

    int h,w;
    int w0 = 0;
#ifdef __GNUC__
    if (cpu & AVS_CPU_INTEGER_SSE) {
        w0 = (nWidth/8)*8;
        YUY2ToPlanes_mmx(pSrcYUY2, nSrcPitchYUY2, w0, nHeight, pSrcY, srcPitchY, pSrcU, pSrcV, srcPitchUV);
    }
#endif
  for (h=0; h<nHeight; h++)
  {
    for (w=w0; w<nWidth; w+=2)
    {
      int w2 = w+w;
      pSrcY[w] = pSrcYUY2[w2];
      pSrcY[w+1] = pSrcYUY2[w2+2];
      pSrcU[(w>>1)] = pSrcYUY2[w2+1];
      pSrcV[(w>>1)] = pSrcYUY2[w2+3];
    }
    pSrcY += srcPitchY;
    pSrcU += srcPitchUV;
    pSrcV += srcPitchUV;
    pSrcYUY2 += nSrcPitchYUY2;
  }
}

//----------------------------------------------------------------------------------------------

void YUY2FromPlanes(unsigned char *pSrcYUY2, int nSrcPitchYUY2, int nWidth, int nHeight,
                const unsigned char * pSrcY, int srcPitchY,
                const unsigned char * pSrcU, const unsigned char * pSrcV, int srcPitchUV, int cpu)
{
    int h,w;
    int w0 = 0;
#ifdef __GNUC__
    if (cpu & AVS_CPU_INTEGER_SSE) {
        w0 = (nWidth/8)*8;
        YUY2FromPlanes_mmx(pSrcYUY2, nSrcPitchYUY2, w0, nHeight, pSrcY, srcPitchY, pSrcU, pSrcV, srcPitchUV);
    }
#endif

  for (h=0; h<nHeight; h++)
  {
    for (w=w0; w<nWidth; w+=2)
    {
      int w2 = w+w;
      pSrcYUY2[w2] = pSrcY[w];
      pSrcYUY2[w2+1] = pSrcU[(w>>1)];
      pSrcYUY2[w2+2] = pSrcY[w+1];
      pSrcYUY2[w2+3] = pSrcV[(w>>1)];
    }
    pSrcY += srcPitchY;
    pSrcU += srcPitchUV;
    pSrcV += srcPitchUV;
    pSrcYUY2 += nSrcPitchYUY2;
  }
}

//----------------------------------------------------------------------------------

AVS_VideoFrame * AVSC_CC yadif_get_frame (AVS_Clip *_p, gint64 ndst)
{
// This is the implementation of the GetFrame function.
// See the header definition for further info.

    int mode;
  int parity;
  int tff;
  int iplane;
  int n;
  AVS_VideoFrame *src, *dst, * prev, *next;
  int h;
        AVS_Clip *next_clip = NULL;
        AVS_VideoInfo *vi = NULL;
  AVS_ScriptEnvironment *e = NULL;

    AVS_Yadif * f = (AVS_Yadif*)_p;
    mode = f->mode;
  next_clip = f->parent.next_clip;
  /* Or you can ((AVS_GenericVideoFilter *)f)->next_clip; */

  vi = &f->parent.vi;
  /* Same as above.
   * Also, it would be more reliable to do 
   * vi = f->env->avs_clip_get_video_info (next_clip);
   * because generic video filter retreives VideoInfo only on creation,
   * but it might have changed since then.
   * that would require vi to not to be a pointer (and you'll have to
   * add & to it each time you use VI-access macros, like AVS_BLAH(&vi))
   */

  e = f->env;


  if (mode & 1)
    n = (ndst>>1); // bob
  else
    n = ndst;

  src = next_clip->get_frame(next_clip, n);
   // Request frame 'n' from the child (source) clip.

  if (n>0)
    prev = next_clip->get_frame(next_clip, n-1); // get previous frame
  else if (vi->num_frames > 1)
    prev = next_clip->get_frame(next_clip, 1); // next frame
  else
    prev = next_clip->get_frame(next_clip, 0); // cur 0 frame for one-frame clip

  if (n < vi->num_frames - 1)
    next = next_clip->get_frame(next_clip, n+1); // get next frame
  else if (vi->num_frames > 1)
    next = next_clip->get_frame(next_clip, vi->num_frames - 2); // prev frame
  else
    next = next_clip->get_frame(next_clip, 0); // cur 0 frame for one-frame clip

    dst = f->env->avs_se_vf_new_a (f->env, vi, AVS_FRAME_ALIGN);
        /* FIXME: this doesn't take into account the fact that num_frames or
         * fps may change, it just copies timestamp from the source clip.
         * Until this is fixed, GStreamer may play the resulting video with
         * wrong speed.
         */
        f->env->avs_vf_set_timestamp(dst, f->env->avs_vf_get_timestamp (src));
   // Construct a frame based on the information of the current frame
   // contained in the "vi" struct.

  if (f->order == -1)
    tff = f->env->avs_vf_get_parity(src) & AVS_IT_TFF ? 1 : 0; // 0 or 1
  else
    tff = f->order;

  parity = (mode & 1) ? (ndst & 1) ^ (1^tff) : (tff ^ 1);  // 0 or 1


    if (AVS_IS_PLANAR(vi))
    {
  for (iplane = 0; iplane<3; iplane++)
  {
    int plane = (iplane==0) ? AVS_PLANAR_Y : (iplane==1) ? AVS_PLANAR_U : AVS_PLANAR_V;

    const unsigned char* srcp = e->avs_vf_get_read_ptr_p(src, plane);
     // Request a Read pointer from the current source frame

    const unsigned char* prevp0 = e->avs_vf_get_read_ptr_p(prev, plane);
     unsigned char* prevp = (unsigned char*) prevp0; // with same pitch
    // Request a Read pointer from the prev source frame.

      const unsigned char* nextp0 = e->avs_vf_get_read_ptr_p(next, plane);
     unsigned char* nextp = (unsigned char*) nextp0; // with same pitch
    // Request a Read pointer from the next source frame.

    unsigned char* dstp = e->avs_vf_get_write_ptr_p(dst, plane);
    // Request a Write pointer from the newly created destination image.
    // You can request a writepointer to images that have just been

    const int dst_pitch = e->avs_vf_get_pitch_p(dst, plane);
    // Requests pitch (length of a line) of the destination image.
    // For more information on pitch see: http://www.avisynth.org/index.php?page=WorkingWithImages
    // (short version - pitch is always equal to or greater than width to allow for seriously fast assembly code)

    const int width = e->avs_vf_get_row_size_p(dst,plane);
    // Requests rowsize (number of used bytes in a line.
    // See the link above for more information.

    const int height = e->avs_vf_get_height_p(dst,plane);
    // Requests the height of the destination image.

    const int src_pitch = e->avs_vf_get_pitch_p(src,plane);
    const int prev_pitch = e->avs_vf_get_pitch_p(prev,plane);
    const int next_pitch = e->avs_vf_get_pitch_p(next,plane);

    // in v.0.1-0.3  all source pitches are  assumed equal (for simplicity)
                                // consider other (rare) case
    if (prev_pitch != src_pitch)
    {
        prevp = (unsigned char *)malloc(height*src_pitch);
        for (h=0; h<height; h++)
           memcpy(prevp+h*src_pitch, prevp0+h*prev_pitch, width);
    }

    if (next_pitch != src_pitch)
    {
        nextp = (unsigned char *)malloc(height*src_pitch);
        for (h=0; h<height; h++)
           memcpy(nextp+h*src_pitch, nextp0+h*next_pitch, width);
    }

    filter_plane(mode, dstp, dst_pitch, prevp, srcp, nextp, src_pitch, width, height, parity, tff, f->cpu);
    if (prev_pitch != src_pitch)
      free(prevp);
    if (next_pitch != src_pitch)
      free(nextp);
  }

   }
   else if (AVS_IS_YUY2(vi) && f->planar_hack==0)
   {
    const unsigned char* srcp = e->avs_vf_get_read_ptr(src);
     // Request a Read pointer from the current source frame

    const unsigned char* prevp = e->avs_vf_get_read_ptr(prev);
    // Request a Read pointer from the prev source frame.

      const unsigned char* nextp = e->avs_vf_get_read_ptr(next);
    // Request a Read pointer from the next source frame.

    unsigned char* dstp = e->avs_vf_get_write_ptr(dst);
    // Request a Write pointer from the newly created destination image.
    // You can request a writepointer to images that have just been

    const int dst_pitch = e->avs_vf_get_pitch(dst);
    // Requests pitch (length of a line) of the destination image.
    // For more information on pitch see: http://www.avisynth.org/index.php?page=WorkingWithImages
    // (short version - pitch is always equal to or greater than width to allow for seriously fast assembly code)

    const int width = e->avs_vf_get_row_size(dst)/2;
    // Requests rowsize (number of used bytes in a line.
    // See the link above for more information.

    const int height = e->avs_vf_get_height(dst);
    // Requests the height of the destination image.

    const int src_pitch = e->avs_vf_get_pitch(src);
    const int prev_pitch = e->avs_vf_get_pitch(prev);
    const int next_pitch = e->avs_vf_get_pitch(next);

        YUY2ToPlanes(srcp, src_pitch, width, height, f->ysrc, f->ypitch, f->usrc, f->vsrc, f->uvpitch, f->cpu);
        YUY2ToPlanes(prevp, prev_pitch, width, height, f->yprev, f->ypitch, f->uprev, f->vprev, f->uvpitch, f->cpu);
        YUY2ToPlanes(nextp, next_pitch, width, height, f->ynext, f->ypitch, f->unext, f->vnext, f->uvpitch, f->cpu);

  filter_plane(mode, f->ydest, f->ypitch, f->yprev, f->ysrc, f->ynext, f->ypitch, width, height, parity, tff, f->cpu);
  filter_plane(mode, f->udest, f->uvpitch, f->uprev, f->usrc, f->unext, f->uvpitch, width/2, height, parity, tff, f->cpu);
  filter_plane(mode, f->vdest, f->uvpitch, f->vprev, f->vsrc, f->vnext, f->uvpitch, width/2, height, parity, tff, f->cpu);

        YUY2FromPlanes(dstp, dst_pitch, width, height, f->ydest, f->ypitch,  f->udest, f->vdest, f->uvpitch, f->cpu);

   }
   else if (AVS_IS_YUY2(vi) && f->planar_hack)
   {
    const unsigned char* srcp = e->avs_vf_get_read_ptr(src);

    const unsigned char* prevp0 = e->avs_vf_get_read_ptr(prev);
     unsigned char* prevp = (unsigned char*) prevp0; // with same pitch

      const unsigned char* nextp0 = e->avs_vf_get_read_ptr(next);
     unsigned char* nextp = (unsigned char*) nextp0; // with same pitch

    unsigned char* dstp = e->avs_vf_get_write_ptr(dst);
    const int dst_pitch = e->avs_vf_get_pitch(dst);

    const int width = e->avs_vf_get_row_size(dst)/2;

    const int height = e->avs_vf_get_height(dst);

    const int src_pitch = e->avs_vf_get_pitch(src);
    const int prev_pitch = e->avs_vf_get_pitch(prev);
    const int next_pitch = e->avs_vf_get_pitch(next);

    if (prev_pitch != src_pitch)
    {
        prevp = (unsigned char *)malloc(height*src_pitch);
        for (h=0; h<height; h++)
           memcpy(prevp+h*src_pitch, prevp0+h*prev_pitch, width*2);
    }

    if (next_pitch != src_pitch)
    {
        nextp = (unsigned char *)malloc(height*src_pitch);
        for (h=0; h<height; h++)
           memcpy(nextp+h*src_pitch, nextp0+h*next_pitch, width*2);
    }
        // Y plane
    filter_plane(mode, dstp, dst_pitch, prevp, srcp, nextp, src_pitch, width, height, parity, tff, f->cpu);
    // U plane
    filter_plane(mode, dstp+width, dst_pitch, prevp+width, srcp+width, nextp+width, src_pitch, width/2, height, parity, tff, f->cpu);
    // V plane
    filter_plane(mode, dstp+width+width/2, dst_pitch, prevp+width+width/2, srcp+width+width/2, nextp+width+width/2, src_pitch, width/2, height, parity, tff, f->cpu);

    if (prev_pitch != src_pitch)
      free(prevp);
    if (next_pitch != src_pitch)
      free(nextp);
   }


  // As we now are finished processing the image, we return the destination image.
  f->env->avs_vf_release (src);
  f->env->avs_vf_release (next);
  f->env->avs_vf_release (prev);
  /* dst is reffed once, and that is how the caller will get it.
   * If you want to keep using it, you should ref it here
   */
  return dst;
}

// The following function is the function that actually registers the filter in AviSynth
// It is called automatically, when the plugin is loaded to see which functions this filter contains.

const char * AVSC_CC avisynth_c_plugin_init(AVS_ScriptEnvironment * env)
{
  env->avs_se_add_function(env, "Yadif", "c[mode]i[order]i[planar]b[opt]i",
  "video/x-raw-yuv, format=(fourcc)YV12; video/x-raw-yuv, format=(fourcc)YUY2",
  "video/x-raw-yuv, format=(fourcc)YV12; video/x-raw-yuv, format=(fourcc)YUY2",
  yadif_apply, NULL);
  return "Yadif deinterlace";
}
