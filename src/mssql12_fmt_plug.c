/* Modified in August, 2012 by Dhiru Kholia (dhiru at openwall.com) for MS SQL 2012
 *
 * This software is Copyright (c) 2010 bartavelle, <bartavelle at bandecon.com>,
 * and it is hereby released to the general public under the following terms:
 * Redistribution and use in source and binary forms, with or without modification, are permitted.
 *
 * Modified by Mathieu Perrin (mathieu at tpfh.org) 09/06
 * Microsoft MS-SQL05 password cracker
 *
 * UTF-8 support by magnum 2011, same terms as above
 *
 * Creating MS SQL 2012 hashes:
 *
 * sqlcmd -L
 * sqlcmd -S <server> -U sa -P <password>
 * 1> select pwdencrypt("openwall")
 * 2> go
 *
 * Dumping hashes from MS SQL server 2012:
 *
 * sqlcmd -S <server> -U sa -P <password>
 * 1> select * from sys.sql_logins
 * 2> go
 */

#if FMT_EXTERNS_H
extern struct fmt_main fmt_mssql12;
#elif FMT_REGISTERS_H
john_register_one(&fmt_mssql12);
#else

#include <string.h>

#include "arch.h"

//#undef _OPENMP
//#undef SIMD_COEF_32
//#undef SIMD_COEF_64

#include "misc.h"
#include "params.h"
#include "common.h"
#include "formats.h"
#include "options.h"
#include "unicode.h"
#include "sha2.h"
#include "johnswap.h"
#include "sse-intrinsics.h"
#include "memdbg.h"
#ifdef _OPENMP
static int omp_t = 1;
#include <omp.h>
#ifdef SIMD_COEF_64
#define OMP_SCALE               2048
#else
#define OMP_SCALE               1024  // tuned K8-dual HT
#endif
#endif

#define FORMAT_LABEL            "mssql12"
#define FORMAT_NAME             "MS SQL 2012/2014"
#define ALGORITHM_NAME          "SHA512 " SHA512_ALGORITHM_NAME

#define BENCHMARK_COMMENT       ""
#define BENCHMARK_LENGTH        0

#define PLAINTEXT_LENGTH        ((111 - SALT_SIZE) / 2)
#define CIPHERTEXT_LENGTH       54 + 44 * 2

#define BINARY_SIZE             64
#define BINARY_ALIGN            8
#define SALT_SIZE               4
#define SALT_ALIGN              4

#ifdef SIMD_COEF_64
#define MIN_KEYS_PER_CRYPT      SIMD_COEF_64
#define MAX_KEYS_PER_CRYPT      SIMD_COEF_64
#else
#define MIN_KEYS_PER_CRYPT      1
#define MAX_KEYS_PER_CRYPT      1
#endif

#undef MIN
#define MIN(a, b)               (((a) > (b)) ? (b) : (a))

static struct fmt_tests tests[] = {
	{"0x0200F733058A07892C5CACE899768F89965F6BD1DED7955FE89E1C9A10E27849B0B213B5CE92CC9347ECCB34C3EFADAF2FD99BFFECD8D9150DD6AACB5D409A9D2652A4E0AF16", "Password1!"},
	{"0x0200AB3E1F9028A739EEF62ABF672427276A32D5EDD349E638E7F2CD81DAA247CFE20EE4E3B0A30B2D0AE3C3FA010E61752F1BF45E045041F1B988C083C7F118527E3E5F0562", "openwall"},
	/* hashes from https://hashcat.net/forum */
	{"0x02006BF4AB05873FF0C8A4AFD1DC5912CBFDEF62E0520A3353B04E1184F05C873C9C76BBADDEAAC1E9948C7B6ABFFD62BFEFD7139F17F6AFE10BE0FEE7A178644623067C2423", "carlos"},
	{"0x0200935819BA20F1C7289CFF2F8FF9F0E40DA5E6D04986F988CFE6603DA0D2BC0160776614763198967D603FBD8C103151A15E70D18E7B494C7F13F16804A7A4EB206084E632", "test"},
	{"0x0200570AC969EF7C6CCB3312E8BEDE1D635EB852C06496957F0FA845B20FCD1C7C457474A5B948B68C47C2CB704D08978871F532C9EB11199BB5F56A06AC915C3799DB8A64C1", "test1"},
	{"0x0200A56045DBCD848E297FA8D06E7579D62B7129928CA0BC5D232A7320972EF5A5455C01411B8D3A7FF3D18A55058A12FAEE5DA410AFE6CE61FF5C39E5FF57CD3EDD57DB1C3B", "test2"},
	{"0x020059799F1B6D897BE2C5A76D3FFDC52B308190E82FA01F2FA51129B4863A7EE21B3FF6FE9F7850976045237805F338DD36DC9345B429F47A402614C6F2F2B02C56DF14C4F4", "Paul"},
	{"0x0200881E2999DD8E3583695F405696257B99559953705A34D774C15AC1D42699BB77BC56DB5F657751335C1B350890E643790553B60329CAE7A2E7D3C04CF8856C4DB0058723", "DBAmaster"},
	{"0x0200D648446E70180A6DFB6DF14DB38623EBFE490FE445751900FD5DC45A2B5D20D7AFFE8C6FFC2890BAE1AF34430A21F2F1E4DE50E25757FDB4789716D8D85C6985A00BC454", "database"},
	{"0x02008AC3B9DC7B67EF9D3C1D25D8007A4B957D5BD61D71E5E9DA08D9F8F012EDDAD168E1CADD93D4627433FBFEE8BCF6CBB42D5B9A31886FC5FF7F970B164F4B5815E03D6DE7", "jhl9mqe5"},
	{"0x020094C4D05A082DB1362B1A972C5D5F1C04C527090A7427E93C13AFEC705A011D8980E994FA647C7D44E25A427246218E25674571DB1710E49C713FB17129549C29E303086A", "coldfusion"},
	{"0x0200B9BD5C85918D9BEE84417957618FBA1CB80B71E81550FAE09AD027B4089017CD6461D8EC9509873C2D5096CDBE8F16E4EFA9035C35F9F4917CE58DB99DC6836CEA7483A7", "sql2005"},
	{NULL}
};

static unsigned char cursalt[SALT_SIZE];
#ifdef SIMD_COEF_64
static ARCH_WORD_64 (*saved_key)[SHA512_BUF_SIZ];
static ARCH_WORD_64 (*crypt_out)[8*SIMD_COEF_64];
static int max_keys;
static int new_keys;
#else
static char (*saved_key)[(PLAINTEXT_LENGTH + 1) * 2 + SALT_SIZE];
static ARCH_WORD_32 (*crypt_out)[BINARY_SIZE / 4];
static int *key_length;
#endif

static int valid(char *ciphertext, struct fmt_main *self)
{
	int i;

	if (strlen(ciphertext) != CIPHERTEXT_LENGTH)
		return 0;
	if (strncmp(ciphertext, "0x0200", 6))
		return 0;
	for (i = 6; i < CIPHERTEXT_LENGTH; i++) {
		if (!((('0' <= ciphertext[i])&&(ciphertext[i] <= '9')) ||
		      (('a' <= ciphertext[i])&&(ciphertext[i] <= 'f'))
		      || (('A' <= ciphertext[i])&&(ciphertext[i] <= 'F'))))
			return 0;
	}
	return 1;
}

static void set_salt(void *salt)
{
	memcpy(cursalt, salt, SALT_SIZE);
#ifdef SIMD_COEF_64
	new_keys = 1;
#endif
}

static void *get_salt(char *ciphertext)
{
	static unsigned char *out2;
	int l;

	if (!out2) out2 = mem_alloc_tiny(SALT_SIZE, MEM_ALIGN_WORD);

	for (l = 0;l<SALT_SIZE;l++)
	{
		out2[l] = atoi16[ARCH_INDEX(ciphertext[l*2+6])]*16
			+ atoi16[ARCH_INDEX(ciphertext[l*2+7])];
	}

	return out2;
}

static void set_key_enc(char *_key, int index);

static void init(struct fmt_main *self)
{
#if defined (_OPENMP)
	omp_t = omp_get_max_threads();
	self->params.min_keys_per_crypt *= omp_t;
	omp_t *= OMP_SCALE;
	self->params.max_keys_per_crypt *= omp_t;
#endif
#ifdef SIMD_COEF_64
	saved_key = mem_calloc_tiny(sizeof(*saved_key) * self->params.max_keys_per_crypt, MEM_ALIGN_SIMD);
	crypt_out = mem_calloc_tiny(sizeof(*crypt_out) * self->params.max_keys_per_crypt/SIMD_COEF_64, MEM_ALIGN_SIMD);
	max_keys = self->params.max_keys_per_crypt;
#else
	saved_key = mem_calloc_tiny(sizeof(*saved_key) *
			self->params.max_keys_per_crypt, MEM_ALIGN_WORD);
	crypt_out = mem_calloc_tiny(sizeof(*crypt_out) * self->params.max_keys_per_crypt, MEM_ALIGN_WORD);
	key_length = mem_calloc_tiny(sizeof(*key_length) * self->params.max_keys_per_crypt, MEM_ALIGN_WORD);
#endif
	if (pers_opts.target_enc == UTF_8)
		self->params.plaintext_length = MIN(125, PLAINTEXT_LENGTH * 3);

	if (pers_opts.target_enc != ISO_8859_1 &&
	         pers_opts.target_enc != ASCII)
		self->methods.set_key = set_key_enc;
}

static void clear_keys()
{
#ifdef SIMD_COEF_64
	memset(saved_key, 0, sizeof(*saved_key) * max_keys);
#endif
}

static void set_key(char *_key, int index)
{
#ifndef SIMD_COEF_64
	/* ASCII or ISO-8859-1 to UCS-2 */
	UTF8 *s = (UTF8*)_key;
	UTF16 *d = (UTF16*)saved_key[index];

	for (key_length[index] = 0; s[key_length[index]]; key_length[index]++)
#if ARCH_LITTLE_ENDIAN
		d[key_length[index]] = s[key_length[index]];
#else
		d[key_length[index]] = s[key_length[index]] << 8;
#endif
	d[key_length[index]] = 0;
	key_length[index] <<= 1;
#else
	ARCH_WORD_64 *keybuffer = saved_key[index];
	unsigned short *w16 = (unsigned short*)keybuffer;
	UTF8 *key = (UTF8*)_key;
	int len = 0;

	while ((*w16++ = *key++))
		len++;

	keybuffer[15] = ((len << 1) + SALT_SIZE) << 3;

	new_keys = 1;
#endif
}

static void set_key_enc(char *_key, int index)
{
#ifndef SIMD_COEF_64
	/* Any encoding -> UTF-16 */
	key_length[index] = enc_to_utf16((UTF16*)saved_key[index],
	                                 PLAINTEXT_LENGTH,
	                                 (unsigned char*)_key, strlen(_key));
	if (key_length[index] < 0)
		key_length[index] = strlen16((UTF16*)saved_key[index]);
	key_length[index] <<= 1;
#else
	ARCH_WORD_64 *keybuffer = saved_key[index];
	UTF16 *w16 = (UTF16*)keybuffer;
	UTF8 *key = (UTF8*)_key;
	int len;

	len = enc_to_utf16(w16, PLAINTEXT_LENGTH, key, strlen(_key));

	if (len < 0)
		len = strlen16(w16);

	keybuffer[15] = ((len << 1) + SALT_SIZE) << 3;

	new_keys = 1;
#endif
}

static char *get_key(int index)
{
#ifndef SIMD_COEF_64
	((UTF16*)saved_key[index])[key_length[index]>>1] = 0;
	return (char*)utf16_to_enc((UTF16*)saved_key[index]);
#else
	ARCH_WORD_64 *keybuffer = saved_key[index];
	UTF16 *w16 = (UTF16*)keybuffer;
	static UTF16 out[PLAINTEXT_LENGTH + 1];
	unsigned int i, len;

	len = ((keybuffer[15] >> 3) - SALT_SIZE) >> 1;

	for(i = 0; i < len; i++)
		out[i] = w16[i];

	out[i] = 0;

	return (char*)utf16_to_enc(out);
#endif
}

static int cmp_all(void *binary, int count)
{
	int index;
	for (index = 0; index < count; index++)
#ifdef SIMD_COEF_64
		if (((ARCH_WORD_64*) binary)[0] == crypt_out[index>>(SIMD_COEF_64>>1)][index&(SIMD_COEF_64-1)])
			return 1;
#else
		if ( ((ARCH_WORD_32*)binary)[0] == crypt_out[index][0] )
			return 1;
#endif
	return 0;
}

static int cmp_exact(char *source, int count)
{
	return 1;
}

static int cmp_one(void *binary, int index)
{
#ifdef SIMD_COEF_64
	int i;
	for (i = 0; i < BINARY_SIZE/sizeof(ARCH_WORD_64); i++)
		if (((ARCH_WORD_64*) binary)[i] != crypt_out[index>>(SIMD_COEF_64>>1)][(index&(SIMD_COEF_64-1))+i*SIMD_COEF_64])
			return 0;
	return 1;
#else
	return !memcmp(binary, crypt_out[index], BINARY_SIZE);
#endif
}

static int crypt_all(int *pcount, struct db_salt *salt)
{
	const int count = *pcount;
	int index = 0;
#ifdef SIMD_COEF_64
	const int inc = SIMD_COEF_64;
#else
	const int inc = 1;
#endif

#ifdef _OPENMP
#pragma omp parallel for
#endif
#if defined(_OPENMP) || PLAINTEXT_LENGTH > 1
	for (index = 0; index < count; index += inc)
#endif
	{
#ifdef SIMD_COEF_64
		if (new_keys) {
			int i;
			for (i = 0; i < SIMD_COEF_64; i++) {
				ARCH_WORD_64 *keybuffer = saved_key[index + i];
				unsigned char *wucp = (unsigned char*)keybuffer;
				int j, len = (keybuffer[15] >> 3) - SALT_SIZE;

				if (len >= 0)
				for (j = 0; j < SALT_SIZE; j++)
					wucp[len + j] = cursalt[j];

				wucp[len + 4] = 0x80;
			}
		}
		SSESHA512body(&saved_key[index], crypt_out[index/SIMD_COEF_64], NULL, SSEi_FLAT_IN);
#else
		SHA512_CTX ctx;
		memcpy(saved_key[index]+key_length[index], cursalt, SALT_SIZE);
		SHA512_Init(&ctx );
		SHA512_Update(&ctx, saved_key[index], key_length[index]+SALT_SIZE );
		SHA512_Final((unsigned char *)crypt_out[index], &ctx);
#endif
	}
#ifdef SIMD_COEF_64
	new_keys = 0;
#endif
	return count;
}

static void *binary(char *ciphertext)
{
	static char *realcipher;
	int i;

	if (!realcipher)
		realcipher = mem_alloc_tiny(BINARY_SIZE, BINARY_ALIGN);

	for (i = 0;i<BINARY_SIZE;i++)
		realcipher[i] = atoi16[ARCH_INDEX(ciphertext[i*2+14])]*16 +
			atoi16[ARCH_INDEX(ciphertext[i*2+15])];

#ifdef SIMD_COEF_64
	alter_endianity_to_BE64 (realcipher, BINARY_SIZE/8);
#endif
	return (void *)realcipher;
}

#ifdef SIMD_COEF_64
static int get_hash_0 (int index) { return crypt_out[index>>(SIMD_COEF_64>>1)][index&(SIMD_COEF_64-1)] & 0xf; }
static int get_hash_1 (int index) { return crypt_out[index>>(SIMD_COEF_64>>1)][index&(SIMD_COEF_64-1)] & 0xff; }
static int get_hash_2 (int index) { return crypt_out[index>>(SIMD_COEF_64>>1)][index&(SIMD_COEF_64-1)] & 0xfff; }
static int get_hash_3 (int index) { return crypt_out[index>>(SIMD_COEF_64>>1)][index&(SIMD_COEF_64-1)] & 0xffff; }
static int get_hash_4 (int index) { return crypt_out[index>>(SIMD_COEF_64>>1)][index&(SIMD_COEF_64-1)] & 0xfffff; }
static int get_hash_5 (int index) { return crypt_out[index>>(SIMD_COEF_64>>1)][index&(SIMD_COEF_64-1)] & 0xffffff; }
static int get_hash_6 (int index) { return crypt_out[index>>(SIMD_COEF_64>>1)][index&(SIMD_COEF_64-1)] & 0x7ffffff; }
#else
static int get_hash_0(int index) { return crypt_out[index][0] & 0xf; }
static int get_hash_1(int index) { return crypt_out[index][0] & 0xff; }
static int get_hash_2(int index) { return crypt_out[index][0] & 0xfff; }
static int get_hash_3(int index) { return crypt_out[index][0] & 0xffff; }
static int get_hash_4(int index) { return crypt_out[index][0] & 0xfffff; }
static int get_hash_5(int index) { return crypt_out[index][0] & 0xffffff; }
static int get_hash_6(int index) { return crypt_out[index][0] & 0x7ffffff; }
#endif

static int salt_hash(void *salt)
{
	// The >> 8 gave much better distribution on a huge set I analysed
	// although that was mssql05
	return (*((ARCH_WORD_32 *)salt) >> 8) & (SALT_HASH_SIZE - 1);
}

struct fmt_main fmt_mssql12 = {
	{
		FORMAT_LABEL,
		FORMAT_NAME,
		ALGORITHM_NAME,
		BENCHMARK_COMMENT,
		BENCHMARK_LENGTH,
		0,
		PLAINTEXT_LENGTH,
		BINARY_SIZE,
		BINARY_ALIGN,
		SALT_SIZE,
		SALT_ALIGN,
		MIN_KEYS_PER_CRYPT,
		MAX_KEYS_PER_CRYPT,
		FMT_CASE | FMT_8_BIT | FMT_UNICODE | FMT_UTF8 | FMT_OMP,
#if FMT_MAIN_VERSION > 11
		{ NULL },
#endif
		tests
	}, {
		init,
		fmt_default_done,
		fmt_default_reset,
		fmt_default_prepare,
		valid,
		fmt_default_split,
		binary,
		get_salt,
#if FMT_MAIN_VERSION > 11
		{ NULL },
#endif
		fmt_default_source,
		{
			fmt_default_binary_hash_0,
			fmt_default_binary_hash_1,
			fmt_default_binary_hash_2,
			fmt_default_binary_hash_3,
			fmt_default_binary_hash_4,
			fmt_default_binary_hash_5,
			fmt_default_binary_hash_6
		},
		salt_hash,
		NULL,
		set_salt,
		set_key,
		get_key,
#ifdef SIMD_COEF_64
		clear_keys,
#else
		fmt_default_clear_keys,
#endif
		crypt_all,
		{
			get_hash_0,
			get_hash_1,
			get_hash_2,
			get_hash_3,
			get_hash_4,
			get_hash_5,
			get_hash_6
		},
		cmp_all,
		cmp_one,
		cmp_exact
	}
};

#endif /* plugin stanza */