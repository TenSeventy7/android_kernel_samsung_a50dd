/*
 * Copyright (C) 2003 Jana Saout <jana@saout.de>
 * Copyright (C) 2004 Clemens Fruhwirth <clemens@endorphin.org>
 * Copyright (C) 2006-2017 Red Hat, Inc. All rights reserved.
 * Copyright (C) 2013-2017 Milan Broz <gmazyland@gmail.com>
 *
 * This file is released under the GPL.
 */

#include <linux/completion.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/key.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/mempool.h>
#include <linux/slab.h>
#include <linux/crypto.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/backing-dev.h>
#include <linux/atomic.h>
#include <linux/scatterlist.h>
#include <linux/rbtree.h>
#include <linux/ctype.h>
#include <asm/page.h>
#include <asm/unaligned.h>
#include <crypto/hash.h>
#include <crypto/md5.h>
#include <crypto/algapi.h>
#include <crypto/skcipher.h>
#include <crypto/aead.h>
#include <crypto/authenc.h>
#include <linux/rtnetlink.h> /* for struct rtattr and RTA macros only */
#include <keys/user-type.h>

#include <linux/device-mapper.h>
#include <crypto/diskcipher.h>

#define DM_MSG_PREFIX "crypt"

/*
 * context holding the current state of a multi-part conversion
 */
struct convert_context {
	struct completion restart;
	struct bio *bio_in;
	struct bio *bio_out;
	struct bvec_iter iter_in;
	struct bvec_iter iter_out;
	sector_t cc_sector;
	atomic_t cc_pending;
	union {
		struct skcipher_request *req;
		struct aead_request *req_aead;
	} r;

};

/*
 * per bio private data
 */
struct dm_crypt_io {
	struct crypt_config *cc;
	struct bio *base_bio;
	u8 *integrity_metadata;
	bool integrity_metadata_from_pool;
	struct work_struct work;

	struct convert_context ctx;

	atomic_t io_pending;
	blk_status_t error;
	sector_t sector;

	struct rb_node rb_node;
} CRYPTO_MINALIGN_ATTR;

struct dm_crypt_request {
	struct convert_context *ctx;
	struct scatterlist sg_in[4];
	struct scatterlist sg_out[4];
	sector_t iv_sector;
};

struct crypt_config;

struct crypt_iv_operations {
	int (*ctr)(struct crypt_config *cc, struct dm_target *ti,
		   const char *opts);
	void (*dtr)(struct crypt_config *cc);
	int (*init)(struct crypt_config *cc);
	int (*wipe)(struct crypt_config *cc);
	int (*generator)(struct crypt_config *cc, u8 *iv,
			 struct dm_crypt_request *dmreq);
	int (*post)(struct crypt_config *cc, u8 *iv,
		    struct dm_crypt_request *dmreq);
};

struct iv_essiv_private {
	struct crypto_ahash *hash_tfm;
	u8 *salt;
};

struct iv_benbi_private {
	int shift;
};

#define LMK_SEED_SIZE 64 /* hash + 0 */
struct iv_lmk_private {
	struct crypto_shash *hash_tfm;
	u8 *seed;
};

#define TCW_WHITENING_SIZE 16
struct iv_tcw_private {
	struct crypto_shash *crc32_tfm;
	u8 *iv_seed;
	u8 *whitening;
};

/*
 * Crypt: maps a linear range of a block device
 * and encrypts / decrypts at the same time.
 */
enum flags { DM_CRYPT_SUSPENDED, DM_CRYPT_KEY_VALID,
	     DM_CRYPT_SAME_CPU, DM_CRYPT_NO_OFFLOAD };

enum cipher_flags {
	CRYPT_MODE_INTEGRITY_AEAD,	/* Use authenticated mode for cihper */
	CRYPT_IV_LARGE_SECTORS,		/* Calculate IV from sector_size, not 512B sectors */
	CRYPT_MODE_DISKCIPHER,
	CRYPT_MODE_SKCIPHER,
};

/*
 * The fields in here must be read only after initialization.
 */
struct crypt_config {
	struct dm_dev *dev;
	sector_t start;

	/*
	 * pool for per bio private data, crypto requests,
	 * encryption requeusts/buffer pages and integrity tags
	 */
	mempool_t *req_pool;
	mempool_t *page_pool;
	mempool_t *tag_pool;
	unsigned tag_pool_max_sectors;

	struct percpu_counter n_allocated_pages;

	struct bio_set *bs;
	struct mutex bio_alloc_lock;

	struct workqueue_struct *io_queue;
	struct workqueue_struct *crypt_queue;

	struct task_struct *write_thread;
	wait_queue_head_t write_thread_wait;
	struct rb_root write_tree;

	char *cipher;
	char *cipher_string;
	char *cipher_auth;
	char *key_string;

	const struct crypt_iv_operations *iv_gen_ops;
	union {
		struct iv_essiv_private essiv;
		struct iv_benbi_private benbi;
		struct iv_lmk_private lmk;
		struct iv_tcw_private tcw;
	} iv_gen_private;
	sector_t iv_offset;
	unsigned int iv_size;
	unsigned short int sector_size;
	unsigned char sector_shift;

	/* ESSIV: struct crypto_cipher *essiv_tfm */
	void *iv_private;
	union {
		struct crypto_skcipher **tfms;
		struct crypto_aead **tfms_aead;
		struct crypto_diskcipher **tfms_diskc;
	} cipher_tfm;
	unsigned tfms_count;
	unsigned long cipher_flags;

	/*
	 * Layout of each crypto request:
	 *
	 *   struct skcipher_request
	 *      context
	 *      padding
	 *   struct dm_crypt_request
	 *      padding
	 *   IV
	 *
	 * The padding is added so that dm_crypt_request and the IV are
	 * correctly aligned.
	 */
	unsigned int dmreq_start;

	unsigned int per_bio_data_size;

	unsigned long flags;
	unsigned int key_size;
	unsigned int key_parts;      /* independent parts in key buffer */
	unsigned int key_extra_size; /* additional keys length */
	unsigned int key_mac_size;   /* MAC key size for authenc(...) */

	unsigned int integrity_tag_size;
	unsigned int integrity_iv_size;
	unsigned int on_disk_tag_size;

	u8 *authenc_key; /* space for keys in authenc() format (if used) */
	u8 key[0];
};

#define MIN_IOS		64
#define MAX_TAG_SIZE	480
#define POOL_ENTRY_SIZE	512

static DEFINE_SPINLOCK(dm_crypt_clients_lock);
static unsigned dm_crypt_clients_n = 0;
static volatile unsigned long dm_crypt_pages_per_client;
#define DM_CRYPT_MEMORY_PERCENT			2
#define DM_CRYPT_MIN_PAGES_PER_CLIENT		(BIO_MAX_PAGES * 16)

static void clone_init(struct dm_crypt_io *, struct bio *);
static void kcryptd_queue_crypt(struct dm_crypt_io *io);
static struct scatterlist *crypt_get_sg_data(struct crypt_config *cc,
					     struct scatterlist *sg);

/*
 * Use this to access cipher attributes that are independent of the key.
 */
static struct crypto_skcipher *any_tfm(struct crypt_config *cc)
{
	return cc->cipher_tfm.tfms[0];
}

static struct crypto_aead *any_tfm_aead(struct crypt_config *cc)
{
	return cc->cipher_tfm.tfms_aead[0];
}

/*
 * Different IV generation algorithms:
 *
 * plain: the initial vector is the 32-bit little-endian version of the sector
 *        number, padded with zeros if necessary.
 *
 * plain64: the initial vector is the 64-bit little-endian version of the sector
 *        number, padded with zeros if necessary.
 *
 * plain64be: the initial vector is the 64-bit big-endian version of the sector
 *        number, padded with zeros if necessary.
 *
 * essiv: "encrypted sector|salt initial vector", the sector number is
 *        encrypted with the bulk cipher using a salt as key. The salt
 *        should be derived from the bulk cipher's key via hashing.
 *
 * benbi: the 64-bit "big-endian 'narrow block'-count", starting at 1
 *        (needed for LRW-32-AES and possible other narrow block modes)
 *
 * null: the initial vector is always zero.  Provides compatibility with
 *       obsolete loop_fish2 devices.  Do not use for new devices.
 *
 * lmk:  Compatible implementation of the block chaining mode used
 *       by the Loop-AES block device encryption system
 *       designed by Jari Ruusu. See http://loop-aes.sourceforge.net/
 *       It operates on full 512 byte sectors and uses CBC
 *       with an IV derived from the sector number, the data and
 *       optionally extra IV seed.
 *       This means that after decryption the first block
 *       of sector must be tweaked according to decrypted data.
 *       Loop-AES can use three encryption schemes:
 *         version 1: is plain aes-cbc mode
 *         version 2: uses 64 multikey scheme with lmk IV generator
 *         version 3: the same as version 2 with additional IV seed
 *                   (it uses 65 keys, last key is used as IV seed)
 *
 * tcw:  Compatible implementation of the block chaining mode used
 *       by the TrueCrypt device encryption system (prior to version 4.1).
 *       For more info see: https://gitlab.com/cryptsetup/cryptsetup/wikis/TrueCryptOnDiskFormat
 *       It operates on full 512 byte sectors and uses CBC
 *       with an IV derived from initial key and the sector number.
 *       In addition, whitening value is applied on every sector, whitening
 *       is calculated from initial key, sector number and mixed using CRC32.
 *       Note that this encryption scheme is vulnerable to watermarking attacks
 *       and should be used for old compatible containers access only.
 *
 * plumb: unimplemented, see:
 * http://article.gmane.org/gmane.linux.kernel.device-mapper.dm-crypt/454
 */

static int crypt_iv_plain_gen(struct crypt_config *cc, u8 *iv,
			      struct dm_crypt_request *dmreq)
{
	memset(iv, 0, cc->iv_size);
	*(__le32 *)iv = cpu_to_le32(dmreq->iv_sector & 0xffffffff);

	return 0;
}

static int crypt_iv_plain64_gen(struct crypt_config *cc, u8 *iv,
				struct dm_crypt_request *dmreq)
{
	memset(iv, 0, cc->iv_size);
	*(__le64 *)iv = cpu_to_le64(dmreq->iv_sector);

	return 0;
}

static int crypt_iv_plain64be_gen(struct crypt_config *cc, u8 *iv,
				  struct dm_crypt_request *dmreq)
{
	memset(iv, 0, cc->iv_size);
	/* iv_size is at least of size u64; usually it is 16 bytes */
	*(__be64 *)&iv[cc->iv_size - sizeof(u64)] = cpu_to_be64(dmreq->iv_sector);

	return 0;
}

/* Initialise ESSIV - compute salt but no local memory allocations */
static int crypt_iv_essiv_init(struct crypt_config *cc)
{
	struct iv_essiv_private *essiv = &cc->iv_gen_private.essiv;
	AHASH_REQUEST_ON_STACK(req, essiv->hash_tfm);
	struct scatterlist sg;
	struct crypto_cipher *essiv_tfm;
	int err;

	sg_init_one(&sg, cc->key, cc->key_size);
	ahash_request_set_tfm(req, essiv->hash_tfm);
	ahash_request_set_callback(req, CRYPTO_TFM_REQ_MAY_SLEEP, NULL, NULL);
	ahash_request_set_crypt(req, &sg, essiv->salt, cc->key_size);

	err = crypto_ahash_digest(req);
	ahash_request_zero(req);
	if (err)
		return err;

	essiv_tfm = cc->iv_private;

	err = crypto_cipher_setkey(essiv_tfm, essiv->salt,
			    crypto_ahash_digestsize(essiv->hash_tfm));
	if (err)
		return err;

	return 0;
}

/* Wipe salt and reset key derived from volume key */
static int crypt_iv_essiv_wipe(struct crypt_config *cc)
{
	struct iv_essiv_private *essiv = &cc->iv_gen_private.essiv;
	unsigned salt_size = crypto_ahash_digestsize(essiv->hash_tfm);
	struct crypto_cipher *essiv_tfm;
	int r, err = 0;

	memset(essiv->salt, 0, salt_size);

	essiv_tfm = cc->iv_private;
	r = crypto_cipher_setkey(essiv_tfm, essiv->salt, salt_size);
	if (r)
		err = r;

	return err;
}

/* Allocate the cipher for ESSIV */
static struct crypto_cipher *alloc_essiv_cipher(struct crypt_config *cc,
						struct dm_target *ti,
						const u8 *salt,
						unsigned int saltsize)
{
	struct crypto_cipher *essiv_tfm;
	int err;

	/* Setup the essiv_tfm with the given salt */
	essiv_tfm = crypto_alloc_cipher(cc->cipher, 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(essiv_tfm)) {
		ti->error = "Error allocating crypto tfm for ESSIV";
		return essiv_tfm;
	}

	if (crypto_cipher_blocksize(essiv_tfm) != cc->iv_size) {
		ti->error = "Block size of ESSIV cipher does "
			    "not match IV size of block cipher";
		crypto_free_cipher(essiv_tfm);
		return ERR_PTR(-EINVAL);
	}

	err = crypto_cipher_setkey(essiv_tfm, salt, saltsize);
	if (err) {
		ti->error = "Failed to set key for ESSIV cipher";
		crypto_free_cipher(essiv_tfm);
		return ERR_PTR(err);
	}

	return essiv_tfm;
}

static void crypt_iv_essiv_dtr(struct crypt_config *cc)
{
	struct crypto_cipher *essiv_tfm;
	struct iv_essiv_private *essiv = &cc->iv_gen_private.essiv;

	crypto_free_ahash(essiv->hash_tfm);
	essiv->hash_tfm = NULL;

	kzfree(essiv->salt);
	essiv->salt = NULL;

	essiv_tfm = cc->iv_private;

	if (essiv_tfm)
		crypto_free_cipher(essiv_tfm);

	cc->iv_private = NULL;
}

static int crypt_iv_essiv_ctr(struct crypt_config *cc, struct dm_target *ti,
			      const char *opts)
{
	struct crypto_cipher *essiv_tfm = NULL;
	struct crypto_ahash *hash_tfm = NULL;
	u8 *salt = NULL;
	int err;

	if (!opts) {
		ti->error = "Digest algorithm missing for ESSIV mode";
		return -EINVAL;
	}

	/* Allocate hash algorithm */
	hash_tfm = crypto_alloc_ahash(opts, 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(hash_tfm)) {
		ti->error = "Error initializing ESSIV hash";
		err = PTR_ERR(hash_tfm);
		goto bad;
	}

	salt = kzalloc(crypto_ahash_digestsize(hash_tfm), GFP_KERNEL);
	if (!salt) {
		ti->error = "Error kmallocing salt storage in ESSIV";
		err = -ENOMEM;
		goto bad;
	}

	cc->iv_gen_private.essiv.salt = salt;
	cc->iv_gen_private.essiv.hash_tfm = hash_tfm;

	essiv_tfm = alloc_essiv_cipher(cc, ti, salt,
				       crypto_ahash_digestsize(hash_tfm));
	if (IS_ERR(essiv_tfm)) {
		crypt_iv_essiv_dtr(cc);
		return PTR_ERR(essiv_tfm);
	}
	cc->iv_private = essiv_tfm;

	return 0;

bad:
	if (hash_tfm && !IS_ERR(hash_tfm))
		crypto_free_ahash(hash_tfm);
	kfree(salt);
	return err;
}

static int crypt_iv_essiv_gen(struct crypt_config *cc, u8 *iv,
			      struct dm_crypt_request *dmreq)
{
	struct crypto_cipher *essiv_tfm = cc->iv_private;

	memset(iv, 0, cc->iv_size);
	*(__le64 *)iv = cpu_to_le64(dmreq->iv_sector);
	crypto_cipher_encrypt_one(essiv_tfm, iv, iv);

	return 0;
}

static int crypt_iv_benbi_ctr(struct crypt_config *cc, struct dm_target *ti,
			      const char *opts)
{
	unsigned bs = crypto_skcipher_blocksize(any_tfm(cc));
	int log = ilog2(bs);

	/* we need to calculate how far we must shift the sector count
	 * to get the cipher block count, we use this shift in _gen */

	if (1 << log != bs) {
		ti->error = "cypher blocksize is not a power of 2";
		return -EINVAL;
	}

	if (log > 9) {
		ti->error = "cypher blocksize is > 512";
		return -EINVAL;
	}

	cc->iv_gen_private.benbi.shift = 9 - log;

	return 0;
}

static void crypt_iv_benbi_dtr(struct crypt_config *cc)
{
}

static int crypt_iv_benbi_gen(struct crypt_config *cc, u8 *iv,
			      struct dm_crypt_request *dmreq)
{
	__be64 val;

	memset(iv, 0, cc->iv_size - sizeof(u64)); /* rest is cleared below */

	val = cpu_to_be64(((u64)dmreq->iv_sector << cc->iv_gen_private.benbi.shift) + 1);
	put_unaligned(val, (__be64 *)(iv + cc->iv_size - sizeof(u64)));

	return 0;
}

static int crypt_iv_null_gen(struct crypt_config *cc, u8 *iv,
			     struct dm_crypt_request *dmreq)
{
	memset(iv, 0, cc->iv_size);

	return 0;
}

static void crypt_iv_lmk_dtr(struct crypt_config *cc)
{
	struct iv_lmk_private *lmk = &cc->iv_gen_private.lmk;

	if (lmk->hash_tfm && !IS_ERR(lmk->hash_tfm))
		crypto_free_shash(lmk->hash_tfm);
	lmk->hash_tfm = NULL;

	kzfree(lmk->seed);
	lmk->seed = NULL;
}

static int crypt_iv_lmk_ctr(struct crypt_config *cc, struct dm_target *ti,
			    const char *opts)
{
	struct iv_lmk_private *lmk = &cc->iv_gen_private.lmk;

	if (cc->sector_size != (1 << SECTOR_SHIFT)) {
		ti->error = "Unsupported sector size for LMK";
		return -EINVAL;
	}

	lmk->hash_tfm = crypto_alloc_shash("md5", 0, 0);
	if (IS_ERR(lmk->hash_tfm)) {
		ti->error = "Error initializing LMK hash";
		return PTR_ERR(lmk->hash_tfm);
	}

	/* No seed in LMK version 2 */
	if (cc->key_parts == cc->tfms_count) {
		lmk->seed = NULL;
		return 0;
	}

	lmk->seed = kzalloc(LMK_SEED_SIZE, GFP_KERNEL);
	if (!lmk->seed) {
		crypt_iv_lmk_dtr(cc);
		ti->error = "Error kmallocing seed storage in LMK";
		return -ENOMEM;
	}

	return 0;
}

static int crypt_iv_lmk_init(struct crypt_config *cc)
{
	struct iv_lmk_private *lmk = &cc->iv_gen_private.lmk;
	int subkey_size = cc->key_size / cc->key_parts;

	/* LMK seed is on the position of LMK_KEYS + 1 key */
	if (lmk->seed)
		memcpy(lmk->seed, cc->key + (cc->tfms_count * subkey_size),
		       crypto_shash_digestsize(lmk->hash_tfm));

	return 0;
}

static int crypt_iv_lmk_wipe(struct crypt_config *cc)
{
	struct iv_lmk_private *lmk = &cc->iv_gen_private.lmk;

	if (lmk->seed)
		memset(lmk->seed, 0, LMK_SEED_SIZE);

	return 0;
}

static int crypt_iv_lmk_one(struct crypt_config *cc, u8 *iv,
			    struct dm_crypt_request *dmreq,
			    u8 *data)
{
	struct iv_lmk_private *lmk = &cc->iv_gen_private.lmk;
	SHASH_DESC_ON_STACK(desc, lmk->hash_tfm);
	struct md5_state md5state;
	__le32 buf[4];
	int i, r;

	desc->tfm = lmk->hash_tfm;
	desc->flags = CRYPTO_TFM_REQ_MAY_SLEEP;

	r = crypto_shash_init(desc);
	if (r)
		return r;

	if (lmk->seed) {
		r = crypto_shash_update(desc, lmk->seed, LMK_SEED_SIZE);
		if (r)
			return r;
	}

	/* Sector is always 512B, block size 16, add data of blocks 1-31 */
	r = crypto_shash_update(desc, data + 16, 16 * 31);
	if (r)
		return r;

	/* Sector is cropped to 56 bits here */
	buf[0] = cpu_to_le32(dmreq->iv_sector & 0xFFFFFFFF);
	buf[1] = cpu_to_le32((((u64)dmreq->iv_sector >> 32) & 0x00FFFFFF) | 0x80000000);
	buf[2] = cpu_to_le32(4024);
	buf[3] = 0;
	r = crypto_shash_update(desc, (u8 *)buf, sizeof(buf));
	if (r)
		return r;

	/* No MD5 padding here */
	r = crypto_shash_export(desc, &md5state);
	if (r)
		return r;

	for (i = 0; i < MD5_HASH_WORDS; i++)
		__cpu_to_le32s(&md5state.hash[i]);
	memcpy(iv, &md5state.hash, cc->iv_size);

	return 0;
}

static int crypt_iv_lmk_gen(struct crypt_config *cc, u8 *iv,
			    struct dm_crypt_request *dmreq)
{
	struct scatterlist *sg;
	u8 *src;
	int r = 0;

	if (bio_data_dir(dmreq->ctx->bio_in) == WRITE) {
		sg = crypt_get_sg_data(cc, dmreq->sg_in);
		src = kmap_atomic(sg_page(sg));
		r = crypt_iv_lmk_one(cc, iv, dmreq, src + sg->offset);
		kunmap_atomic(src);
	} else
		memset(iv, 0, cc->iv_size);

	return r;
}

static int crypt_iv_lmk_post(struct crypt_config *cc, u8 *iv,
			     struct dm_crypt_request *dmreq)
{
	struct scatterlist *sg;
	u8 *dst;
	int r;

	if (bio_data_dir(dmreq->ctx->bio_in) == WRITE)
		return 0;

	sg = crypt_get_sg_data(cc, dmreq->sg_out);
	dst = kmap_atomic(sg_page(sg));
	r = crypt_iv_lmk_one(cc, iv, dmreq, dst + sg->offset);

	/* Tweak the first block of plaintext sector */
	if (!r)
		crypto_xor(dst + sg->offset, iv, cc->iv_size);

	kunmap_atomic(dst);
	return r;
}

static void crypt_iv_tcw_dtr(struct crypt_config *cc)
{
	struct iv_tcw_private *tcw = &cc->iv_gen_private.tcw;

	kzfree(tcw->iv_seed);
	tcw->iv_seed = NULL;
	kzfree(tcw->whitening);
	tcw->whitening = NULL;

	if (tcw->crc32_tfm && !IS_ERR(tcw->crc32_tfm))
		crypto_free_shash(tcw->crc32_tfm);
	tcw->crc32_tfm = NULL;
}

static int crypt_iv_tcw_ctr(struct crypt_config *cc, struct dm_target *ti,
			    const char *opts)
{
	struct iv_tcw_private *tcw = &cc->iv_gen_private.tcw;

	if (cc->sector_size != (1 << SECTOR_SHIFT)) {
		ti->error = "Unsupported sector size for TCW";
		return -EINVAL;
	}

	if (cc->key_size <= (cc->iv_size + TCW_WHITENING_SIZE)) {
		ti->error = "Wrong key size for TCW";
		return -EINVAL;
	}

	tcw->crc32_tfm = crypto_alloc_shash("crc32", 0, 0);
	if (IS_ERR(tcw->crc32_tfm)) {
		ti->error = "Error initializing CRC32 in TCW";
		return PTR_ERR(tcw->crc32_tfm);
	}

	tcw->iv_seed = kzalloc(cc->iv_size, GFP_KERNEL);
	tcw->whitening = kzalloc(TCW_WHITENING_SIZE, GFP_KERNEL);
	if (!tcw->iv_seed || !tcw->whitening) {
		crypt_iv_tcw_dtr(cc);
		ti->error = "Error allocating seed storage in TCW";
		return -ENOMEM;
	}

	return 0;
}

static int crypt_iv_tcw_init(struct crypt_config *cc)
{
	struct iv_tcw_private *tcw = &cc->iv_gen_private.tcw;
	int key_offset = cc->key_size - cc->iv_size - TCW_WHITENING_SIZE;

	memcpy(tcw->iv_seed, &cc->key[key_offset], cc->iv_size);
	memcpy(tcw->whitening, &cc->key[key_offset + cc->iv_size],
	       TCW_WHITENING_SIZE);

	return 0;
}

static int crypt_iv_tcw_wipe(struct crypt_config *cc)
{
	struct iv_tcw_private *tcw = &cc->iv_gen_private.tcw;

	memset(tcw->iv_seed, 0, cc->iv_size);
	memset(tcw->whitening, 0, TCW_WHITENING_SIZE);

	return 0;
}

static int crypt_iv_tcw_whitening(struct crypt_config *cc,
				  struct dm_crypt_request *dmreq,
				  u8 *data)
{
	struct iv_tcw_private *tcw = &cc->iv_gen_private.tcw;
	__le64 sector = cpu_to_le64(dmreq->iv_sector);
	u8 buf[TCW_WHITENING_SIZE];
	SHASH_DESC_ON_STACK(desc, tcw->crc32_tfm);
	int i, r;

	/* xor whitening with sector number */
	crypto_xor_cpy(buf, tcw->whitening, (u8 *)&sector, 8);
	crypto_xor_cpy(&buf[8], tcw->whitening + 8, (u8 *)&sector, 8);

	/* calculate crc32 for every 32bit part and xor it */
	desc->tfm = tcw->crc32_tfm;
	desc->flags = CRYPTO_TFM_REQ_MAY_SLEEP;
	for (i = 0; i < 4; i++) {
		r = crypto_shash_init(desc);
		if (r)
			goto out;
		r = crypto_shash_update(desc, &buf[i * 4], 4);
		if (r)
			goto out;
		r = crypto_shash_final(desc, &buf[i * 4]);
		if (r)
			goto out;
	}
	crypto_xor(&buf[0], &buf[12], 4);
	crypto_xor(&buf[4], &buf[8], 4);

	/* apply whitening (8 bytes) to whole sector */
	for (i = 0; i < ((1 << SECTOR_SHIFT) / 8); i++)
		crypto_xor(data + i * 8, buf, 8);
out:
	memzero_explicit(buf, sizeof(buf));
	return r;
}

static int crypt_iv_tcw_gen(struct crypt_config *cc, u8 *iv,
			    struct dm_crypt_request *dmreq)
{
	struct scatterlist *sg;
	struct iv_tcw_private *tcw = &cc->iv_gen_private.tcw;
	__le64 sector = cpu_to_le64(dmreq->iv_sector);
	u8 *src;
	int r = 0;

	/* Remove whitening from ciphertext */
	if (bio_data_dir(dmreq->ctx->bio_in) != WRITE) {
		sg = crypt_get_sg_data(cc, dmreq->sg_in);
		src = kmap_atomic(sg_page(sg));
		r = crypt_iv_tcw_whitening(cc, dmreq, src + sg->offset);
		kunmap_atomic(src);
	}

	/* Calculate IV */
	crypto_xor_cpy(iv, tcw->iv_seed, (u8 *)&sector, 8);
	if (cc->iv_size > 8)
		crypto_xor_cpy(&iv[8], tcw->iv_seed + 8, (u8 *)&sector,
			       cc->iv_size - 8);

	return r;
}

static int crypt_iv_tcw_post(struct crypt_config *cc, u8 *iv,
			     struct dm_crypt_request *dmreq)
{
	struct scatterlist *sg;
	u8 *dst;
	int r;

	if (bio_data_dir(dmreq->ctx->bio_in) != WRITE)
		return 0;

	/* Apply whitening on ciphertext */
	sg = crypt_get_sg_data(cc, dmreq->sg_out);
	dst = kmap_atomic(sg_page(sg));
	r = crypt_iv_tcw_whitening(cc, dmreq, dst + sg->offset);
	kunmap_atomic(dst);

	return r;
}

static int crypt_iv_random_gen(struct crypt_config *cc, u8 *iv,
				struct dm_crypt_request *dmreq)
{
	/* Used only for writes, there must be an additional space to store IV */
	get_random_bytes(iv, cc->iv_size);
	return 0;
}

static const struct crypt_iv_operations crypt_iv_plain_ops = {
	.generator = crypt_iv_plain_gen
};

static const struct crypt_iv_operations crypt_iv_plain64_ops = {
	.generator = crypt_iv_plain64_gen
};

static const struct crypt_iv_operations crypt_iv_plain64be_ops = {
	.generator = crypt_iv_plain64be_gen
};

static const struct crypt_iv_operations crypt_iv_essiv_ops = {
	.ctr       = crypt_iv_essiv_ctr,
	.dtr       = crypt_iv_essiv_dtr,
	.init      = crypt_iv_essiv_init,
	.wipe      = crypt_iv_essiv_wipe,
	.generator = crypt_iv_essiv_gen
};

static const struct crypt_iv_operations crypt_iv_benbi_ops = {
	.ctr	   = crypt_iv_benbi_ctr,
	.dtr	   = crypt_iv_benbi_dtr,
	.generator = crypt_iv_benbi_gen
};

static const struct crypt_iv_operations crypt_iv_null_ops = {
	.generator = crypt_iv_null_gen
};

static const struct crypt_iv_operations crypt_iv_lmk_ops = {
	.ctr	   = crypt_iv_lmk_ctr,
	.dtr	   = crypt_iv_lmk_dtr,
	.init	   = crypt_iv_lmk_init,
	.wipe	   = crypt_iv_lmk_wipe,
	.generator = crypt_iv_lmk_gen,
	.post	   = crypt_iv_lmk_post
};

static const struct crypt_iv_operations crypt_iv_tcw_ops = {
	.ctr	   = crypt_iv_tcw_ctr,
	.dtr	   = crypt_iv_tcw_dtr,
	.init	   = crypt_iv_tcw_init,
	.wipe	   = crypt_iv_tcw_wipe,
	.generator = crypt_iv_tcw_gen,
	.post	   = crypt_iv_tcw_post
};

static struct crypt_iv_operations crypt_iv_random_ops = {
	.generator = crypt_iv_random_gen
};

/*
 * Integrity extensions
 */
static bool crypt_integrity_aead(struct crypt_config *cc)
{
	return test_bit(CRYPT_MODE_INTEGRITY_AEAD, &cc->cipher_flags);
}

static bool crypt_integrity_hmac(struct crypt_config *cc)
{
	return crypt_integrity_aead(cc) && cc->key_mac_size;
}

static bool crypt_mode_diskcipher(struct crypt_config *cc)
{
	return test_bit(CRYPT_MODE_DISKCIPHER, &cc->cipher_flags);
}

static bool crypt_mode_skcipher(struct crypt_config *cc)
{
	return test_bit(CRYPT_MODE_SKCIPHER, &cc->cipher_flags);
}


/* Get sg containing data */
static struct scatterlist *crypt_get_sg_data(struct crypt_config *cc,
					     struct scatterlist *sg)
{
	if (unlikely(crypt_integrity_aead(cc)))
		return &sg[2];

	return sg;
}

static int dm_crypt_integrity_io_alloc(struct dm_crypt_io *io, struct bio *bio)
{
	struct bio_integrity_payload *bip;
	unsigned int tag_len;
	int ret;

	if (!bio_sectors(bio) || !io->cc->on_disk_tag_size)
		return 0;

	bip = bio_integrity_alloc(bio, GFP_NOIO, 1);
	if (IS_ERR(bip))
		return PTR_ERR(bip);

	tag_len = io->cc->on_disk_tag_size * bio_sectors(bio);

	bip->bip_iter.bi_size = tag_len;
	bip->bip_iter.bi_sector = io->cc->start + io->sector;

	ret = bio_integrity_add_page(bio, virt_to_page(io->integrity_metadata),
				     tag_len, offset_in_page(io->integrity_metadata));
	if (unlikely(ret != tag_len))
		return -ENOMEM;

	return 0;
}

static int crypt_integrity_ctr(struct crypt_config *cc, struct dm_target *ti)
{
#ifdef CONFIG_BLK_DEV_INTEGRITY
	struct blk_integrity *bi = blk_get_integrity(cc->dev->bdev->bd_disk);

	/* From now we require underlying device with our integrity profile */
	if (!bi || strcasecmp(bi->profile->name, "DM-DIF-EXT-TAG")) {
		ti->error = "Integrity profile not supported.";
		return -EINVAL;
	}

	if (bi->tag_size != cc->on_disk_tag_size ||
	    bi->tuple_size != cc->on_disk_tag_size) {
		ti->error = "Integrity profile tag size mismatch.";
		return -EINVAL;
	}
	if (1 << bi->interval_exp != cc->sector_size) {
		ti->error = "Integrity profile sector size mismatch.";
		return -EINVAL;
	}

	if (crypt_integrity_aead(cc)) {
		cc->integrity_tag_size = cc->on_disk_tag_size - cc->integrity_iv_size;
		DMINFO("Integrity AEAD, tag size %u, IV size %u.",
		       cc->integrity_tag_size, cc->integrity_iv_size);

		if (crypto_aead_setauthsize(any_tfm_aead(cc), cc->integrity_tag_size)) {
			ti->error = "Integrity AEAD auth tag size is not supported.";
			return -EINVAL;
		}
	} else if (cc->integrity_iv_size)
		DMINFO("Additional per-sector space %u bytes for IV.",
		       cc->integrity_iv_size);

	if ((cc->integrity_tag_size + cc->integrity_iv_size) != bi->tag_size) {
		ti->error = "Not enough space for integrity tag in the profile.";
		return -EINVAL;
	}

	return 0;
#else
	ti->error = "Integrity profile not supported.";
	return -EINVAL;
#endif
}

static void crypt_convert_init(struct crypt_config *cc,
			       struct convert_context *ctx,
			       struct bio *bio_out, struct bio *bio_in,
			       sector_t sector)
{
	ctx->bio_in = bio_in;
	ctx->bio_out = bio_out;
	if (bio_in)
		ctx->iter_in = bio_in->bi_iter;
	if (bio_out)
		ctx->iter_out = bio_out->bi_iter;
	ctx->cc_sector = sector + cc->iv_offset;
	init_completion(&ctx->restart);
}

static struct dm_crypt_request *dmreq_of_req(struct crypt_config *cc,
					     void *req)
{
	return (struct dm_crypt_request *)((char *)req + cc->dmreq_start);
}

static void *req_of_dmreq(struct crypt_config *cc, struct dm_crypt_request *dmreq)
{
	return (void *)((char *)dmreq - cc->dmreq_start);
}

static u8 *iv_of_dmreq(struct crypt_config *cc,
		       struct dm_crypt_request *dmreq)
{
	if (crypt_integrity_aead(cc))
		return (u8 *)ALIGN((unsigned long)(dmreq + 1),
			crypto_aead_alignmask(any_tfm_aead(cc)) + 1);
	else
		return (u8 *)ALIGN((unsigned long)(dmreq + 1),
			crypto_skcipher_alignmask(any_tfm(cc)) + 1);
}

static u8 *org_iv_of_dmreq(struct crypt_config *cc,
		       struct dm_crypt_request *dmreq)
{
	return iv_of_dmreq(cc, dmreq) + cc->iv_size;
}

static uint64_t *org_sector_of_dmreq(struct crypt_config *cc,
		       struct dm_crypt_request *dmreq)
{
	u8 *ptr = iv_of_dmreq(cc, dmreq) + cc->iv_size + cc->iv_size;
	return (uint64_t*) ptr;
}

static unsigned int *org_tag_of_dmreq(struct crypt_config *cc,
		       struct dm_crypt_request *dmreq)
{
	u8 *ptr = iv_of_dmreq(cc, dmreq) + cc->iv_size +
		  cc->iv_size + sizeof(uint64_t);
	return (unsigned int*)ptr;
}

static void *tag_from_dmreq(struct crypt_config *cc,
				struct dm_crypt_request *dmreq)
{
	struct convert_context *ctx = dmreq->ctx;
	struct dm_crypt_io *io = container_of(ctx, struct dm_crypt_io, ctx);

	return &io->integrity_metadata[*org_tag_of_dmreq(cc, dmreq) *
		cc->on_disk_tag_size];
}

static void *iv_tag_from_dmreq(struct crypt_config *cc,
			       struct dm_crypt_request *dmreq)
{
	return tag_from_dmreq(cc, dmreq) + cc->integrity_tag_size;
}

static int crypt_convert_block_aead(struct crypt_config *cc,
				     struct convert_context *ctx,
				     struct aead_request *req,
				     unsigned int tag_offset)
{
	struct bio_vec bv_in = bio_iter_iovec(ctx->bio_in, ctx->iter_in);
	struct bio_vec bv_out = bio_iter_iovec(ctx->bio_out, ctx->iter_out);
	struct dm_crypt_request *dmreq;
	u8 *iv, *org_iv, *tag_iv, *tag;
	uint64_t *sector;
	int r = 0;

	BUG_ON(cc->integrity_iv_size && cc->integrity_iv_size != cc->iv_size);

	/* Reject unexpected unaligned bio. */
	if (unlikely(bv_in.bv_len & (cc->sector_size - 1)))
		return -EIO;

	dmreq = dmreq_of_req(cc, req);
	dmreq->iv_sector = ctx->cc_sector;
	if (test_bit(CRYPT_IV_LARGE_SECTORS, &cc->cipher_flags))
		dmreq->iv_sector >>= cc->sector_shift;
	dmreq->ctx = ctx;

	*org_tag_of_dmreq(cc, dmreq) = tag_offset;

	sector = org_sector_of_dmreq(cc, dmreq);
	*sector = cpu_to_le64(ctx->cc_sector - cc->iv_offset);

	iv = iv_of_dmreq(cc, dmreq);
	org_iv = org_iv_of_dmreq(cc, dmreq);
	tag = tag_from_dmreq(cc, dmreq);
	tag_iv = iv_tag_from_dmreq(cc, dmreq);

	/* AEAD request:
	 *  |----- AAD -------|------ DATA -------|-- AUTH TAG --|
	 *  | (authenticated) | (auth+encryption) |              |
	 *  | sector_LE |  IV |  sector in/out    |  tag in/out  |
	 */
	sg_init_table(dmreq->sg_in, 4);
	sg_set_buf(&dmreq->sg_in[0], sector, sizeof(uint64_t));
	sg_set_buf(&dmreq->sg_in[1], org_iv, cc->iv_size);
	sg_set_page(&dmreq->sg_in[2], bv_in.bv_page, cc->sector_size, bv_in.bv_offset);
	sg_set_buf(&dmreq->sg_in[3], tag, cc->integrity_tag_size);

	sg_init_table(dmreq->sg_out, 4);
	sg_set_buf(&dmreq->sg_out[0], sector, sizeof(uint64_t));
	sg_set_buf(&dmreq->sg_out[1], org_iv, cc->iv_size);
	sg_set_page(&dmreq->sg_out[2], bv_out.bv_page, cc->sector_size, bv_out.bv_offset);
	sg_set_buf(&dmreq->sg_out[3], tag, cc->integrity_tag_size);

	if (cc->iv_gen_ops) {
		/* For READs use IV stored in integrity metadata */
		if (cc->integrity_iv_size && bio_data_dir(ctx->bio_in) != WRITE) {
			memcpy(org_iv, tag_iv, cc->iv_size);
		} else {
			r = cc->iv_gen_ops->generator(cc, org_iv, dmreq);
			if (r < 0)
				return r;
			/* Store generated IV in integrity metadata */
			if (cc->integrity_iv_size)
				memcpy(tag_iv, org_iv, cc->iv_size);
		}
		/* Working copy of IV, to be modified in crypto API */
		memcpy(iv, org_iv, cc->iv_size);
	}

	aead_request_set_ad(req, sizeof(uint64_t) + cc->iv_size);
	if (bio_data_dir(ctx->bio_in) == WRITE) {
		aead_request_set_crypt(req, dmreq->sg_in, dmreq->sg_out,
				       cc->sector_size, iv);
		r = crypto_aead_encrypt(req);
		if (cc->integrity_tag_size + cc->integrity_iv_size != cc->on_disk_tag_size)
			memset(tag + cc->integrity_tag_size + cc->integrity_iv_size, 0,
			       cc->on_disk_tag_size - (cc->integrity_tag_size + cc->integrity_iv_size));
	} else {
		aead_request_set_crypt(req, dmreq->sg_in, dmreq->sg_out,
				       cc->sector_size + cc->integrity_tag_size, iv);
		r = crypto_aead_decrypt(req);
	}

	if (r == -EBADMSG)
		DMERR_LIMIT("INTEGRITY AEAD ERROR, sector %llu",
			    (unsigned long long)le64_to_cpu(*sector));

	if (!r && cc->iv_gen_ops && cc->iv_gen_ops->post)
		r = cc->iv_gen_ops->post(cc, org_iv, dmreq);

	bio_advance_iter(ctx->bio_in, &ctx->iter_in, cc->sector_size);
	bio_advance_iter(ctx->bio_out, &ctx->iter_out, cc->sector_size);

	return r;
}

static int crypt_convert_block_skcipher(struct crypt_config *cc,
					struct convert_context *ctx,
					struct skcipher_request *req,
					unsigned int tag_offset)
{
	struct bio_vec bv_in = bio_iter_iovec(ctx->bio_in, ctx->iter_in);
	struct bio_vec bv_out = bio_iter_iovec(ctx->bio_out, ctx->iter_out);
	struct scatterlist *sg_in, *sg_out;
	struct dm_crypt_request *dmreq;
	u8 *iv, *org_iv, *tag_iv;
	uint64_t *sector;
	int r = 0;

	/* Reject unexpected unaligned bio. */
	if (unlikely(bv_in.bv_len & (cc->sector_size - 1)))
		return -EIO;

	dmreq = dmreq_of_req(cc, req);
	dmreq->iv_sector = ctx->cc_sector;
	if (test_bit(CRYPT_IV_LARGE_SECTORS, &cc->cipher_flags))
		dmreq->iv_sector >>= cc->sector_shift;
	dmreq->ctx = ctx;

	*org_tag_of_dmreq(cc, dmreq) = tag_offset;

	iv = iv_of_dmreq(cc, dmreq);
	org_iv = org_iv_of_dmreq(cc, dmreq);
	tag_iv = iv_tag_from_dmreq(cc, dmreq);

	sector = org_sector_of_dmreq(cc, dmreq);
	*sector = cpu_to_le64(ctx->cc_sector - cc->iv_offset);

	/* For skcipher we use only the first sg item */
	sg_in  = &dmreq->sg_in[0];
	sg_out = &dmreq->sg_out[0];

	sg_init_table(sg_in, 1);
	sg_set_page(sg_in, bv_in.bv_page, cc->sector_size, bv_in.bv_offset);

	sg_init_table(sg_out, 1);
	sg_set_page(sg_out, bv_out.bv_page, cc->sector_size, bv_out.bv_offset);

	if (cc->iv_gen_ops) {
		/* For READs use IV stored in integrity metadata */
		if (cc->integrity_iv_size && bio_data_dir(ctx->bio_in) != WRITE) {
			memcpy(org_iv, tag_iv, cc->integrity_iv_size);
		} else {
			r = cc->iv_gen_ops->generator(cc, org_iv, dmreq);
			if (r < 0)
				return r;
			/* Store generated IV in integrity metadata */
			if (cc->integrity_iv_size)
				memcpy(tag_iv, org_iv, cc->integrity_iv_size);
		}
		/* Working copy of IV, to be modified in crypto API */
		memcpy(iv, org_iv, cc->iv_size);
	}

	skcipher_request_set_crypt(req, sg_in, sg_out, cc->sector_size, iv);

	if (bio_data_dir(ctx->bio_in) == WRITE)
		r = crypto_skcipher_encrypt(req);
	else
		r = crypto_skcipher_decrypt(req);

	if (!r && cc->iv_gen_ops && cc->iv_gen_ops->post)
		r = cc->iv_gen_ops->post(cc, org_iv, dmreq);

	bio_advance_iter(ctx->bio_in, &ctx->iter_in, cc->sector_size);
	bio_advance_iter(ctx->bio_out, &ctx->iter_out, cc->sector_size);

	return r;
}

static void kcryptd_async_done(struct crypto_async_request *async_req,
			       int error);

static void crypt_alloc_req_skcipher(struct crypt_config *cc,
				     struct convert_context *ctx)
{
	unsigned key_index = ctx->cc_sector & (cc->tfms_count - 1);

	if (!ctx->r.req)
		ctx->r.req = mempool_alloc(cc->req_pool, GFP_NOIO);

	skcipher_request_set_tfm(ctx->r.req, cc->cipher_tfm.tfms[key_index]);

	/*
	 * Use REQ_MAY_BACKLOG so a cipher driver internally backlogs
	 * requests if driver request queue is full.
	 */
	skcipher_request_set_callback(ctx->r.req,
	    CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
	    kcryptd_async_done, dmreq_of_req(cc, ctx->r.req));
}

static void crypt_alloc_req_aead(struct crypt_config *cc,
				 struct convert_context *ctx)
{
	if (!ctx->r.req_aead)
		ctx->r.req_aead = mempool_alloc(cc->req_pool, GFP_NOIO);

	aead_request_set_tfm(ctx->r.req_aead, cc->cipher_tfm.tfms_aead[0]);

	/*
	 * Use REQ_MAY_BACKLOG so a cipher driver internally backlogs
	 * requests if driver request queue is full.
	 */
	aead_request_set_callback(ctx->r.req_aead,
	    CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
	    kcryptd_async_done, dmreq_of_req(cc, ctx->r.req_aead));
}

static void crypt_alloc_req(struct crypt_config *cc,
			    struct convert_context *ctx)
{
	if (crypt_integrity_aead(cc))
		crypt_alloc_req_aead(cc, ctx);
	else
		crypt_alloc_req_skcipher(cc, ctx);
}

static void crypt_free_req_skcipher(struct crypt_config *cc,
				    struct skcipher_request *req, struct bio *base_bio)
{
	struct dm_crypt_io *io = dm_per_bio_data(base_bio, cc->per_bio_data_size);

	if ((struct skcipher_request *)(io + 1) != req)
		mempool_free(req, cc->req_pool);
}

static void crypt_free_req_aead(struct crypt_config *cc,
				struct aead_request *req, struct bio *base_bio)
{
	struct dm_crypt_io *io = dm_per_bio_data(base_bio, cc->per_bio_data_size);

	if ((struct aead_request *)(io + 1) != req)
		mempool_free(req, cc->req_pool);
}

static void crypt_free_req(struct crypt_config *cc, void *req, struct bio *base_bio)
{
	if (crypt_integrity_aead(cc))
		crypt_free_req_aead(cc, req, base_bio);
	else
		crypt_free_req_skcipher(cc, req, base_bio);
}

/*
 * Encrypt / decrypt data from one bio to another one (can be the same one)
 */
static blk_status_t crypt_convert(struct crypt_config *cc,
			 struct convert_context *ctx)
{
	unsigned int tag_offset = 0;
	unsigned int sector_step = cc->sector_size >> SECTOR_SHIFT;
	int r;

	atomic_set(&ctx->cc_pending, 1);

	while (ctx->iter_in.bi_size && ctx->iter_out.bi_size) {

		crypt_alloc_req(cc, ctx);
		atomic_inc(&ctx->cc_pending);

		if (crypt_integrity_aead(cc))
			r = crypt_convert_block_aead(cc, ctx, ctx->r.req_aead, tag_offset);
		else
			r = crypt_convert_block_skcipher(cc, ctx, ctx->r.req, tag_offset);

		switch (r) {
		/*
		 * The request was queued by a crypto driver
		 * but the driver request queue is full, let's wait.
		 */
		case -EBUSY:
			wait_for_completion(&ctx->restart);
			reinit_completion(&ctx->restart);
			/* fall through */
		/*
		 * The request is queued and processed asynchronously,
		 * completion function kcryptd_async_done() will be called.
		 */
		case -EINPROGRESS:
			ctx->r.req = NULL;
			ctx->cc_sector += sector_step;
			tag_offset++;
			continue;
		/*
		 * The request was already processed (synchronously).
		 */
		case 0:
			atomic_dec(&ctx->cc_pending);
			ctx->cc_sector += sector_step;
			tag_offset++;
			cond_resched();
			continue;
		/*
		 * There was a data integrity error.
		 */
		case -EBADMSG:
			atomic_dec(&ctx->cc_pending);
			return BLK_STS_PROTECTION;
		/*
		 * There was an error while processing the request.
		 */
		default:
			atomic_dec(&ctx->cc_pending);
			return BLK_STS_IOERR;
		}
	}

	return 0;
}

static void crypt_free_buffer_pages(struct crypt_config *cc, struct bio *clone);

/*
 * Generate a new unfragmented bio with the given size
 * This should never violate the device limitations (but only because
 * max_segment_size is being constrained to PAGE_SIZE).
 *
 * This function may be called concurrently. If we allocate from the mempool
 * concurrently, there is a possibility of deadlock. For example, if we have
 * mempool of 256 pages, two processes, each wanting 256, pages allocate from
 * the mempool concurrently, it may deadlock in a situation where both processes
 * have allocated 128 pages and the mempool is exhausted.
 *
 * In order to avoid this scenario we allocate the pages under a mutex.
 *
 * In order to not degrade performance with excessive locking, we try
 * non-blocking allocations without a mutex first but on failure we fallback
 * to blocking allocations with a mutex.
 */
static struct bio *crypt_alloc_buffer(struct dm_crypt_io *io, unsigned size)
{
	struct crypt_config *cc = io->cc;
	struct bio *clone;
	unsigned int nr_iovecs = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	gfp_t gfp_mask = GFP_NOWAIT | __GFP_HIGHMEM;
	unsigned i, len, remaining_size;
	struct page *page;

retry:
	if (unlikely(gfp_mask & __GFP_DIRECT_RECLAIM))
		mutex_lock(&cc->bio_alloc_lock);

	clone = bio_alloc_bioset(GFP_NOIO, nr_iovecs, cc->bs);
	if (!clone)
		goto out;

	clone_init(io, clone);

	remaining_size = size;

	for (i = 0; i < nr_iovecs; i++) {
		page = mempool_alloc(cc->page_pool, gfp_mask);
		if (!page) {
			crypt_free_buffer_pages(cc, clone);
			bio_put(clone);
			gfp_mask |= __GFP_DIRECT_RECLAIM;
			goto retry;
		}

		len = (remaining_size > PAGE_SIZE) ? PAGE_SIZE : remaining_size;

		bio_add_page(clone, page, len, 0);

		remaining_size -= len;
	}

	/* Allocate space for integrity tags */
	if (dm_crypt_integrity_io_alloc(io, clone)) {
		crypt_free_buffer_pages(cc, clone);
		bio_put(clone);
		clone = NULL;
	}
out:
	if (unlikely(gfp_mask & __GFP_DIRECT_RECLAIM))
		mutex_unlock(&cc->bio_alloc_lock);

	return clone;
}

static void crypt_free_buffer_pages(struct crypt_config *cc, struct bio *clone)
{
	unsigned int i;
	struct bio_vec *bv;

	bio_for_each_segment_all(bv, clone, i) {
		BUG_ON(!bv->bv_page);
		mempool_free(bv->bv_page, cc->page_pool);
		bv->bv_page = NULL;
	}
}

static void crypt_io_init(struct dm_crypt_io *io, struct crypt_config *cc,
			  struct bio *bio, sector_t sector)
{
	io->cc = cc;
	io->base_bio = bio;
	io->sector = sector;
	io->error = 0;
	io->ctx.r.req = NULL;
	io->integrity_metadata = NULL;
	io->integrity_metadata_from_pool = false;
	atomic_set(&io->io_pending, 0);
}

static void crypt_inc_pending(struct dm_crypt_io *io)
{
	atomic_inc(&io->io_pending);
}

/*
 * One of the bios was finished. Check for completion of
 * the whole request and correctly clean up the buffer.
 */
static void crypt_dec_pending(struct dm_crypt_io *io)
{
	struct crypt_config *cc = io->cc;
	struct bio *base_bio = io->base_bio;
	blk_status_t error = io->error;

	if (!atomic_dec_and_test(&io->io_pending))
		return;

	if (io->ctx.r.req)
		crypt_free_req(cc, io->ctx.r.req, base_bio);

	if (unlikely(io->integrity_metadata_from_pool))
		mempool_free(io->integrity_metadata, io->cc->tag_pool);
	else
		kfree(io->integrity_metadata);

	base_bio->bi_status = error;
	bio_endio(base_bio);
}

/*
 * kcryptd/kcryptd_io:
 *
 * Needed because it would be very unwise to do decryption in an
 * interrupt context.
 *
 * kcryptd performs the actual encryption or decryption.
 *
 * kcryptd_io performs the IO submission.
 *
 * They must be separated as otherwise the final stages could be
 * starved by new requests which can block in the first stages due
 * to memory allocation.
 *
 * The work is done per CPU global for all dm-crypt instances.
 * They should not depend on each other and do not block.
 */
static void crypt_endio(struct bio *clone)
{
	struct dm_crypt_io *io = clone->bi_private;
	struct crypt_config *cc = io->cc;
	unsigned rw = bio_data_dir(clone);
	blk_status_t error;

	/*
	 * free the processed pages
	 */
	if ((rw == WRITE) && !crypt_mode_diskcipher(cc))
		crypt_free_buffer_pages(cc, clone);

	error = clone->bi_status;
	bio_put(clone);

	if (rw == READ && !error && !crypt_mode_diskcipher(cc)) {
		kcryptd_queue_crypt(io);
		return;
	}

	if (unlikely(error))
		io->error = error;

	crypt_dec_pending(io);
}

static void clone_init(struct dm_crypt_io *io, struct bio *clone)
{
	struct crypt_config *cc = io->cc;

	clone->bi_private = io;
	clone->bi_end_io  = crypt_endio;
	bio_set_dev(clone, cc->dev->bdev);
	clone->bi_opf	  = io->base_bio->bi_opf;
}

static int kcryptd_io_read(struct dm_crypt_io *io, gfp_t gfp)
{
	struct crypt_config *cc = io->cc;
	struct bio *clone;

	/*
	 * We need the original biovec array in order to decrypt
	 * the whole bio data *afterwards* -- thanks to immutable
	 * biovecs we don't need to worry about the block layer
	 * modifying the biovec array; so leverage bio_clone_fast().
	 */
	clone = bio_clone_fast(io->base_bio, gfp, cc->bs);
	if (!clone)
		return 1;

	crypt_inc_pending(io);

	clone_init(io, clone);

	if (crypt_mode_diskcipher(cc))
		crypto_diskcipher_set(clone, cc->cipher_tfm.tfms_diskc[0], 0);

	clone->bi_iter.bi_sector = cc->start + io->sector;

	if (dm_crypt_integrity_io_alloc(io, clone)) {
		crypt_dec_pending(io);
		bio_put(clone);
		return 1;
	}

	generic_make_request(clone);
	return 0;
}

static void kcryptd_io_read_work(struct work_struct *work)
{
	struct dm_crypt_io *io = container_of(work, struct dm_crypt_io, work);

	crypt_inc_pending(io);
	if (kcryptd_io_read(io, GFP_NOIO))
		io->error = BLK_STS_RESOURCE;
	crypt_dec_pending(io);
}

static void kcryptd_queue_read(struct dm_crypt_io *io)
{
	struct crypt_config *cc = io->cc;

	INIT_WORK(&io->work, kcryptd_io_read_work);
	queue_work(cc->io_queue, &io->work);
}

static void kcryptd_io_write(struct dm_crypt_io *io)
{
	struct bio *clone = io->ctx.bio_out;

	generic_make_request(clone);
}

#define crypt_io_from_node(node) rb_entry((node), struct dm_crypt_io, rb_node)

static int dmcrypt_write(void *data)
{
	struct crypt_config *cc = data;
	struct dm_crypt_io *io;

	while (1) {
		struct rb_root write_tree;
		struct blk_plug plug;

		DECLARE_WAITQUEUE(wait, current);

		spin_lock_irq(&cc->write_thread_wait.lock);
continue_locked:

		if (!RB_EMPTY_ROOT(&cc->write_tree))
			goto pop_from_list;

		set_current_state(TASK_INTERRUPTIBLE);
		__add_wait_queue(&cc->write_thread_wait, &wait);

		spin_unlock_irq(&cc->write_thread_wait.lock);

		if (unlikely(kthread_should_stop())) {
			set_current_state(TASK_RUNNING);
			remove_wait_queue(&cc->write_thread_wait, &wait);
			break;
		}

		schedule();

		set_current_state(TASK_RUNNING);
		spin_lock_irq(&cc->write_thread_wait.lock);
		__remove_wait_queue(&cc->write_thread_wait, &wait);
		goto continue_locked;

pop_from_list:
		write_tree = cc->write_tree;
		cc->write_tree = RB_ROOT;
		spin_unlock_irq(&cc->write_thread_wait.lock);

		BUG_ON(rb_parent(write_tree.rb_node));

		/*
		 * Note: we cannot walk the tree here with rb_next because
		 * the structures may be freed when kcryptd_io_write is called.
		 */
		blk_start_plug(&plug);
		do {
			io = crypt_io_from_node(rb_first(&write_tree));
			rb_erase(&io->rb_node, &write_tree);
			kcryptd_io_write(io);
		} while (!RB_EMPTY_ROOT(&write_tree));
		blk_finish_plug(&plug);
	}
	return 0;
}

static void kcryptd_crypt_write_io_submit(struct dm_crypt_io *io, int async)
{
	struct bio *clone = io->ctx.bio_out;
	struct crypt_config *cc = io->cc;
	unsigned long flags;
	sector_t sector;
	struct rb_node **rbp, *parent;

	if (unlikely(io->error)) {
		crypt_free_buffer_pages(cc, clone);
		bio_put(clone);
		crypt_dec_pending(io);
		return;
	}

	/* crypt_convert should have filled the clone bio */
	BUG_ON(io->ctx.iter_out.bi_size);

	clone->bi_iter.bi_sector = cc->start + io->sector;

	if (likely(!async) && test_bit(DM_CRYPT_NO_OFFLOAD, &cc->flags)) {
		generic_make_request(clone);
		return;
	}

	spin_lock_irqsave(&cc->write_thread_wait.lock, flags);
	rbp = &cc->write_tree.rb_node;
	parent = NULL;
	sector = io->sector;
	while (*rbp) {
		parent = *rbp;
		if (sector < crypt_io_from_node(parent)->sector)
			rbp = &(*rbp)->rb_left;
		else
			rbp = &(*rbp)->rb_right;
	}
	rb_link_node(&io->rb_node, parent, rbp);
	rb_insert_color(&io->rb_node, &cc->write_tree);

	wake_up_locked(&cc->write_thread_wait);
	spin_unlock_irqrestore(&cc->write_thread_wait.lock, flags);
}

static void kcryptd_crypt_write_convert(struct dm_crypt_io *io)
{
	struct crypt_config *cc = io->cc;
	struct bio *clone;
	int crypt_finished;
	sector_t sector = io->sector;
	blk_status_t r;

	/*
	 * Prevent io from disappearing until this function completes.
	 */
	crypt_inc_pending(io);
	crypt_convert_init(cc, &io->ctx, NULL, io->base_bio, sector);

	clone = crypt_alloc_buffer(io, io->base_bio->bi_iter.bi_size);
	if (unlikely(!clone)) {
		io->error = BLK_STS_IOERR;
		goto dec;
	}

	io->ctx.bio_out = clone;
	io->ctx.iter_out = clone->bi_iter;

	sector += bio_sectors(clone);

	crypt_inc_pending(io);
	r = crypt_convert(cc, &io->ctx);
	if (r)
		io->error = r;
	crypt_finished = atomic_dec_and_test(&io->ctx.cc_pending);

	/* Encryption was already finished, submit io now */
	if (crypt_finished) {
		kcryptd_crypt_write_io_submit(io, 0);
		io->sector = sector;
	}

dec:
	crypt_dec_pending(io);
}

static void kcryptd_crypt_read_done(struct dm_crypt_io *io)
{
	crypt_dec_pending(io);
}

static void kcryptd_crypt_read_convert(struct dm_crypt_io *io)
{
	struct crypt_config *cc = io->cc;
	blk_status_t r;

	crypt_inc_pending(io);

	crypt_convert_init(cc, &io->ctx, io->base_bio, io->base_bio,
			   io->sector);

	r = crypt_convert(cc, &io->ctx);
	if (r)
		io->error = r;

	if (atomic_dec_and_test(&io->ctx.cc_pending))
		kcryptd_crypt_read_done(io);

	crypt_dec_pending(io);
}

static void kcryptd_async_done(struct crypto_async_request *async_req,
			       int error)
{
	struct dm_crypt_request *dmreq = async_req->data;
	struct convert_context *ctx = dmreq->ctx;
	struct dm_crypt_io *io = container_of(ctx, struct dm_crypt_io, ctx);
	struct crypt_config *cc = io->cc;

	/*
	 * A request from crypto driver backlog is going to be processed now,
	 * finish the completion and continue in crypt_convert().
	 * (Callback will be called for the second time for this request.)
	 */
	if (error == -EINPROGRESS) {
		complete(&ctx->restart);
		return;
	}

	if (!error && cc->iv_gen_ops && cc->iv_gen_ops->post)
		error = cc->iv_gen_ops->post(cc, org_iv_of_dmreq(cc, dmreq), dmreq);

	if (error == -EBADMSG) {
		DMERR_LIMIT("INTEGRITY AEAD ERROR, sector %llu",
			    (unsigned long long)le64_to_cpu(*org_sector_of_dmreq(cc, dmreq)));
		io->error = BLK_STS_PROTECTION;
	} else if (error < 0)
		io->error = BLK_STS_IOERR;

	crypt_free_req(cc, req_of_dmreq(cc, dmreq), io->base_bio);

	if (!atomic_dec_and_test(&ctx->cc_pending))
		return;

	if (bio_data_dir(io->base_bio) == READ)
		kcryptd_crypt_read_done(io);
	else
		kcryptd_crypt_write_io_submit(io, 1);
}

static void kcryptd_crypt(struct work_struct *work)
{
	struct dm_crypt_io *io = container_of(work, struct dm_crypt_io, work);

	if (bio_data_dir(io->base_bio) == READ)
		kcryptd_crypt_read_convert(io);
	else
		kcryptd_crypt_write_convert(io);
}

static void kcryptd_queue_crypt(struct dm_crypt_io *io)
{
	struct crypt_config *cc = io->cc;

	INIT_WORK(&io->work, kcryptd_crypt);
	queue_work(cc->crypt_queue, &io->work);
}

static void crypt_free_tfms_aead(struct crypt_config *cc)
{
	if (!cc->cipher_tfm.tfms_aead)
		return;

	if (cc->cipher_tfm.tfms_aead[0] && !IS_ERR(cc->cipher_tfm.tfms_aead[0])) {
		crypto_free_aead(cc->cipher_tfm.tfms_aead[0]);
		cc->cipher_tfm.tfms_aead[0] = NULL;
	}

	kfree(cc->cipher_tfm.tfms_aead);
	cc->cipher_tfm.tfms_aead = NULL;
}

static void crypt_free_tfms_skcipher(struct crypt_config *cc)
{
	unsigned i;

	if (!cc->cipher_tfm.tfms)
		return;

	for (i = 0; i < cc->tfms_count; i++)
		if (cc->cipher_tfm.tfms[i] && !IS_ERR(cc->cipher_tfm.tfms[i])) {
			crypto_free_skcipher(cc->cipher_tfm.tfms[i]);
			cc->cipher_tfm.tfms[i] = NULL;
		}

	kfree(cc->cipher_tfm.tfms);
	cc->cipher_tfm.tfms = NULL;
}

static void crypt_free_tfms_diskcipher(struct crypt_config *cc)
{
	if (!cc->cipher_tfm.tfms_diskc)
		return;

	if (cc->cipher_tfm.tfms_diskc[0] && !IS_ERR(cc->cipher_tfm.tfms_diskc[0])) {
		crypto_diskcipher_clearkey(cc->cipher_tfm.tfms_diskc[0]);
		crypto_free_diskcipher(cc->cipher_tfm.tfms_diskc[0]);
		cc->cipher_tfm.tfms_diskc[0] = NULL;
	}

	kfree(cc->cipher_tfm.tfms_diskc);
	cc->cipher_tfm.tfms_diskc = NULL;
}


static void crypt_free_tfms(struct crypt_config *cc)
{
	if (crypt_integrity_aead(cc))
		crypt_free_tfms_aead(cc);
	else if (crypt_mode_diskcipher(cc))
		crypt_free_tfms_diskcipher(cc);
	else
		crypt_free_tfms_skcipher(cc);
}

static int crypt_alloc_tfms_skcipher(struct crypt_config *cc, char *ciphermode)
{
	unsigned i;
	int err;

	cc->cipher_tfm.tfms = kzalloc(cc->tfms_count *
				      sizeof(struct crypto_skcipher *), GFP_KERNEL);
	if (!cc->cipher_tfm.tfms)
		return -ENOMEM;

	for (i = 0; i < cc->tfms_count; i++) {
		cc->cipher_tfm.tfms[i] = crypto_alloc_skcipher(ciphermode, 0, 0);
		if (IS_ERR(cc->cipher_tfm.tfms[i])) {
			err = PTR_ERR(cc->cipher_tfm.tfms[i]);
			crypt_free_tfms(cc);
			return err;
		}
	}
	set_bit(CRYPT_MODE_SKCIPHER, &cc->cipher_flags);

	return 0;
}

static int crypt_alloc_tfms_aead(struct crypt_config *cc, char *ciphermode)
{
	int err;

	cc->cipher_tfm.tfms = kmalloc(sizeof(struct crypto_aead *), GFP_KERNEL);
	if (!cc->cipher_tfm.tfms)
		return -ENOMEM;

	cc->cipher_tfm.tfms_aead[0] = crypto_alloc_aead(ciphermode, 0, 0);
	if (IS_ERR(cc->cipher_tfm.tfms_aead[0])) {
		err = PTR_ERR(cc->cipher_tfm.tfms_aead[0]);
		crypt_free_tfms(cc);
		return err;
	}

	return 0;
}

static int crypt_alloc_tfms_diskcipher(struct crypt_config *cc, char *ciphermode)
{
	int err;

	cc->cipher_tfm.tfms = kmalloc(sizeof(struct crypto_aead *), GFP_KERNEL);
	if (!cc->cipher_tfm.tfms)
		return -ENOMEM;

	cc->cipher_tfm.tfms_diskc[0] = crypto_alloc_diskcipher(ciphermode, 0, 0, 1);
	if (IS_ERR(cc->cipher_tfm.tfms_diskc[0])) {
		err = PTR_ERR(cc->cipher_tfm.tfms_diskc[0]);
		crypt_free_tfms(cc);
		pr_err("%s: no diskcipher with %s\n", __func__, ciphermode);
		return err;
	}
	pr_info("%s is done with %s\n", __func__, ciphermode);

	return 0;
}

static int crypt_alloc_tfms(struct crypt_config *cc, char *ciphermode)
{
	if (crypt_integrity_aead(cc))
		return crypt_alloc_tfms_aead(cc, ciphermode);
	else if (crypt_mode_diskcipher(cc))
		return crypt_alloc_tfms_diskcipher(cc, ciphermode);
	else
		return crypt_alloc_tfms_skcipher(cc, ciphermode);
}

static unsigned crypt_subkey_size(struct crypt_config *cc)
{
	return (cc->key_size - cc->key_extra_size) >> ilog2(cc->tfms_count);
}

static unsigned crypt_authenckey_size(struct crypt_config *cc)
{
	return crypt_subkey_size(cc) + RTA_SPACE(sizeof(struct crypto_authenc_key_param));
}

/*
 * If AEAD is composed like authenc(hmac(sha256),xts(aes)),
 * the key must be for some reason in special format.
 * This funcion converts cc->key to this special format.
 */
static void crypt_copy_authenckey(char *p, const void *key,
				  unsigned enckeylen, unsigned authkeylen)
{
	struct crypto_authenc_key_param *param;
	struct rtattr *rta;

	rta = (struct rtattr *)p;
	param = RTA_DATA(rta);
	param->enckeylen = cpu_to_be32(enckeylen);
	rta->rta_len = RTA_LENGTH(sizeof(*param));
	rta->rta_type = CRYPTO_AUTHENC_KEYA_PARAM;
	p += RTA_SPACE(sizeof(*param));
	memcpy(p, key + enckeylen, authkeylen);
	p += authkeylen;
	memcpy(p, key, enckeylen);
}

static int crypt_setkey(struct crypt_config *cc)
{
	unsigned subkey_size;
	int err = 0, i, r;

	/* Ignore extra keys (which are used for IV etc) */
	subkey_size = crypt_subkey_size(cc);

	if (crypt_integrity_hmac(cc)) {
		if (subkey_size < cc->key_mac_size)
			return -EINVAL;

		crypt_copy_authenckey(cc->authenc_key, cc->key,
				      subkey_size - cc->key_mac_size,
				      cc->key_mac_size);
	}

	for (i = 0; i < cc->tfms_count; i++) {
		if (crypt_integrity_hmac(cc))
			r = crypto_aead_setkey(cc->cipher_tfm.tfms_aead[i],
				cc->authenc_key, crypt_authenckey_size(cc));
		else if (crypt_integrity_aead(cc))
			r = crypto_aead_setkey(cc->cipher_tfm.tfms_aead[i],
					       cc->key + (i * subkey_size),
					       subkey_size);
		else if (crypt_mode_diskcipher(cc))
			r = crypto_diskcipher_setkey(cc->cipher_tfm.tfms_diskc[i],
						   cc->key + (i * subkey_size),
						   subkey_size, 1);
		else
			r = crypto_skcipher_setkey(cc->cipher_tfm.tfms[i],
						   cc->key + (i * subkey_size),
						   subkey_size);
		if (r)
			err = r;
	}

	if (crypt_integrity_hmac(cc))
		memzero_explicit(cc->authenc_key, crypt_authenckey_size(cc));

	return err;
}

#ifdef CONFIG_KEYS

static bool contains_whitespace(const char *str)
{
	while (*str)
		if (isspace(*str++))
			return true;
	return false;
}

static int crypt_set_keyring_key(struct crypt_config *cc, const char *key_string)
{
	char *new_key_string, *key_desc;
	int ret;
	struct key *key;
	const struct user_key_payload *ukp;

	/*
	 * Reject key_string with whitespace. dm core currently lacks code for
	 * proper whitespace escaping in arguments on DM_TABLE_STATUS path.
	 */
	if (contains_whitespace(key_string)) {
		DMERR("whitespace chars not allowed in key string");
		return -EINVAL;
	}

	/* look for next ':' separating key_type from key_description */
	key_desc = strpbrk(key_string, ":");
	if (!key_desc || key_desc == key_string || !strlen(key_desc + 1))
		return -EINVAL;

	if (strncmp(key_string, "logon:", key_desc - key_string + 1) &&
	    strncmp(key_string, "user:", key_desc - key_string + 1))
		return -EINVAL;

	new_key_string = kstrdup(key_string, GFP_KERNEL);
	if (!new_key_string)
		return -ENOMEM;

	key = request_key(key_string[0] == 'l' ? &key_type_logon : &key_type_user,
			  key_desc + 1, NULL);
	if (IS_ERR(key)) {
		kzfree(new_key_string);
		return PTR_ERR(key);
	}

	down_read(&key->sem);

	ukp = user_key_payload_locked(key);
	if (!ukp) {
		up_read(&key->sem);
		key_put(key);
		kzfree(new_key_string);
		return -EKEYREVOKED;
	}

	if (cc->key_size != ukp->datalen) {
		up_read(&key->sem);
		key_put(key);
		kzfree(new_key_string);
		return -EINVAL;
	}

	memcpy(cc->key, ukp->data, cc->key_size);

	up_read(&key->sem);
	key_put(key);

	/* clear the flag since following operations may invalidate previously valid key */
	clear_bit(DM_CRYPT_KEY_VALID, &cc->flags);

	ret = crypt_setkey(cc);

	if (!ret) {
		set_bit(DM_CRYPT_KEY_VALID, &cc->flags);
		kzfree(cc->key_string);
		cc->key_string = new_key_string;
	} else
		kzfree(new_key_string);

	return ret;
}

static int get_key_size(char **key_string)
{
	char *colon, dummy;
	int ret;

	if (*key_string[0] != ':')
		return strlen(*key_string) >> 1;

	/* look for next ':' in key string */
	colon = strpbrk(*key_string + 1, ":");
	if (!colon)
		return -EINVAL;

	if (sscanf(*key_string + 1, "%u%c", &ret, &dummy) != 2 || dummy != ':')
		return -EINVAL;

	*key_string = colon;

	/* remaining key string should be :<logon|user>:<key_desc> */

	return ret;
}

#else

static int crypt_set_keyring_key(struct crypt_config *cc, const char *key_string)
{
	return -EINVAL;
}

static int get_key_size(char **key_string)
{
	return (*key_string[0] == ':') ? -EINVAL : strlen(*key_string) >> 1;
}

#endif

static int crypt_set_key(struct crypt_config *cc, char *key)
{
	int r = -EINVAL;
	int key_string_len = strlen(key);

	/* Hyphen (which gives a key_size of zero) means there is no key. */
	if (!cc->key_size && strcmp(key, "-"))
		goto out;

	/* ':' means the key is in kernel keyring, short-circuit normal key processing */
	if (key[0] == ':') {
		r = crypt_set_keyring_key(cc, key + 1);
		goto out;
	}

	/* clear the flag since following operations may invalidate previously valid key */
	clear_bit(DM_CRYPT_KEY_VALID, &cc->flags);

	/* wipe references to any kernel keyring key */
	kzfree(cc->key_string);
	cc->key_string = NULL;

	/* Decode key from its hex representation. */
	if (cc->key_size && hex2bin(cc->key, key, cc->key_size) < 0)
		goto out;

	r = crypt_setkey(cc);
	if (!r)
		set_bit(DM_CRYPT_KEY_VALID, &cc->flags);

out:
	/* Hex key string not needed after here, so wipe it. */
	memset(key, '0', key_string_len);

	return r;
}

static int crypt_wipe_key(struct crypt_config *cc)
{
	int r;

	clear_bit(DM_CRYPT_KEY_VALID, &cc->flags);
	get_random_bytes(&cc->key, cc->key_size);
	kzfree(cc->key_string);
	cc->key_string = NULL;
	r = crypt_setkey(cc);
	memset(&cc->key, 0, cc->key_size * sizeof(u8));

	return r;
}

static void crypt_calculate_pages_per_client(void)
{
	unsigned long pages = (totalram_pages - totalhigh_pages) * DM_CRYPT_MEMORY_PERCENT / 100;

	if (!dm_crypt_clients_n)
		return;

	pages /= dm_crypt_clients_n;
	if (pages < DM_CRYPT_MIN_PAGES_PER_CLIENT)
		pages = DM_CRYPT_MIN_PAGES_PER_CLIENT;
	dm_crypt_pages_per_client = pages;
}

static void *crypt_page_alloc(gfp_t gfp_mask, void *pool_data)
{
	struct crypt_config *cc = pool_data;
	struct page *page;

	if (unlikely(percpu_counter_compare(&cc->n_allocated_pages, dm_crypt_pages_per_client) >= 0) &&
	    likely(gfp_mask & __GFP_NORETRY))
		return NULL;

	page = alloc_page(gfp_mask);
	if (likely(page != NULL))
		percpu_counter_add(&cc->n_allocated_pages, 1);

	return page;
}

static void crypt_page_free(void *page, void *pool_data)
{
	struct crypt_config *cc = pool_data;

	__free_page(page);
	percpu_counter_sub(&cc->n_allocated_pages, 1);
}

static void crypt_dtr(struct dm_target *ti)
{
	struct crypt_config *cc = ti->private;

	ti->private = NULL;

	if (!cc)
		return;

	if (cc->write_thread)
		kthread_stop(cc->write_thread);

	if (cc->io_queue)
		destroy_workqueue(cc->io_queue);
	if (cc->crypt_queue)
		destroy_workqueue(cc->crypt_queue);

	crypt_free_tfms(cc);

	if (cc->bs)
		bioset_free(cc->bs);

	mempool_destroy(cc->page_pool);
	mempool_destroy(cc->req_pool);
	mempool_destroy(cc->tag_pool);

	if (cc->page_pool)
		WARN_ON(percpu_counter_sum(&cc->n_allocated_pages) != 0);
	percpu_counter_destroy(&cc->n_allocated_pages);

	if (cc->iv_gen_ops && cc->iv_gen_ops->dtr)
		cc->iv_gen_ops->dtr(cc);

	if (cc->dev)
		dm_put_device(ti, cc->dev);

	kzfree(cc->cipher);
	kzfree(cc->cipher_string);
	kzfree(cc->key_string);
	kzfree(cc->cipher_auth);
	kzfree(cc->authenc_key);

	/* Must zero key material before freeing */
	kzfree(cc);

	spin_lock(&dm_crypt_clients_lock);
	WARN_ON(!dm_crypt_clients_n);
	dm_crypt_clients_n--;
	crypt_calculate_pages_per_client();
	spin_unlock(&dm_crypt_clients_lock);
}

static int crypt_ctr_ivmode(struct dm_target *ti, const char *ivmode)
{
	struct crypt_config *cc = ti->private;

	if (crypt_integrity_aead(cc))
		cc->iv_size = crypto_aead_ivsize(any_tfm_aead(cc));
	else
		cc->iv_size = crypto_skcipher_ivsize(any_tfm(cc));

	if (cc->iv_size)
		/* at least a 64 bit sector number should fit in our buffer */
		cc->iv_size = max(cc->iv_size,
				  (unsigned int)(sizeof(u64) / sizeof(u8)));
	else if (ivmode) {
		DMWARN("Selected cipher does not support IVs");
		ivmode = NULL;
	}

	/* Choose ivmode, see comments at iv code. */
	if (ivmode == NULL)
		cc->iv_gen_ops = NULL;
	else if (strcmp(ivmode, "plain") == 0)
		cc->iv_gen_ops = &crypt_iv_plain_ops;
	else if (strcmp(ivmode, "plain64") == 0)
		cc->iv_gen_ops = &crypt_iv_plain64_ops;
	else if (strcmp(ivmode, "plain64be") == 0)
		cc->iv_gen_ops = &crypt_iv_plain64be_ops;
	else if (strcmp(ivmode, "essiv") == 0)
		cc->iv_gen_ops = &crypt_iv_essiv_ops;
	else if (strcmp(ivmode, "benbi") == 0)
		cc->iv_gen_ops = &crypt_iv_benbi_ops;
	else if (strcmp(ivmode, "null") == 0)
		cc->iv_gen_ops = &crypt_iv_null_ops;
	else if (strcmp(ivmode, "lmk") == 0) {
		cc->iv_gen_ops = &crypt_iv_lmk_ops;
		/*
		 * Version 2 and 3 is recognised according
		 * to length of provided multi-key string.
		 * If present (version 3), last key is used as IV seed.
		 * All keys (including IV seed) are always the same size.
		 */
		if (cc->key_size % cc->key_parts) {
			cc->key_parts++;
			cc->key_extra_size = cc->key_size / cc->key_parts;
		}
	} else if (strcmp(ivmode, "tcw") == 0) {
		cc->iv_gen_ops = &crypt_iv_tcw_ops;
		cc->key_parts += 2; /* IV + whitening */
		cc->key_extra_size = cc->iv_size + TCW_WHITENING_SIZE;
	} else if (strcmp(ivmode, "random") == 0) {
		cc->iv_gen_ops = &crypt_iv_random_ops;
		/* Need storage space in integrity fields. */
		cc->integrity_iv_size = cc->iv_size;
	} else {
		ti->error = "Invalid IV mode";
		return -EINVAL;
	}

	return 0;
}

/*
 * Workaround to parse cipher algorithm from crypto API spec.
 * The cc->cipher is currently used only in ESSIV.
 * This should be probably done by crypto-api calls (once available...)
 */
static int crypt_ctr_blkdev_cipher(struct crypt_config *cc)
{
	const char *alg_name = NULL;
	char *start, *end;

	if (crypt_integrity_aead(cc)) {
		alg_name = crypto_tfm_alg_name(crypto_aead_tfm(any_tfm_aead(cc)));
		if (!alg_name)
			return -EINVAL;
		if (crypt_integrity_hmac(cc)) {
			alg_name = strchr(alg_name, ',');
			if (!alg_name)
				return -EINVAL;
		}
		alg_name++;
	} else {
		alg_name = crypto_tfm_alg_name(crypto_skcipher_tfm(any_tfm(cc)));
		if (!alg_name)
			return -EINVAL;
	}

	start = strchr(alg_name, '(');
	end = strchr(alg_name, ')');

	if (!start && !end) {
		cc->cipher = kstrdup(alg_name, GFP_KERNEL);
		return cc->cipher ? 0 : -ENOMEM;
	}

	if (!start || !end || ++start >= end)
		return -EINVAL;

	cc->cipher = kzalloc(end - start + 1, GFP_KERNEL);
	if (!cc->cipher)
		return -ENOMEM;

	strncpy(cc->cipher, start, end - start);

	return 0;
}

/*
 * Workaround to parse HMAC algorithm from AEAD crypto API spec.
 * The HMAC is needed to calculate tag size (HMAC digest size).
 * This should be probably done by crypto-api calls (once available...)
 */
static int crypt_ctr_auth_cipher(struct crypt_config *cc, char *cipher_api)
{
	char *start, *end, *mac_alg = NULL;
	struct crypto_ahash *mac;

	if (!strstarts(cipher_api, "authenc("))
		return 0;

	start = strchr(cipher_api, '(');
	end = strchr(cipher_api, ',');
	if (!start || !end || ++start > end)
		return -EINVAL;

	mac_alg = kzalloc(end - start + 1, GFP_KERNEL);
	if (!mac_alg)
		return -ENOMEM;
	strncpy(mac_alg, start, end - start);

	mac = crypto_alloc_ahash(mac_alg, 0, 0);
	kfree(mac_alg);

	if (IS_ERR(mac))
		return PTR_ERR(mac);

	cc->key_mac_size = crypto_ahash_digestsize(mac);
	crypto_free_ahash(mac);

	cc->authenc_key = kmalloc(crypt_authenckey_size(cc), GFP_KERNEL);
	if (!cc->authenc_key)
		return -ENOMEM;

	return 0;
}

static int crypt_ctr_cipher_new(struct dm_target *ti, char *cipher_in, char *key,
				char **ivmode, char **ivopts)
{
	struct crypt_config *cc = ti->private;
	char *tmp, *cipher_api;
	int ret = -EINVAL;

	cc->tfms_count = 1;

	/*
	 * New format (capi: prefix)
	 * capi:cipher_api_spec-iv:ivopts
	 */
	tmp = &cipher_in[strlen("capi:")];
	cipher_api = strsep(&tmp, "-");
	*ivmode = strsep(&tmp, ":");
	*ivopts = tmp;

	if (*ivmode && !strcmp(*ivmode, "lmk"))
		cc->tfms_count = 64;

	cc->key_parts = cc->tfms_count;

	/* Allocate cipher */
	ret = crypt_alloc_tfms(cc, cipher_api);
	if (ret < 0) {
		ti->error = "Error allocating crypto tfm";
		return ret;
	}

	/* Alloc AEAD, can be used only in new format. */
	if (crypt_integrity_aead(cc)) {
		ret = crypt_ctr_auth_cipher(cc, cipher_api);
		if (ret < 0) {
			ti->error = "Invalid AEAD cipher spec";
			return -ENOMEM;
		}
		cc->iv_size = crypto_aead_ivsize(any_tfm_aead(cc));
	} else if (crypt_mode_skcipher(cc))
		cc->iv_size = crypto_skcipher_ivsize(any_tfm(cc));

	ret = crypt_ctr_blkdev_cipher(cc);
	if (ret < 0) {
		ti->error = "Cannot allocate cipher string";
		return -ENOMEM;
	}

	return 0;
}

static int crypt_ctr_cipher_old(struct dm_target *ti, char *cipher_in, char *key,
				char **ivmode, char **ivopts)
{
	struct crypt_config *cc = ti->private;
	char *tmp, *cipher, *chainmode, *keycount;
	char *cipher_api = NULL;
	int ret = -EINVAL;
	char dummy;

	if (strchr(cipher_in, '(') || crypt_integrity_aead(cc)) {
		ti->error = "Bad cipher specification";
		return -EINVAL;
	}

	/*
	 * Legacy dm-crypt cipher specification
	 * cipher[:keycount]-mode-iv:ivopts
	 */
	tmp = cipher_in;
	keycount = strsep(&tmp, "-");
	cipher = strsep(&keycount, ":");

	if (!keycount)
		cc->tfms_count = 1;
	else if (sscanf(keycount, "%u%c", &cc->tfms_count, &dummy) != 1 ||
		 !is_power_of_2(cc->tfms_count)) {
		ti->error = "Bad cipher key count specification";
		return -EINVAL;
	}
	cc->key_parts = cc->tfms_count;

	cc->cipher = kstrdup(cipher, GFP_KERNEL);
	if (!cc->cipher)
		goto bad_mem;

	chainmode = strsep(&tmp, "-");
	*ivopts = strsep(&tmp, "-");
	*ivmode = strsep(&*ivopts, ":");

	if (*ivmode && (!strcmp(*ivmode, "disk") || !strcmp(*ivmode, "fmp")))
		set_bit(CRYPT_MODE_DISKCIPHER, &cc->cipher_flags);

	if (tmp)
		DMWARN("Ignoring unexpected additional cipher options");

	/*
	 * For compatibility with the original dm-crypt mapping format, if
	 * only the cipher name is supplied, use cbc-plain.
	 */
	if (!chainmode || (!strcmp(chainmode, "plain") && !*ivmode)) {
		chainmode = "cbc";
		*ivmode = "plain";
	}

	if (strcmp(chainmode, "ecb") && !*ivmode) {
		ti->error = "IV mechanism required";
		return -EINVAL;
	}

	cipher_api = kmalloc(CRYPTO_MAX_ALG_NAME, GFP_KERNEL);
	if (!cipher_api)
		goto bad_mem;

	ret = snprintf(cipher_api, CRYPTO_MAX_ALG_NAME,
		       "%s(%s)", chainmode, cipher);
	if (ret < 0) {
		kfree(cipher_api);
		goto bad_mem;
	}

	/* Allocate cipher */
	ret = crypt_alloc_tfms(cc, cipher_api);
	if (ret < 0) {
		ti->error = "Error allocating crypto tfm";
		kfree(cipher_api);
		return ret;
	}
	kfree(cipher_api);

	return 0;
bad_mem:
	ti->error = "Cannot allocate cipher strings";
	return -ENOMEM;
}

static int crypt_ctr_cipher(struct dm_target *ti, char *cipher_in, char *key)
{
	struct crypt_config *cc = ti->private;
	char *ivmode = NULL, *ivopts = NULL;
	int ret;

	cc->cipher_string = kstrdup(cipher_in, GFP_KERNEL);
	if (!cc->cipher_string) {
		ti->error = "Cannot allocate cipher strings";
		return -ENOMEM;
	}

	if (strstarts(cipher_in, "capi:"))
		ret = crypt_ctr_cipher_new(ti, cipher_in, key, &ivmode, &ivopts);
	else
		ret = crypt_ctr_cipher_old(ti, cipher_in, key, &ivmode, &ivopts);
	if (ret)
		return ret;

	/* Initialize IV */
	if (!crypt_mode_diskcipher(cc)) {
		ret = crypt_ctr_ivmode(ti, ivmode);
		if (ret < 0)
			return ret;
	}

	/* Initialize and set key */
	ret = crypt_set_key(cc, key);
	if (ret < 0) {
		ti->error = "Error decoding and setting key";
		return ret;
	}

	/* Allocate IV */
	if (cc->iv_gen_ops && cc->iv_gen_ops->ctr) {
		ret = cc->iv_gen_ops->ctr(cc, ti, ivopts);
		if (ret < 0) {
			ti->error = "Error creating IV";
			return ret;
		}
	}

	/* Initialize IV (set keys for ESSIV etc) */
	if (cc->iv_gen_ops && cc->iv_gen_ops->init) {
		ret = cc->iv_gen_ops->init(cc);
		if (ret < 0) {
			ti->error = "Error initialising IV";
			return ret;
		}
	}

	/* wipe the kernel key payload copy */
	if (cc->key_string)
		memset(cc->key, 0, cc->key_size * sizeof(u8));

	pr_info("%s with ivmode:%s, aead:%d, diskcipher:%d(%p), skcipher:%d\n",
			__func__, ivmode, crypt_integrity_aead(cc),
			crypt_mode_diskcipher(cc), cc->cipher_tfm.tfms_diskc[0],
			crypt_mode_skcipher(cc));

	return ret;
}

static int crypt_ctr_optional(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct crypt_config *cc = ti->private;
	struct dm_arg_set as;
	static const struct dm_arg _args[] = {
		{0, 6, "Invalid number of feature args"},
	};
	unsigned int opt_params, val;
	const char *opt_string, *sval;
	char dummy;
	int ret;

	/* Optional parameters */
	as.argc = argc;
	as.argv = argv;

	ret = dm_read_arg_group(_args, &as, &opt_params, &ti->error);
	if (ret)
		return ret;

	while (opt_params--) {
		opt_string = dm_shift_arg(&as);
		if (!opt_string) {
			ti->error = "Not enough feature arguments";
			return -EINVAL;
		}

		if (!strcasecmp(opt_string, "allow_discards"))
			ti->num_discard_bios = 1;

		else if (!strcasecmp(opt_string, "same_cpu_crypt"))
			set_bit(DM_CRYPT_SAME_CPU, &cc->flags);

		else if (!strcasecmp(opt_string, "submit_from_crypt_cpus"))
			set_bit(DM_CRYPT_NO_OFFLOAD, &cc->flags);
		else if (sscanf(opt_string, "integrity:%u:", &val) == 1) {
			if (val == 0 || val > MAX_TAG_SIZE) {
				ti->error = "Invalid integrity arguments";
				return -EINVAL;
			}
			cc->on_disk_tag_size = val;
			sval = strchr(opt_string + strlen("integrity:"), ':') + 1;
			if (!strcasecmp(sval, "aead")) {
				set_bit(CRYPT_MODE_INTEGRITY_AEAD, &cc->cipher_flags);
			} else  if (strcasecmp(sval, "none")) {
				ti->error = "Unknown integrity profile";
				return -EINVAL;
			}

			cc->cipher_auth = kstrdup(sval, GFP_KERNEL);
			if (!cc->cipher_auth)
				return -ENOMEM;
		} else if (sscanf(opt_string, "sector_size:%hu%c", &cc->sector_size, &dummy) == 1) {
			if (cc->sector_size < (1 << SECTOR_SHIFT) ||
			    cc->sector_size > 4096 ||
			    (cc->sector_size & (cc->sector_size - 1))) {
				ti->error = "Invalid feature value for sector_size";
				return -EINVAL;
			}
			if (ti->len & ((cc->sector_size >> SECTOR_SHIFT) - 1)) {
				ti->error = "Device size is not multiple of sector_size feature";
				return -EINVAL;
			}
			cc->sector_shift = __ffs(cc->sector_size) - SECTOR_SHIFT;
		} else if (!strcasecmp(opt_string, "iv_large_sectors"))
			set_bit(CRYPT_IV_LARGE_SECTORS, &cc->cipher_flags);
		else {
			ti->error = "Invalid feature arguments";
			return -EINVAL;
		}
	}

	return 0;
}

/*
 * Construct an encryption mapping:
 * <cipher> [<key>|:<key_size>:<user|logon>:<key_description>] <iv_offset> <dev_path> <start>
 */
static int crypt_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct crypt_config *cc;
	int key_size;
	unsigned int align_mask;
	unsigned long long tmpll;
	int ret;
	size_t iv_size_padding, additional_req_size;
	char dummy;

	if (argc < 5) {
		ti->error = "Not enough arguments";
		return -EINVAL;
	}

	key_size = get_key_size(&argv[1]);
	if (key_size < 0) {
		ti->error = "Cannot parse key size";
		return -EINVAL;
	}

	cc = kzalloc(sizeof(*cc) + key_size * sizeof(u8), GFP_KERNEL);
	if (!cc) {
		ti->error = "Cannot allocate encryption context";
		return -ENOMEM;
	}
	cc->key_size = key_size;
	cc->sector_size = (1 << SECTOR_SHIFT);
	cc->sector_shift = 0;

	ti->private = cc;

	spin_lock(&dm_crypt_clients_lock);
	dm_crypt_clients_n++;
	crypt_calculate_pages_per_client();
	spin_unlock(&dm_crypt_clients_lock);

	ret = percpu_counter_init(&cc->n_allocated_pages, 0, GFP_KERNEL);
	if (ret < 0)
		goto bad;

	/* Optional parameters need to be read before cipher constructor */
	if (argc > 5) {
		ret = crypt_ctr_optional(ti, argc - 5, &argv[5]);
		if (ret)
			goto bad;
	}

	ret = crypt_ctr_cipher(ti, argv[0], argv[1]);
	if (ret < 0)
		goto bad;
	if (crypt_integrity_aead(cc)) {
		cc->dmreq_start = sizeof(struct aead_request);
		cc->dmreq_start += crypto_aead_reqsize(any_tfm_aead(cc));
		align_mask = crypto_aead_alignmask(any_tfm_aead(cc));
	} else if (crypt_mode_diskcipher(cc)) {
		cc->per_bio_data_size = ti->per_io_data_size =
			ALIGN(sizeof(struct dm_crypt_io), ARCH_KMALLOC_MINALIGN);
		goto get_bio;
	} else {
		cc->dmreq_start = sizeof(struct skcipher_request);
		cc->dmreq_start += crypto_skcipher_reqsize(any_tfm(cc));
		align_mask = crypto_skcipher_alignmask(any_tfm(cc));
	}
	cc->dmreq_start = ALIGN(cc->dmreq_start, __alignof__(struct dm_crypt_request));

	if (align_mask < CRYPTO_MINALIGN) {
		/* Allocate the padding exactly */
		iv_size_padding = -(cc->dmreq_start + sizeof(struct dm_crypt_request))
				& align_mask;
	} else {
		/*
		 * If the cipher requires greater alignment than kmalloc
		 * alignment, we don't know the exact position of the
		 * initialization vector. We must assume worst case.
		 */
		iv_size_padding = align_mask;
	}

	ret = -ENOMEM;

	/*  ...| IV + padding | original IV | original sec. number | bio tag offset | */
	additional_req_size = sizeof(struct dm_crypt_request) +
		iv_size_padding + cc->iv_size +
		cc->iv_size +
		sizeof(uint64_t) +
		sizeof(unsigned int);

	cc->req_pool = mempool_create_kmalloc_pool(MIN_IOS, cc->dmreq_start + additional_req_size);
	if (!cc->req_pool) {
		ti->error = "Cannot allocate crypt request mempool";
		goto bad;
	}

	cc->per_bio_data_size = ti->per_io_data_size =
		ALIGN(sizeof(struct dm_crypt_io) + cc->dmreq_start + additional_req_size,
		      ARCH_KMALLOC_MINALIGN);

	cc->page_pool = mempool_create(BIO_MAX_PAGES, crypt_page_alloc, crypt_page_free, cc);
	if (!cc->page_pool) {
		ti->error = "Cannot allocate page mempool";
		goto bad;
	}

get_bio:
	cc->bs = bioset_create(MIN_IOS, 0, (BIOSET_NEED_BVECS |
					    BIOSET_NEED_RESCUER));
	if (!cc->bs) {
		ti->error = "Cannot allocate crypt bioset";
		goto bad;
	}

	mutex_init(&cc->bio_alloc_lock);

	ret = -EINVAL;
	if ((sscanf(argv[2], "%llu%c", &tmpll, &dummy) != 1) ||
	    (tmpll & ((cc->sector_size >> SECTOR_SHIFT) - 1))) {
		ti->error = "Invalid iv_offset sector";
		goto bad;
	}
	cc->iv_offset = tmpll;

	ret = dm_get_device(ti, argv[3], dm_table_get_mode(ti->table), &cc->dev);
	if (ret) {
		ti->error = "Device lookup failed";
		goto bad;
	}

	ret = -EINVAL;
	if (sscanf(argv[4], "%llu%c", &tmpll, &dummy) != 1) {
		ti->error = "Invalid device sector";
		goto bad;
	}
	cc->start = tmpll;

	if (crypt_integrity_aead(cc) || cc->integrity_iv_size) {
		ret = crypt_integrity_ctr(cc, ti);
		if (ret)
			goto bad;

		cc->tag_pool_max_sectors = POOL_ENTRY_SIZE / cc->on_disk_tag_size;
		if (!cc->tag_pool_max_sectors)
			cc->tag_pool_max_sectors = 1;

		cc->tag_pool = mempool_create_kmalloc_pool(MIN_IOS,
			cc->tag_pool_max_sectors * cc->on_disk_tag_size);
		if (!cc->tag_pool) {
			ti->error = "Cannot allocate integrity tags mempool";
			ret = -ENOMEM;
			goto bad;
		}

		cc->tag_pool_max_sectors <<= cc->sector_shift;
	}

	ret = -ENOMEM;
	cc->io_queue = alloc_workqueue("kcryptd_io", WQ_HIGHPRI | WQ_CPU_INTENSIVE | WQ_MEM_RECLAIM, 1);
	if (!cc->io_queue) {
		ti->error = "Couldn't create kcryptd io queue";
		goto bad;
	}

	if (crypt_mode_diskcipher(cc)) {
		cc->crypt_queue = NULL;
		cc->write_thread = NULL;
		goto out;
	}

	if (test_bit(DM_CRYPT_SAME_CPU, &cc->flags))
		cc->crypt_queue = alloc_workqueue("kcryptd", WQ_HIGHPRI | WQ_CPU_INTENSIVE | WQ_MEM_RECLAIM, 1);
	else
		cc->crypt_queue = alloc_workqueue("kcryptd",
						  WQ_HIGHPRI | WQ_CPU_INTENSIVE | WQ_MEM_RECLAIM | WQ_UNBOUND,
						  num_online_cpus());
	if (!cc->crypt_queue) {
		ti->error = "Couldn't create kcryptd queue";
		goto bad;
	}

	init_waitqueue_head(&cc->write_thread_wait);
	cc->write_tree = RB_ROOT;

	cc->write_thread = kthread_create(dmcrypt_write, cc, "dmcrypt_write");
	if (IS_ERR(cc->write_thread)) {
		ret = PTR_ERR(cc->write_thread);
		cc->write_thread = NULL;
		ti->error = "Couldn't spawn write thread";
		goto bad;
	}
	wake_up_process(cc->write_thread);

out:
	ti->num_flush_bios = 1;

	return 0;

bad:
	crypt_dtr(ti);
	return ret;
}

static int crypt_map(struct dm_target *ti, struct bio *bio)
{
	struct dm_crypt_io *io;
	struct crypt_config *cc = ti->private;

	/*
	 * If bio is REQ_PREFLUSH or REQ_OP_DISCARD, just bypass crypt queues.
	 * - for REQ_PREFLUSH device-mapper core ensures that no IO is in-flight
	 * - for REQ_OP_DISCARD caller must use flush if IO ordering matters
	 */
	if (unlikely(bio->bi_opf & REQ_PREFLUSH ||
	    bio_op(bio) == REQ_OP_DISCARD)) {
		bio_set_dev(bio, cc->dev->bdev);
		if (bio_sectors(bio))
			bio->bi_iter.bi_sector = cc->start +
				dm_target_offset(ti, bio->bi_iter.bi_sector);
		return DM_MAPIO_REMAPPED;
	}

	/*
	 * Check if bio is too large, split as needed.
	 */
	if (unlikely(bio->bi_iter.bi_size > (BIO_MAX_PAGES << PAGE_SHIFT)) &&
	    (bio_data_dir(bio) == WRITE || cc->on_disk_tag_size))
		dm_accept_partial_bio(bio, ((BIO_MAX_PAGES << PAGE_SHIFT) >> SECTOR_SHIFT));

	/*
	 * Ensure that bio is a multiple of internal sector encryption size
	 * and is aligned to this size as defined in IO hints.
	 */
	if (unlikely((bio->bi_iter.bi_sector & ((cc->sector_size >> SECTOR_SHIFT) - 1)) != 0))
		return DM_MAPIO_KILL;

	if (unlikely(bio->bi_iter.bi_size & (cc->sector_size - 1)))
		return DM_MAPIO_KILL;

	io = dm_per_bio_data(bio, cc->per_bio_data_size);
	crypt_io_init(io, cc, bio, dm_target_offset(ti, bio->bi_iter.bi_sector));

	if (cc->on_disk_tag_size) {
		unsigned tag_len = cc->on_disk_tag_size * (bio_sectors(bio) >> cc->sector_shift);

		if (unlikely(tag_len > KMALLOC_MAX_SIZE) ||
		    unlikely(!(io->integrity_metadata = kmalloc(tag_len,
				GFP_NOIO | __GFP_NORETRY | __GFP_NOMEMALLOC | __GFP_NOWARN)))) {
			if (bio_sectors(bio) > cc->tag_pool_max_sectors)
				dm_accept_partial_bio(bio, cc->tag_pool_max_sectors);
			io->integrity_metadata = mempool_alloc(cc->tag_pool, GFP_NOIO);
			io->integrity_metadata_from_pool = true;
		}
	}

	if (crypt_integrity_aead(cc))
		io->ctx.r.req_aead = (struct aead_request *)(io + 1);
	else if (crypt_mode_skcipher(cc))
		io->ctx.r.req = (struct skcipher_request *)(io + 1);

	if ((bio_data_dir(io->base_bio) == READ) || crypt_mode_diskcipher(cc)) {
		if (kcryptd_io_read(io, GFP_NOWAIT))
			kcryptd_queue_read(io);
	} else
		kcryptd_queue_crypt(io);

	return DM_MAPIO_SUBMITTED;
}

static void crypt_status(struct dm_target *ti, status_type_t type,
			 unsigned status_flags, char *result, unsigned maxlen)
{
	struct crypt_config *cc = ti->private;
	unsigned i, sz = 0;
	int num_feature_args = 0;

	switch (type) {
	case STATUSTYPE_INFO:
		result[0] = '\0';
		break;

	case STATUSTYPE_TABLE:
		DMEMIT("%s ", cc->cipher_string);

		if (cc->key_size > 0) {
			if (cc->key_string)
				DMEMIT(":%u:%s", cc->key_size, cc->key_string);
			else
				for (i = 0; i < cc->key_size; i++)
					DMEMIT("%02x", cc->key[i]);
		} else
			DMEMIT("-");

		DMEMIT(" %llu %s %llu", (unsigned long long)cc->iv_offset,
				cc->dev->name, (unsigned long long)cc->start);

		num_feature_args += !!ti->num_discard_bios;
		num_feature_args += test_bit(DM_CRYPT_SAME_CPU, &cc->flags);
		num_feature_args += test_bit(DM_CRYPT_NO_OFFLOAD, &cc->flags);
		num_feature_args += cc->sector_size != (1 << SECTOR_SHIFT);
		num_feature_args += test_bit(CRYPT_IV_LARGE_SECTORS, &cc->cipher_flags);
		if (cc->on_disk_tag_size)
			num_feature_args++;
		if (num_feature_args) {
			DMEMIT(" %d", num_feature_args);
			if (ti->num_discard_bios)
				DMEMIT(" allow_discards");
			if (test_bit(DM_CRYPT_SAME_CPU, &cc->flags))
				DMEMIT(" same_cpu_crypt");
			if (test_bit(DM_CRYPT_NO_OFFLOAD, &cc->flags))
				DMEMIT(" submit_from_crypt_cpus");
			if (cc->on_disk_tag_size)
				DMEMIT(" integrity:%u:%s", cc->on_disk_tag_size, cc->cipher_auth);
			if (cc->sector_size != (1 << SECTOR_SHIFT))
				DMEMIT(" sector_size:%d", cc->sector_size);
			if (test_bit(CRYPT_IV_LARGE_SECTORS, &cc->cipher_flags))
				DMEMIT(" iv_large_sectors");
		}

		break;
	}
}

static void crypt_postsuspend(struct dm_target *ti)
{
	struct crypt_config *cc = ti->private;

	set_bit(DM_CRYPT_SUSPENDED, &cc->flags);
}

static int crypt_preresume(struct dm_target *ti)
{
	struct crypt_config *cc = ti->private;

	if (!test_bit(DM_CRYPT_KEY_VALID, &cc->flags)) {
		DMERR("aborting resume - crypt key is not set.");
		return -EAGAIN;
	}

	return 0;
}

static void crypt_resume(struct dm_target *ti)
{
	struct crypt_config *cc = ti->private;

	clear_bit(DM_CRYPT_SUSPENDED, &cc->flags);
}

/* Message interface
 *	key set <key>
 *	key wipe
 */
static int crypt_message(struct dm_target *ti, unsigned argc, char **argv)
{
	struct crypt_config *cc = ti->private;
	int key_size, ret = -EINVAL;

	if (argc < 2)
		goto error;

	if (!strcasecmp(argv[0], "key")) {
		if (!test_bit(DM_CRYPT_SUSPENDED, &cc->flags)) {
			DMWARN("not suspended during key manipulation.");
			return -EINVAL;
		}
		if (argc == 3 && !strcasecmp(argv[1], "set")) {
			/* The key size may not be changed. */
			key_size = get_key_size(&argv[2]);
			if (key_size < 0 || cc->key_size != key_size) {
				memset(argv[2], '0', strlen(argv[2]));
				return -EINVAL;
			}

			ret = crypt_set_key(cc, argv[2]);
			if (ret)
				return ret;
			if (cc->iv_gen_ops && cc->iv_gen_ops->init)
				ret = cc->iv_gen_ops->init(cc);
			/* wipe the kernel key payload copy */
			if (cc->key_string)
				memset(cc->key, 0, cc->key_size * sizeof(u8));
			return ret;
		}
		if (argc == 2 && !strcasecmp(argv[1], "wipe")) {
			if (cc->iv_gen_ops && cc->iv_gen_ops->wipe) {
				ret = cc->iv_gen_ops->wipe(cc);
				if (ret)
					return ret;
			}
			return crypt_wipe_key(cc);
		}
	}

error:
	DMWARN("unrecognised message received.");
	return -EINVAL;
}

static int crypt_iterate_devices(struct dm_target *ti,
				 iterate_devices_callout_fn fn, void *data)
{
	struct crypt_config *cc = ti->private;

	return fn(ti, cc->dev, cc->start, ti->len, data);
}

static void crypt_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct crypt_config *cc = ti->private;

	/*
	 * Unfortunate constraint that is required to avoid the potential
	 * for exceeding underlying device's max_segments limits -- due to
	 * crypt_alloc_buffer() possibly allocating pages for the encryption
	 * bio that are not as physically contiguous as the original bio.
	 */
	limits->max_segment_size = PAGE_SIZE;

	limits->logical_block_size =
		max_t(unsigned short, limits->logical_block_size, cc->sector_size);
	limits->physical_block_size =
		max_t(unsigned, limits->physical_block_size, cc->sector_size);
	limits->io_min = max_t(unsigned, limits->io_min, cc->sector_size);

	if (crypt_mode_diskcipher(cc))
		limits->logical_block_size = PAGE_SIZE;
}

static struct target_type crypt_target = {
	.name   = "crypt",
	.version = {1, 18, 1},
	.module = THIS_MODULE,
	.ctr    = crypt_ctr,
	.dtr    = crypt_dtr,
	.map    = crypt_map,
	.status = crypt_status,
	.postsuspend = crypt_postsuspend,
	.preresume = crypt_preresume,
	.resume = crypt_resume,
	.message = crypt_message,
	.iterate_devices = crypt_iterate_devices,
	.io_hints = crypt_io_hints,
};

static int __init dm_crypt_init(void)
{
	int r;

	r = dm_register_target(&crypt_target);
	if (r < 0)
		DMERR("register failed %d", r);

	return r;
}

static void __exit dm_crypt_exit(void)
{
	dm_unregister_target(&crypt_target);
}

module_init(dm_crypt_init);
module_exit(dm_crypt_exit);

MODULE_AUTHOR("Jana Saout <jana@saout.de>");
MODULE_DESCRIPTION(DM_NAME " target for transparent encryption / decryption");
MODULE_LICENSE("GPL");
