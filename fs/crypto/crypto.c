/*
 * This contains encryption functions for per-file encryption.
 *
 * Copyright (C) 2015, Google, Inc.
 * Copyright (C) 2015, Motorola Mobility
 *
 * Written by Michael Halcrow, 2014.
 *
 * Filename encryption additions
 *	Uday Savagaonkar, 2014
 * Encryption policy handling additions
 *	Ildar Muslukhov, 2014
 * Add fscrypt_pullback_bio_page()
 *	Jaegeuk Kim, 2015.
 *
 * This has not yet undergone a rigorous security audit.
 *
 * The usage of AES-XTS should conform to recommendations in NIST
 * Special Publication 800-38E and IEEE P1619/D16.
 */

#include <linux/pagemap.h>
#include <linux/mempool.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/ratelimit.h>
#include <linux/dcache.h>
#include <linux/namei.h>
#include <crypto/aes.h>
#include <crypto/skcipher.h>
#include "fscrypt_private.h"

static unsigned int num_prealloc_crypto_pages = 32;
static unsigned int num_prealloc_crypto_ctxs = 128;

module_param(num_prealloc_crypto_pages, uint, 0444);
MODULE_PARM_DESC(num_prealloc_crypto_pages,
		"Number of crypto pages to preallocate");
module_param(num_prealloc_crypto_ctxs, uint, 0444);
MODULE_PARM_DESC(num_prealloc_crypto_ctxs,
		"Number of crypto contexts to preallocate");

static mempool_t *fscrypt_bounce_page_pool = NULL;

static LIST_HEAD(fscrypt_free_ctxs);
static DEFINE_SPINLOCK(fscrypt_ctx_lock);

static struct workqueue_struct *fscrypt_read_workqueue;
static DEFINE_MUTEX(fscrypt_init_mutex);

static struct kmem_cache *fscrypt_ctx_cachep;
struct kmem_cache *fscrypt_info_cachep;

void fscrypt_enqueue_decrypt_work(struct work_struct *work)
{
	queue_work(fscrypt_read_workqueue, work);
}
EXPORT_SYMBOL(fscrypt_enqueue_decrypt_work);

/**
 * fscrypt_release_ctx() - Release a decryption context
 * @ctx: The decryption context to release.
 *
 * If the decryption context was allocated from the pre-allocated pool, return
 * it to that pool.  Else, free it.
 */
void fscrypt_release_ctx(struct fscrypt_ctx *ctx)
{
	unsigned long flags;

	if (ctx->flags & FS_CTX_REQUIRES_FREE_ENCRYPT_FL) {
		kmem_cache_free(fscrypt_ctx_cachep, ctx);
	} else {
		spin_lock_irqsave(&fscrypt_ctx_lock, flags);
		list_add(&ctx->free_list, &fscrypt_free_ctxs);
		spin_unlock_irqrestore(&fscrypt_ctx_lock, flags);
	}
}
EXPORT_SYMBOL(fscrypt_release_ctx);

/**
 * fscrypt_get_ctx() - Get a decryption context
 * @gfp_flags:   The gfp flag for memory allocation
 *
 * Allocate and initialize a decryption context.
 *
 * Return: A new decryption context on success; an ERR_PTR() otherwise.
 */
 /**
 * fscrypt_free_bounce_page() - free a ciphertext bounce page
 * @bounce_page: the bounce page to free, or NULL
 *
 * Free a bounce page that was allocated by fscrypt_encrypt_pagecache_blocks(),
 * or by fscrypt_alloc_bounce_page() directly.
 */
void fscrypt_free_bounce_page(struct page *bounce_page)
{
	if (!bounce_page)
		return;
	set_page_private(bounce_page, (unsigned long)NULL);
	ClearPagePrivate(bounce_page);
	mempool_free(bounce_page, fscrypt_bounce_page_pool);
}
EXPORT_SYMBOL(fscrypt_free_bounce_page);

/* Encrypt or decrypt a single filesystem block of file contents */
int fscrypt_crypt_block(const struct inode *inode, fscrypt_direction_t rw,
			u64 lblk_num, struct page *src_page,
			struct page *dest_page, unsigned int len,
			unsigned int offs, gfp_t gfp_flags)
{
	union fscrypt_iv iv;
	struct skcipher_request *req = NULL;
	DECLARE_CRYPTO_WAIT(wait);
	struct scatterlist dst, src;
	struct fscrypt_info *ci = inode->i_crypt_info;
	struct crypto_skcipher *tfm = ci->ci_key.tfm;
	int res = 0;

	if (WARN_ON_ONCE(len <= 0))
		return -EINVAL;
	if (WARN_ON_ONCE(len % FS_CRYPTO_BLOCK_SIZE != 0))
		return -EINVAL;

	fscrypt_generate_iv(&iv, lblk_num, ci);

	req = skcipher_request_alloc(tfm, gfp_flags);
	if (!req)
		return -ENOMEM;

	skcipher_request_set_callback(
		req, CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
		crypto_req_done, &wait);

	sg_init_table(&dst, 1);
	sg_set_page(&dst, dest_page, len, offs);
	sg_init_table(&src, 1);
	sg_set_page(&src, src_page, len, offs);
	skcipher_request_set_crypt(req, &src, &dst, len, &iv);
	if (rw == FS_DECRYPT)
		res = crypto_wait_req(crypto_skcipher_decrypt(req), &wait);
	else
		res = crypto_wait_req(crypto_skcipher_encrypt(req), &wait);
	skcipher_request_free(req);
	if (res) {
		return res;
	}
	return 0;
}

struct fscrypt_ctx *fscrypt_get_ctx(gfp_t gfp_flags)
{
	struct fscrypt_ctx *ctx;
	unsigned long flags;

	/*
	 * First try getting a ctx from the free list so that we don't have to
	 * call into the slab allocator.
	 */
	spin_lock_irqsave(&fscrypt_ctx_lock, flags);
	ctx = list_first_entry_or_null(&fscrypt_free_ctxs,
					struct fscrypt_ctx, free_list);
	if (ctx)
		list_del(&ctx->free_list);
	spin_unlock_irqrestore(&fscrypt_ctx_lock, flags);
	if (!ctx) {
		ctx = kmem_cache_zalloc(fscrypt_ctx_cachep, gfp_flags);
		if (!ctx)
			return ERR_PTR(-ENOMEM);
		ctx->flags |= FS_CTX_REQUIRES_FREE_ENCRYPT_FL;
	} else {
		ctx->flags &= ~FS_CTX_REQUIRES_FREE_ENCRYPT_FL;
	}
	return ctx;
}
EXPORT_SYMBOL(fscrypt_get_ctx);

struct page *fscrypt_alloc_bounce_page(gfp_t gfp_flags)
{
	return mempool_alloc(fscrypt_bounce_page_pool, gfp_flags);
}

void fscrypt_generate_iv(union fscrypt_iv *iv, u64 lblk_num,
			const struct fscrypt_info *ci)
{
	u8 flags = fscrypt_policy_flags(&ci->ci_policy);

	memset(iv, 0, ci->ci_mode->ivsize);

	if (flags & FSCRYPT_POLICY_FLAG_IV_INO_LBLK_64) {
		WARN_ON_ONCE(lblk_num > U32_MAX);
		WARN_ON_ONCE(ci->ci_inode->i_ino > U32_MAX);
		lblk_num |= (u64)ci->ci_inode->i_ino << 32;
	} else if (flags & FSCRYPT_POLICY_FLAG_IV_INO_LBLK_32) {
		WARN_ON_ONCE(lblk_num > U32_MAX);
		lblk_num = (u32)(ci->ci_hashed_ino + lblk_num);
	} else if (flags & FSCRYPT_POLICY_FLAG_DIRECT_KEY) {
	memcpy(iv->nonce, ci->ci_nonce, FS_KEY_DERIVATION_NONCE_SIZE);
	} else if ((fscrypt_policy_contents_mode(&ci->ci_policy) ==
						127)
						&& false) {
		if (ci->ci_inode->i_sb->s_type->name) {
			if (!strcmp(ci->ci_inode->i_sb->s_type->name, "f2fs")) {
				WARN_ON_ONCE(lblk_num > U32_MAX);
				WARN_ON_ONCE(ci->ci_inode->i_ino > U32_MAX);
			lblk_num |= (u64)ci->ci_inode->i_ino << 32;
			}
		}
	}
	iv->lblk_num = cpu_to_le64(lblk_num);
}


/**
 * fscrypt_encrypt_pagecache_blocks() - Encrypt filesystem blocks from a pagecache page
 * @page:      The locked pagecache page containing the block(s) to encrypt
 * @len:       Total size of the block(s) to encrypt.  Must be a nonzero
 *		multiple of the filesystem's block size.
 * @offs:      Byte offset within @page of the first block to encrypt.  Must be
 *		a multiple of the filesystem's block size.
 * @gfp_flags: Memory allocation flags
 *
 * A new bounce page is allocated, and the specified block(s) are encrypted into
 * it.  In the bounce page, the ciphertext block(s) will be located at the same
 * offsets at which the plaintext block(s) were located in the source page; any
 * other parts of the bounce page will be left uninitialized.  However, normally
 * blocksize == PAGE_SIZE and the whole page is encrypted at once.
 *
 * This is for use by the filesystem's ->writepages() method.
 *
 * Return: the new encrypted bounce page on success; an ERR_PTR() on failure
 */
struct page *fscrypt_encrypt_pagecache_blocks(struct page *page,
					      unsigned int len,
					      unsigned int offs,
					      gfp_t gfp_flags)

{
	const struct inode *inode = page->mapping->host;
	const unsigned int blockbits = inode->i_blkbits;
	const unsigned int blocksize = 1 << blockbits;
	struct page *ciphertext_page;
	u64 lblk_num = ((u64)page->index << (PAGE_SHIFT - blockbits)) +
		       (offs >> blockbits);
	unsigned int i;
	int err;

	if (WARN_ON_ONCE(!PageLocked(page)))
		return ERR_PTR(-EINVAL);

	if (WARN_ON_ONCE(len <= 0 || !IS_ALIGNED(len | offs, blocksize)))
		return ERR_PTR(-EINVAL);

	ciphertext_page = fscrypt_alloc_bounce_page(gfp_flags);
	if (!ciphertext_page)
		return ERR_PTR(-ENOMEM);

	for (i = offs; i < offs + len; i += blocksize, lblk_num++) {
		err = fscrypt_crypt_block(inode, FS_ENCRYPT, lblk_num,
					  page, ciphertext_page,
					  blocksize, i, gfp_flags);
		if (err) {
			fscrypt_free_bounce_page(ciphertext_page);
			return ERR_PTR(err);
		}
	}
	SetPagePrivate(ciphertext_page);
	set_page_private(ciphertext_page, (unsigned long)page);
	return ciphertext_page;
}
EXPORT_SYMBOL(fscrypt_encrypt_pagecache_blocks);

/**
 * fscrypt_encrypt_block_inplace() - Encrypt a filesystem block in-place
 * @inode:     The inode to which this block belongs
 * @page:      The page containing the block to encrypt
 * @len:       Size of block to encrypt.  Doesn't need to be a multiple of the
 *		fs block size, but must be a multiple of FS_CRYPTO_BLOCK_SIZE.
 * @offs:      Byte offset within @page at which the block to encrypt begins
 * @lblk_num:  Filesystem logical block number of the block, i.e. the 0-based
 *		number of the block within the file
 * @gfp_flags: Memory allocation flags
 *
 * Encrypt a possibly-compressed filesystem block that is located in an
 * arbitrary page, not necessarily in the original pagecache page.  The @inode
 * and @lblk_num must be specified, as they can't be determined from @page.
 *
 * Return: 0 on success; -errno on failure
 */
int fscrypt_encrypt_block_inplace(const struct inode *inode, struct page *page,
				  unsigned int len, unsigned int offs,
				  u64 lblk_num, gfp_t gfp_flags)
{
	return fscrypt_crypt_block(inode, FS_ENCRYPT, lblk_num, page, page,
				   len, offs, gfp_flags);
}
EXPORT_SYMBOL(fscrypt_encrypt_block_inplace);

/**
 * fscrypt_decrypt_pagecache_blocks() - Decrypt filesystem blocks in a pagecache page
 * @page:      The locked pagecache page containing the block(s) to decrypt
 * @len:       Total size of the block(s) to decrypt.  Must be a nonzero
 *		multiple of the filesystem's block size.
 * @offs:      Byte offset within @page of the first block to decrypt.  Must be
 *		a multiple of the filesystem's block size.
 *
 * The specified block(s) are decrypted in-place within the pagecache page,
 * which must still be locked and not uptodate.  Normally, blocksize ==
 * PAGE_SIZE and the whole page is decrypted at once.
 *
 * This is for use by the filesystem's ->readpages() method.
 *
 * Return: 0 on success; -errno on failure
 */
int fscrypt_decrypt_pagecache_blocks(struct page *page, unsigned int len,
				     unsigned int offs)
{
	const struct inode *inode = page->mapping->host;
	const unsigned int blockbits = inode->i_blkbits;
	const unsigned int blocksize = 1 << blockbits;
	u64 lblk_num = ((u64)page->index << (PAGE_SHIFT - blockbits)) +
		       (offs >> blockbits);
	unsigned int i;
	int err;

	if (WARN_ON_ONCE(!PageLocked(page)))
		return -EINVAL;

	if (WARN_ON_ONCE(len <= 0 || !IS_ALIGNED(len | offs, blocksize)))
		return -EINVAL;

	for (i = offs; i < offs + len; i += blocksize, lblk_num++) {
		err = fscrypt_crypt_block(inode, FS_DECRYPT, lblk_num, page,
					  page, blocksize, i, GFP_NOFS);
		if (err)
			return err;
	}
	return 0;
}
EXPORT_SYMBOL(fscrypt_decrypt_pagecache_blocks);

/**
 * fscrypt_decrypt_block_inplace() - Decrypt a filesystem block in-place
 * @inode:     The inode to which this block belongs
 * @page:      The page containing the block to decrypt
 * @len:       Size of block to decrypt.  Doesn't need to be a multiple of the
 *		fs block size, but must be a multiple of FS_CRYPTO_BLOCK_SIZE.
 * @offs:      Byte offset within @page at which the block to decrypt begins
 * @lblk_num:  Filesystem logical block number of the block, i.e. the 0-based
 *		number of the block within the file
 *
 * Decrypt a possibly-compressed filesystem block that is located in an
 * arbitrary page, not necessarily in the original pagecache page.  The @inode
 * and @lblk_num must be specified, as they can't be determined from @page.
 *
 * Return: 0 on success; -errno on failure
 */
int fscrypt_decrypt_block_inplace(const struct inode *inode, struct page *page,
				  unsigned int len, unsigned int offs,
				  u64 lblk_num)
{
	return fscrypt_crypt_block(inode, FS_DECRYPT, lblk_num, page, page,
				   len, offs, GFP_NOFS);
}
EXPORT_SYMBOL(fscrypt_decrypt_block_inplace);

/*
 * Validate dentries in encrypted directories to make sure we aren't potentially
 * caching stale dentries after a key has been added.
 */
static int fscrypt_d_revalidate(struct dentry *dentry, unsigned int flags)
{
	struct dentry *dir;
	int err;
	int valid;

	/*
	 * Plaintext names are always valid, since fscrypt doesn't support
	 * reverting to ciphertext names without evicting the directory's inode
	 * -- which implies eviction of the dentries in the directory.
	 */
	if (!(dentry->d_flags & DCACHE_ENCRYPTED_NAME))
		return 1;

	/*
	 * Ciphertext name; valid if the directory's key is still unavailable.
	 *
	 * Although fscrypt forbids rename() on ciphertext names, we still must
	 * use dget_parent() here rather than use ->d_parent directly.  That's
	 * because a corrupted fs image may contain directory hard links, which
	 * the VFS handles by moving the directory's dentry tree in the dcache
	 * each time ->lookup() finds the directory and it already has a dentry
	 * elsewhere.  Thus ->d_parent can be changing, and we must safely grab
	 * a reference to some ->d_parent to prevent it from being freed.
	 */

	if (flags & LOOKUP_RCU)
		return -ECHILD;

	dir = dget_parent(dentry);
	err = fscrypt_get_encryption_info(d_inode(dir));
	valid = !fscrypt_has_encryption_key(d_inode(dir));
	dput(dir);

	if (err < 0)
		return err;

	return valid;
}

const struct dentry_operations fscrypt_d_ops = {
	.d_revalidate = fscrypt_d_revalidate,
};

static void fscrypt_destroy(void)
{
	struct fscrypt_ctx *pos, *n;

	list_for_each_entry_safe(pos, n, &fscrypt_free_ctxs, free_list)
		kmem_cache_free(fscrypt_ctx_cachep, pos);
	INIT_LIST_HEAD(&fscrypt_free_ctxs);
	mempool_destroy(fscrypt_bounce_page_pool);
	fscrypt_bounce_page_pool = NULL;
}

/**
 * fscrypt_initialize() - allocate major buffers for fs encryption.
 * @cop_flags:  fscrypt operations flags
 *
 * We only call this when we start accessing encrypted files, since it
 * results in memory getting allocated that wouldn't otherwise be used.
 *
 * Return: Zero on success, non-zero otherwise.
 */
int fscrypt_initialize(unsigned int cop_flags)
{
	int i, res = -ENOMEM;

	/* No need to allocate a bounce page pool if this FS won't use it. */
	if (cop_flags & FS_CFLG_OWN_PAGES)
		return 0;

	mutex_lock(&fscrypt_init_mutex);
	if (fscrypt_bounce_page_pool)
		goto already_initialized;

	for (i = 0; i < num_prealloc_crypto_ctxs; i++) {
		struct fscrypt_ctx *ctx;

		ctx = kmem_cache_zalloc(fscrypt_ctx_cachep, GFP_NOFS);
		if (!ctx)
			goto fail;
		list_add(&ctx->free_list, &fscrypt_free_ctxs);
	}

	fscrypt_bounce_page_pool =
		mempool_create_page_pool(num_prealloc_crypto_pages, 0);
	if (!fscrypt_bounce_page_pool)
		goto fail;

already_initialized:
	mutex_unlock(&fscrypt_init_mutex);
	return 0;
fail:
	fscrypt_destroy();
	mutex_unlock(&fscrypt_init_mutex);
	return res;
}

void fscrypt_msg(struct super_block *sb, const char *level,
		 const char *fmt, ...)
{
	static DEFINE_RATELIMIT_STATE(rs, DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);
	struct va_format vaf;
	va_list args;

	if (!__ratelimit(&rs))
		return;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	if (sb)
		printk("%sfscrypt (%s): %pV\n", level, sb->s_id, &vaf);
	else
		printk("%sfscrypt: %pV\n", level, &vaf);
	va_end(args);
}

/**
 * fscrypt_init() - Set up for fs encryption.
 */
static int __init fscrypt_init(void)
{
	int err = -ENOMEM;
	/*
	 * Use an unbound workqueue to allow bios to be decrypted in parallel
	 * even when they happen to complete on the same CPU.  This sacrifices
	 * locality, but it's worthwhile since decryption is CPU-intensive.
	 *
	 * Also use a high-priority workqueue to prioritize decryption work,
	 * which blocks reads from completing, over regular application tasks.
	 */
	fscrypt_read_workqueue = alloc_workqueue("fscrypt_read_queue",
						 WQ_UNBOUND | WQ_HIGHPRI,
						 num_online_cpus());
	if (!fscrypt_read_workqueue)
		goto fail;

	fscrypt_ctx_cachep = KMEM_CACHE(fscrypt_ctx, SLAB_RECLAIM_ACCOUNT);
	if (!fscrypt_ctx_cachep)
		goto fail_free_queue;

	fscrypt_info_cachep = KMEM_CACHE(fscrypt_info, SLAB_RECLAIM_ACCOUNT);
	if (!fscrypt_info_cachep)
		goto fail_free_ctx;

	err = fscrypt_init_keyring();
	if (err)
		goto fail_free_info;

	return 0;

fail_free_ctx:
	kmem_cache_destroy(fscrypt_ctx_cachep);
fail_free_queue:
	destroy_workqueue(fscrypt_read_workqueue);
fail_free_info:
	kmem_cache_destroy(fscrypt_info_cachep);
fail:
	return err;
}
late_initcall(fscrypt_init)
MODULE_LICENSE("GPL");
