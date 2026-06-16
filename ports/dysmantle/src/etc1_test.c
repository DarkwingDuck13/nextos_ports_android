/* etc1_test.c -- valida o encoder ETC1 no host: encode -> decode INDEPENDENTE ->
 * PSNR. Decoder escrito direto do spec (codigo separado do encoder p/ pegar bugs).
 * build: gcc -O2 -o /tmp/etc1test src/etc1_test.c src/etc1_encode.c -lm */
#include "etc1_encode.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

static const int kMod[8][4] = {
  {  2,   8,  -2,   -8}, {  5,  17,  -5,  -17}, {  9,  29,  -9,  -29}, { 13,  42, -13,  -42},
  { 18,  60, -18,  -60}, { 24,  80, -24,  -80}, { 33, 106, -33, -106}, { 47, 183, -47, -183}
};
static int clamp8(int v){ return v<0?0:(v>255?255:v); }
static int exp4(int v){ return (v<<4)|v; }
static int exp5(int v){ return (v<<3)|(v>>2); }

static void dec_block(const unsigned char b[8], unsigned char out[16*4]) {
  int diff=(b[3]>>1)&1, flip=b[3]&1, t1=(b[3]>>5)&7, t2=(b[3]>>2)&7;
  int base1[3], base2[3];
  if(!diff){
    base1[0]=exp4((b[0]>>4)&0xF); base2[0]=exp4(b[0]&0xF);
    base1[1]=exp4((b[1]>>4)&0xF); base2[1]=exp4(b[1]&0xF);
    base1[2]=exp4((b[2]>>4)&0xF); base2[2]=exp4(b[2]&0xF);
  } else {
    int r5=(b[0]>>3)&0x1F, dr=b[0]&0x7; if(dr&4)dr-=8;
    int g5=(b[1]>>3)&0x1F, dg=b[1]&0x7; if(dg&4)dg-=8;
    int b5=(b[2]>>3)&0x1F, db=b[2]&0x7; if(db&4)db-=8;
    base1[0]=exp5(r5); base2[0]=exp5(r5+dr);
    base1[1]=exp5(g5); base2[1]=exp5(g5+dg);
    base1[2]=exp5(b5); base2[2]=exp5(b5+db);
  }
  unsigned msb=(b[4]<<8)|b[5], lsb=(b[6]<<8)|b[7];
  for(int x=0;x<4;x++)for(int y=0;y<4;y++){
    int p=x*4+y, sel=(((msb>>p)&1)<<1)|((lsb>>p)&1);
    int first=flip?(y<2):(x<2);
    int *base=first?base1:base2; int t=first?t1:t2, m=kMod[t][sel];
    unsigned char *o=out+(y*4+x)*4;
    o[0]=clamp8(base[0]+m); o[1]=clamp8(base[1]+m); o[2]=clamp8(base[2]+m); o[3]=255;
  }
}

static double psnr_img(const unsigned char *a, const unsigned char *b, int n){
  double mse=0; for(int i=0;i<n;i++){ for(int c=0;c<3;c++){ int d=a[i*4+c]-b[i*4+c]; mse+=(double)d*d; } }
  mse/=(n*3); if(mse<1e-9) return 99.0; return 10.0*log10(255.0*255.0/mse);
}

int main(void){
  int W=128,H=128,N=W*H;
  unsigned char *img=malloc(N*4), *dec=malloc(N*4);
  unsigned char *etc=malloc((W/4)*(H/4)*8);

  /* === teste 1: gradiente liso === */
  for(int y=0;y<H;y++)for(int x=0;x<W;x++){ int i=(y*W+x)*4;
    img[i]=x*2; img[i+1]=y*2; img[i+2]=(x+y); img[i+3]=255; }
  etc1_encode_image(img,W,H,4,etc);
  for(int by=0;by<H/4;by++)for(int bx=0;bx<W/4;bx++){
    unsigned char blk[16*4]; dec_block(etc+((by*(W/4)+bx)*8), blk);
    for(int y=0;y<4;y++)for(int x=0;x<4;x++){ int di=((by*4+y)*W+(bx*4+x))*4, si=(y*4+x)*4;
      dec[di]=blk[si]; dec[di+1]=blk[si+1]; dec[di+2]=blk[si+2]; dec[di+3]=255; } }
  printf("gradiente:   PSNR = %.2f dB\n", psnr_img(img,dec,N));

  /* === teste 2: cores solidas + bordas (alto contraste) === */
  for(int y=0;y<H;y++)for(int x=0;x<W;x++){ int i=(y*W+x)*4;
    int c=((x/16)+(y/16))&1; img[i]=c?230:20; img[i+1]=((x/8)&1)?200:40; img[i+2]=c?30:210; img[i+3]=255; }
  etc1_encode_image(img,W,H,4,etc);
  for(int by=0;by<H/4;by++)for(int bx=0;bx<W/4;bx++){
    unsigned char blk[16*4]; dec_block(etc+((by*(W/4)+bx)*8), blk);
    for(int y=0;y<4;y++)for(int x=0;x<4;x++){ int di=((by*4+y)*W+(bx*4+x))*4, si=(y*4+x)*4;
      dec[di]=blk[si]; dec[di+1]=blk[si+1]; dec[di+2]=blk[si+2]; } }
  printf("xadrez/bordas: PSNR = %.2f dB\n", psnr_img(img,dec,N));

  /* === teste 3: ruido pseudo-aleatorio (pior caso) === */
  unsigned s=12345; for(int i=0;i<N;i++){ for(int c=0;c<3;c++){ s=s*1103515245+12345; img[i*4+c]=(s>>16)&0xFF; } img[i*4+3]=255; }
  etc1_encode_image(img,W,H,4,etc);
  for(int by=0;by<H/4;by++)for(int bx=0;bx<W/4;bx++){
    unsigned char blk[16*4]; dec_block(etc+((by*(W/4)+bx)*8), blk);
    for(int y=0;y<4;y++)for(int x=0;x<4;x++){ int di=((by*4+y)*W+(bx*4+x))*4, si=(y*4+x)*4;
      dec[di]=blk[si]; dec[di+1]=blk[si+1]; dec[di+2]=blk[si+2]; } }
  printf("ruido:       PSNR = %.2f dB\n", psnr_img(img,dec,N));

  printf("\n(ETC1 bom: gradiente >40, xadrez >30, ruido ~22-26. Se gradiente<30 = BUG)\n");
  free(img); free(dec); free(etc); return 0;
}
