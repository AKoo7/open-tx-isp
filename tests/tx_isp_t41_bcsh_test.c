#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../driver/t41/tx_isp_t41_bcsh.h"
static void put16(unsigned char *p, unsigned int v) { p[0]=v; p[1]=v>>8; }
static void put32(unsigned char *p, unsigned int v) { put16(p,v); put16(p+2,v>>16); }
int main(void)
{
	unsigned char storage[361]={0}, *p=storage+1, csc[92]={0};
	unsigned int i,j,words[29],saved[29];
	static const unsigned int offsets[]={0x42,0x54,0x6c,0x7e,0x90,0xa2,0xbe,0xd0,0xe2,0xf4,0x106};
	for(i=0;i<9;++i) put32(p+i*4,10+i*100);
	for(i=0;i<6;++i) put16(p+0x24+i*2,1800+i*1000);
	for(i=0;i<11;++i) for(j=0;j<9;++j) put16(p+offsets[i]+j*2,0);
	for(i=0;i<3;++i) {
		put16(p+0x118+i*2,1024);
		put32(csc+4+i*16,65536); put32(csc+48+i*16,65536);
	}
	csc[43]=csc[45]=255;
	assert(!t41_bcsh_compute(p,360,5000,200,csc,92,words));
	assert(words[9]==1024 && words[10]==0 && words[11]==(1024U<<16));
	assert(words[12]==0 && words[13]==0 && words[14]==1024);
	assert(words[0]==1023 && words[6]==1024);
	assert(words[21]==0 && words[22]==1024);
	assert(words[23]==(1023U<<16) && words[24]==(1023U<<16));
	assert(words[27]==0x04000400 && words[28]==0x04000400);
	memcpy(saved,words,sizeof(words));
	put32(p+4,10);
	assert(t41_bcsh_compute(p,360,5000,200,csc,92,words));
	assert(!memcmp(saved,words,sizeof(words)));
	put32(p+4,110); p[0x166]=2;
	assert(t41_bcsh_compute(p,360,5000,200,csc,92,words));
	p[0x166]=0; put32(csc+4,262145);
	assert(t41_bcsh_compute(p,360,5000,200,csc,92,words));
	assert(t41_bcsh_compute(p,359,5000,200,csc,92,words));
	assert(t41_bcsh_compute(p,360,5000,200,csc,91,words));
	assert(!memcmp(saved,words,sizeof(words)));
	assert(t41_bcsh_round(-32,6)==-1 && t41_bcsh_round(32,6)==1);
	/* Non-neutral API controls (stock BCS_adjust / H_adjust). */
	{
		unsigned char ctl[4] = {128,128,128,128};
		unsigned int api[29], base[29];
		p[0x166]=0; put32(csc+4,65536); put32(p+4,110);
		for(i=0;i<9;++i) put16(p + 0x106 + i*2,40);	/* brightness base */
		for(i=0;i<9;++i) put16(p + 0x6c + i*2,100);	/* chroma knees v2..v5 */
		for(i=0;i<9;++i) put16(p + 0x7e + i*2,200);
		for(i=0;i<9;++i) put16(p + 0x90 + i*2,100);
		for(i=0;i<9;++i) put16(p + 0xa2 + i*2,200);
		put16(p+0x68,12); put16(p+0x6a,48);
		assert(!t41_bcsh_compute(p,360,5000,200,csc,92,base));
		assert(!t41_bcsh_compute_api(p,360,5000,200,csc,92,ctl,api));
		assert(!memcmp(api,base,sizeof(base)));
		ctl[0]=255;	/* brightness: Y offset += v10*(255-128)/128 */
		assert(!t41_bcsh_compute_api(p,360,5000,200,csc,92,ctl,api));
		assert((api[6]>>16) == (base[6]>>16) + ((40*255+64)>>7) - 40);
		ctl[0]=128; ctl[3]=192;	/* hue +90 degrees: rows 1/2 rotate */
		assert(!t41_bcsh_compute_api(p,360,5000,200,csc,92,ctl,api));
		assert(api[11]==0 && api[12]==1024);
		assert(api[13]==(((unsigned)-1024&16383U)<<16) && api[14]==0);
		ctl[3]=128; ctl[2]=255;	/* saturation max: knees reach 6144 */
		assert(!t41_bcsh_compute_api(p,360,5000,200,csc,92,ctl,api));
		assert(api[27]==((6144U<<16)|6144U) && api[28]==((6144U<<16)|6144U));
		ctl[2]=0;
		assert(!t41_bcsh_compute_api(p,360,5000,200,csc,92,ctl,api));
		assert(api[27]==0 && api[28]==0);
		ctl[2]=128; ctl[1]=0;	/* contrast 0 also fades saturation */
		assert(!t41_bcsh_compute_api(p,360,5000,200,csc,92,ctl,api));
		assert(api[27]==0 && memcmp(api,base,sizeof(base)));
	}
	puts("T41 neutral-API BCSH matrix conversion, fallback and atomic rejection: passed");
	return 0;
}
