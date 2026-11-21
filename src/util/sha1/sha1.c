/*	$OpenBSD: sha1.c,v 1.26 2015/09/11 09:18:27 guenther Exp $	*/

/*
 * SHA-1 in C
 * By Steve Reid <steve@edmweb.com>
 * 100% Public Domain
 *
 * Test Vectors (from FIPS PUB 180-1)
 * "abc"
 *   A9993E36 4706816A BA3E2571 7850C26C 9CD0D89D
 * "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
 *   84983E44 1C3BD26E BAAE4AA1 F95129E5 E54670F1
 * A million repetitions of "a"
 *   34AA973C D4C4DAA4 F61EEB2B DBAD2731 6534016F
 */

#include <stdint.h>
#include <string.h>
#include "sha1.h"
#if defined(__aarch64__) || defined(__arm64ec__)
#include <arm_neon.h>
#ifdef _WIN32
#include <windows.h>
#endif
#endif

#define rol(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

/*
 * blk0() and blk() perform the initial expand.
 * I got the idea of expanding during the round function from SSLeay
 */
# define blk0(i) (block->l[i] = (rol(block->l[i],24)&0xFF00FF00) \
    |(rol(block->l[i],8)&0x00FF00FF))
    
#define blk(i) (block->l[i&15] = rol(block->l[(i+13)&15]^block->l[(i+8)&15] \
    ^block->l[(i+2)&15]^block->l[i&15],1))

/*
 * (R0+R1), R2, R3, R4 are the different operations (rounds) used in SHA1
 */
#define R0(v,w,x,y,z,i) z+=((w&(x^y))^y)+blk0(i)+0x5A827999+rol(v,5);w=rol(w,30);
#define R1(v,w,x,y,z,i) z+=((w&(x^y))^y)+blk(i)+0x5A827999+rol(v,5);w=rol(w,30);
#define R2(v,w,x,y,z,i) z+=(w^x^y)+blk(i)+0x6ED9EBA1+rol(v,5);w=rol(w,30);
#define R3(v,w,x,y,z,i) z+=(((w|x)&y)|(w&x))+blk(i)+0x8F1BBCDC+rol(v,5);w=rol(w,30);
#define R4(v,w,x,y,z,i) z+=(w^x^y)+blk(i)+0xCA62C1D6+rol(v,5);w=rol(w,30);

typedef union {
	uint8_t c[64];
	uint32_t l[16];
} CHAR64LONG16;

/*
 * Hash a single 512-bit block. This is the core of the algorithm.
 */
void
SHA1Transform(uint32_t state[5], const uint8_t* buffer)
{
	uint32_t a, b, c, d, e;
	uint8_t workspace[SHA1_BLOCK_LENGTH];
	CHAR64LONG16 *block = (CHAR64LONG16 *)workspace;

	(void)memcpy(block, buffer, SHA1_BLOCK_LENGTH);

	/* Copy context->state[] to working vars */
	a = state[0];
	b = state[1];
	c = state[2];
	d = state[3];
	e = state[4];

	/* 4 rounds of 20 operations each. Loop unrolled. */
	R0(a,b,c,d,e, 0); R0(e,a,b,c,d, 1); R0(d,e,a,b,c, 2); R0(c,d,e,a,b, 3);
	R0(b,c,d,e,a, 4); R0(a,b,c,d,e, 5); R0(e,a,b,c,d, 6); R0(d,e,a,b,c, 7);
	R0(c,d,e,a,b, 8); R0(b,c,d,e,a, 9); R0(a,b,c,d,e,10); R0(e,a,b,c,d,11);
	R0(d,e,a,b,c,12); R0(c,d,e,a,b,13); R0(b,c,d,e,a,14); R0(a,b,c,d,e,15);
	R1(e,a,b,c,d,16); R1(d,e,a,b,c,17); R1(c,d,e,a,b,18); R1(b,c,d,e,a,19);
	R2(a,b,c,d,e,20); R2(e,a,b,c,d,21); R2(d,e,a,b,c,22); R2(c,d,e,a,b,23);
	R2(b,c,d,e,a,24); R2(a,b,c,d,e,25); R2(e,a,b,c,d,26); R2(d,e,a,b,c,27);
	R2(c,d,e,a,b,28); R2(b,c,d,e,a,29); R2(a,b,c,d,e,30); R2(e,a,b,c,d,31);
	R2(d,e,a,b,c,32); R2(c,d,e,a,b,33); R2(b,c,d,e,a,34); R2(a,b,c,d,e,35);
	R2(e,a,b,c,d,36); R2(d,e,a,b,c,37); R2(c,d,e,a,b,38); R2(b,c,d,e,a,39);
	R3(a,b,c,d,e,40); R3(e,a,b,c,d,41); R3(d,e,a,b,c,42); R3(c,d,e,a,b,43);
	R3(b,c,d,e,a,44); R3(a,b,c,d,e,45); R3(e,a,b,c,d,46); R3(d,e,a,b,c,47);
	R3(c,d,e,a,b,48); R3(b,c,d,e,a,49); R3(a,b,c,d,e,50); R3(e,a,b,c,d,51);
	R3(d,e,a,b,c,52); R3(c,d,e,a,b,53); R3(b,c,d,e,a,54); R3(a,b,c,d,e,55);
	R3(e,a,b,c,d,56); R3(d,e,a,b,c,57); R3(c,d,e,a,b,58); R3(b,c,d,e,a,59);
	R4(a,b,c,d,e,60); R4(e,a,b,c,d,61); R4(d,e,a,b,c,62); R4(c,d,e,a,b,63);
	R4(b,c,d,e,a,64); R4(a,b,c,d,e,65); R4(e,a,b,c,d,66); R4(d,e,a,b,c,67);
	R4(c,d,e,a,b,68); R4(b,c,d,e,a,69); R4(a,b,c,d,e,70); R4(e,a,b,c,d,71);
	R4(d,e,a,b,c,72); R4(c,d,e,a,b,73); R4(b,c,d,e,a,74); R4(a,b,c,d,e,75);
	R4(e,a,b,c,d,76); R4(d,e,a,b,c,77); R4(c,d,e,a,b,78); R4(b,c,d,e,a,79);

	/* Add the working vars back into context.state[] */
	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;

	/* Wipe variables */
	a = b = c = d = e = 0;
}


/*
 * SHA1Init - Initialize new context
 */
void
SHA1Init(SHA1_CTX *context)
{

	/* SHA1 initialization constants */
	context->count = 0;
	context->state[0] = 0x67452301;
	context->state[1] = 0xEFCDAB89;
	context->state[2] = 0x98BADCFE;
	context->state[3] = 0x10325476;
	context->state[4] = 0xC3D2E1F0;
}


/*
 * Run your data through this.
 */
#if defined(__aarch64__) || defined(__arm64ec__)
#ifdef _WIN32
/* Windows says so for the ARMv8 crypto instructions, SHA-1's among them (Wine from FEAT_AES) */
#define sha1_hardware() IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE)
#else
/* every arm64 Mac has them */
#define sha1_hardware() 1
#endif

/*
 * SHA1Transform for `blocks` 64-byte blocks in a row, on the ARMv8 SHA-1 instructions
 */
__attribute__((target("sha2"))) static void
SHA1TransformBlocksNeon(uint32_t state[5], const uint8_t *data, size_t blocks)
{
	uint32x4_t abcd = vld1q_u32(state);
	uint32_t e0 = state[4];
	const uint32x4_t k0 = vdupq_n_u32(0x5A827999), k1 = vdupq_n_u32(0x6ED9EBA1),
	                 k2 = vdupq_n_u32(0x8F1BBCDC), k3 = vdupq_n_u32(0xCA62C1D6);
	while (blocks--) {
		uint32x4_t abcd_saved = abcd;
		uint32_t e_saved = e0, e1;
		uint32x4_t m0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data)));
		uint32x4_t m1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 16)));
		uint32x4_t m2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 32)));
		uint32x4_t m3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 48)));
		uint32x4_t t0 = vaddq_u32(m0, k0), t1 = vaddq_u32(m1, k0);
		/* rounds 0-3 */
		e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1cq_u32(abcd, e0, t0); t0 = vaddq_u32(m2, k0); m0 = vsha1su0q_u32(m0, m1, m2);
		/* 4-7 */
		e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1cq_u32(abcd, e1, t1); t1 = vaddq_u32(m3, k0); m0 = vsha1su1q_u32(m0, m3); m1 = vsha1su0q_u32(m1, m2, m3);
		/* 8-11 */
		e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1cq_u32(abcd, e0, t0); t0 = vaddq_u32(m0, k0); m1 = vsha1su1q_u32(m1, m0); m2 = vsha1su0q_u32(m2, m3, m0);
		/* 12-15 */
		e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1cq_u32(abcd, e1, t1); t1 = vaddq_u32(m1, k1); m2 = vsha1su1q_u32(m2, m1); m3 = vsha1su0q_u32(m3, m0, m1);
		/* 16-19 */
		e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1cq_u32(abcd, e0, t0); t0 = vaddq_u32(m2, k1); m3 = vsha1su1q_u32(m3, m2); m0 = vsha1su0q_u32(m0, m1, m2);
		/* 20-23 */
		e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1pq_u32(abcd, e1, t1); t1 = vaddq_u32(m3, k1); m0 = vsha1su1q_u32(m0, m3); m1 = vsha1su0q_u32(m1, m2, m3);
		/* 24-27 */
		e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1pq_u32(abcd, e0, t0); t0 = vaddq_u32(m0, k1); m1 = vsha1su1q_u32(m1, m0); m2 = vsha1su0q_u32(m2, m3, m0);
		/* 28-31 */
		e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1pq_u32(abcd, e1, t1); t1 = vaddq_u32(m1, k1); m2 = vsha1su1q_u32(m2, m1); m3 = vsha1su0q_u32(m3, m0, m1);
		/* 32-35 */
		e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1pq_u32(abcd, e0, t0); t0 = vaddq_u32(m2, k2); m3 = vsha1su1q_u32(m3, m2); m0 = vsha1su0q_u32(m0, m1, m2);
		/* 36-39 */
		e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1pq_u32(abcd, e1, t1); t1 = vaddq_u32(m3, k2); m0 = vsha1su1q_u32(m0, m3); m1 = vsha1su0q_u32(m1, m2, m3);
		/* 40-43 */
		e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1mq_u32(abcd, e0, t0); t0 = vaddq_u32(m0, k2); m1 = vsha1su1q_u32(m1, m0); m2 = vsha1su0q_u32(m2, m3, m0);
		/* 44-47 */
		e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1mq_u32(abcd, e1, t1); t1 = vaddq_u32(m1, k2); m2 = vsha1su1q_u32(m2, m1); m3 = vsha1su0q_u32(m3, m0, m1);
		/* 48-51 */
		e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1mq_u32(abcd, e0, t0); t0 = vaddq_u32(m2, k2); m3 = vsha1su1q_u32(m3, m2); m0 = vsha1su0q_u32(m0, m1, m2);
		/* 52-55 */
		e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1mq_u32(abcd, e1, t1); t1 = vaddq_u32(m3, k3); m0 = vsha1su1q_u32(m0, m3); m1 = vsha1su0q_u32(m1, m2, m3);
		/* 56-59 */
		e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1mq_u32(abcd, e0, t0); t0 = vaddq_u32(m0, k3); m1 = vsha1su1q_u32(m1, m0); m2 = vsha1su0q_u32(m2, m3, m0);
		/* 60-63 */
		e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1pq_u32(abcd, e1, t1); t1 = vaddq_u32(m1, k3); m2 = vsha1su1q_u32(m2, m1); m3 = vsha1su0q_u32(m3, m0, m1);
		/* 64-67 */
		e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1pq_u32(abcd, e0, t0); t0 = vaddq_u32(m2, k3); m3 = vsha1su1q_u32(m3, m2);
		/* 68-71 */
		e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1pq_u32(abcd, e1, t1); t1 = vaddq_u32(m3, k3);
		/* 72-75 */
		e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1pq_u32(abcd, e0, t0);
		/* 76-79 */
		e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0)); abcd = vsha1pq_u32(abcd, e1, t1);
		e0 += e_saved;
		abcd = vaddq_u32(abcd_saved, abcd);
		data += 64;
	}
	vst1q_u32(state, abcd);
	state[4] = e0;
}
#endif

void
SHA1Update(SHA1_CTX *context, const uint8_t *data, size_t len)
{
	size_t i, j;

	j = (size_t)((context->count >> 3) & 63);
	context->count += (len << 3);
	if ((j + len) > 63) {
		(void)memcpy(&context->buffer[j], data, (i = 64-j));
		SHA1Transform(context->state, context->buffer);
#if defined(__aarch64__) || defined(__arm64ec__)
		if (sha1_hardware() && i + 63 < len) {
			size_t blocks = (len - i) / 64;
			SHA1TransformBlocksNeon(context->state, &data[i], blocks);
			i += blocks * 64;
		}
#endif
		for ( ; i + 63 < len; i += 64)
			SHA1Transform(context->state, (uint8_t *)&data[i]);
		j = 0;
	} else {
		i = 0;
	}
	(void)memcpy(&context->buffer[j], &data[i], len - i);
}


/*
 * Add padding and return the message digest.
 */
void
SHA1Pad(SHA1_CTX *context)
{
	uint8_t finalcount[8];
	uint32_t i;

	for (i = 0; i < 8; i++) {
		finalcount[i] = (uint8_t)((context->count >>
		    ((7 - (i & 7)) * 8)) & 255);	/* Endian independent */
	}
	SHA1Update(context, (uint8_t *)"\200", 1);
	while ((context->count & 504) != 448)
		SHA1Update(context, (uint8_t *)"\0", 1);
	SHA1Update(context, finalcount, 8); /* Should cause a SHA1Transform() */
}

void
SHA1Final(uint8_t digest[SHA1_DIGEST_LENGTH], SHA1_CTX *context)
{
	uint32_t i;

	SHA1Pad(context);
	for (i = 0; i < SHA1_DIGEST_LENGTH; i++) {
		digest[i] = (uint8_t)
		   ((context->state[i>>2] >> ((3-(i & 3)) * 8) ) & 255);
	}
	memset(context, 0, sizeof(*context));
}
