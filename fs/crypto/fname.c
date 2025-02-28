/*
 * This contains functions for filename crypto management
 *
 * Copyright (C) 2015, Google, Inc.
 * Copyright (C) 2015, Motorola Mobility
 *
 * Written by Uday Savagaonkar, 2014.
 * Modified by Jaegeuk Kim, 2015.
 *
 * This has not yet undergone a rigorous security audit.
 */

#include <linux/scatterlist.h>
#include <crypto/skcipher.h>
#include "fscrypt_private.h"

static inline bool fscrypt_is_dot_dotdot(const struct qstr *str)
{
	if (str->len == 1 && str->name[0] == '.')
		return true;

	if (str->len == 2 && str->name[0] == '.' && str->name[1] == '.')
		return true;

	return false;
}

/**
 * fname_encrypt() - encrypt a filename
 *
 * The output buffer must be at least as large as the input buffer.
 * Any extra space is filled with NUL padding before encryption.
 *
 * Return: 0 on success, -errno on failure
 */
int fname_encrypt(struct inode *inode, const struct qstr *iname,
		  u8 *out, unsigned int olen)
{
	struct skcipher_request *req = NULL;
	DECLARE_CRYPTO_WAIT(wait);
	struct fscrypt_info *ci = inode->i_crypt_info;
	struct crypto_skcipher *tfm = ci->ci_ctfm;
	union fscrypt_iv iv;
	struct scatterlist sg;
	int res;

	/*
	 * Copy the filename to the output buffer for encrypting in-place and
	 * pad it with the needed number of NUL bytes.
	 */
	if (WARN_ON(olen < iname->len))
		return -ENOBUFS;
	memcpy(out, iname->name, iname->len);
	memset(out + iname->len, 0, olen - iname->len);

	/* Initialize the IV */
	fscrypt_generate_iv(&iv, 0, ci);

	/* Set up the encryption request */
	req = skcipher_request_alloc(tfm, GFP_NOFS);
	if (!req)
		return -ENOMEM;
	skcipher_request_set_callback(req,
			CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
			crypto_req_done, &wait);
	sg_init_one(&sg, out, olen);
	skcipher_request_set_crypt(req, &sg, &sg, olen, &iv);

	/* Do the encryption */
	res = crypto_wait_req(crypto_skcipher_encrypt(req), &wait);
	skcipher_request_free(req);
	if (res < 0) {
		fscrypt_err(inode->i_sb,
			    "Filename encryption failed for inode %lu: %d",
			    inode->i_ino, res);
		return res;
	}

	return 0;
}

/**
 * fname_decrypt() - decrypt a filename
 *
 * The caller must have allocated sufficient memory for the @oname string.
 *
 * Return: 0 on success, -errno on failure
 */
static int fname_decrypt(const struct inode *inode,
				const struct fscrypt_str *iname,
				struct fscrypt_str *oname)
{
	struct skcipher_request *req = NULL;
	DECLARE_CRYPTO_WAIT(wait);
	struct scatterlist src_sg, dst_sg;
	struct fscrypt_info *ci = inode->i_crypt_info;
	struct crypto_skcipher *tfm = ci->ci_ctfm;
	union fscrypt_iv iv;
	int res;

	/* Allocate request */
	req = skcipher_request_alloc(tfm, GFP_NOFS);
	if (!req)
		return -ENOMEM;
	skcipher_request_set_callback(req,
		CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
		crypto_req_done, &wait);

	/* Initialize IV */
	fscrypt_generate_iv(&iv, 0, ci);

	/* Create decryption request */
	sg_init_one(&src_sg, iname->name, iname->len);
	sg_init_one(&dst_sg, oname->name, oname->len);
	skcipher_request_set_crypt(req, &src_sg, &dst_sg, iname->len, &iv);
	res = crypto_wait_req(crypto_skcipher_decrypt(req), &wait);
	skcipher_request_free(req);
	if (res < 0) {
		fscrypt_err(inode->i_sb,
			    "Filename decryption failed for inode %lu: %d",
			    inode->i_ino, res);
		return res;
	}

	oname->len = strnlen(oname->name, iname->len);
	return 0;
}

static const char *lookup_table =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,";

#define BASE64_CHARS(nbytes)	DIV_ROUND_UP((nbytes) * 4, 3)

bool fscrypt_fname_encrypted_size(const struct inode *inode, u32 orig_len,
				  u32 max_len, u32 *encrypted_len_ret)
{
	int padding = 4 << (inode->i_crypt_info->ci_flags &
			    FS_POLICY_FLAGS_PAD_MASK);
	u32 encrypted_len;

	if (orig_len > max_len)
		return false;
	encrypted_len = max(orig_len, (u32)FS_CRYPTO_BLOCK_SIZE);
	encrypted_len = round_up(encrypted_len, padding);
	*encrypted_len_ret = min(encrypted_len, max_len);
	return true;
}

/**
 * fscrypt_fname_alloc_buffer - allocate a buffer for presented filenames
 *
 * Allocate a buffer that is large enough to hold any decrypted or encoded
 * filename (null-terminated), for the given maximum encrypted filename length.
 *
 * Return: 0 on success, -errno on failure
 */
int fscrypt_fname_alloc_buffer(const struct inode *inode,
			       u32 max_encrypted_len,
			       struct fscrypt_str *crypto_str)
{
	const u32 max_encoded_len =
		max_t(u32, BASE64_CHARS(FSCRYPT_FNAME_MAX_UNDIGESTED_SIZE),
		      1 + BASE64_CHARS(sizeof(struct fscrypt_digested_name)));
	u32 max_presented_len;

	max_presented_len = max(max_encoded_len, max_encrypted_len);

	crypto_str->name = kmalloc(max_presented_len + 1, GFP_NOFS);
	if (!crypto_str->name)
		return -ENOMEM;
	crypto_str->len = max_presented_len;
	return 0;
}
EXPORT_SYMBOL(fscrypt_fname_alloc_buffer);

/**
 * fscrypt_fname_free_buffer - free the buffer for presented filenames
 *
 * Free the buffer allocated by fscrypt_fname_alloc_buffer().
 */
void fscrypt_fname_free_buffer(struct fscrypt_str *crypto_str)
{
	if (!crypto_str)
		return;
	kfree(crypto_str->name);
	crypto_str->name = NULL;
}
EXPORT_SYMBOL(fscrypt_fname_free_buffer);

static int base64_encode(const u8 *src, int len, char *dst)
{
        int i, bits = 0, ac = 0;
        char *cp = dst;

        for (i = 0; i < len; i++) {
                ac += src[i] << bits;
                bits += 8;
                do {
                        *cp++ = lookup_table[ac & 0x3f];
                        ac >>= 6;
                        bits -= 6;
                } while (bits >= 6);
        }
        if (bits)
                *cp++ = lookup_table[ac & 0x3f];
        return cp - dst;
}

static int base64_decode(const char *src, int len, u8 *dst)
{
        int i, bits = 0, ac = 0;
        const char *p;
        u8 *cp = dst;

        for (i = 0; i < len; i++) {
                p = strchr(lookup_table, src[i]);
                if (p == NULL || src[i] == 0)
                        return -2;
                ac += (p - lookup_table) << bits;
                bits += 6;
                if (bits >= 8) {
                        *cp++ = ac & 0xff;
                        ac >>= 8;
                        bits -= 8;
                }
        }
        if (ac)
                return -1;
        return cp - dst;
}

/*
 * Decoded size of max-size nokey name, i.e. a name that was abbreviated using
 * the strong hash and thus includes the 'sha256' field.  This isn't simply
 * sizeof(struct fscrypt_nokey_name), as the padding at the end isn't included.
 */
#define FSCRYPT_NOKEY_NAME_MAX  offsetofend(struct fscrypt_nokey_name, sha256)

struct fscrypt_nokey_name {
        u32 dirhash[2];
        u8 bytes[149];
        u8 sha256[32];
};

static struct crypto_shash *sha256_hash_tfm;

static int fscrypt_do_sha256(const u8 *data, unsigned int data_len, u8 *result)
{
    struct crypto_shash *tfm = READ_ONCE(sha256_hash_tfm);

    if (unlikely(!tfm)) {
        struct crypto_shash *prev_tfm;
        tfm = crypto_alloc_shash("sha256", 0, 0);
        if (IS_ERR(tfm)) {
             return PTR_ERR(tfm);
        }
        prev_tfm = cmpxchg(&sha256_hash_tfm, NULL, tfm);
        if (prev_tfm) {
            crypto_free_shash(tfm);
            tfm = prev_tfm;
        }
    }
    {
        SHASH_DESC_ON_STACK(desc, tfm);
        desc->tfm = tfm;
        desc->flags = 0;
        return crypto_shash_digest(desc, data, data_len, result);
    }
}

/**
 * fscrypt_fname_disk_to_usr() - converts a filename from disk space to user
 * space
 *
 * The caller must have allocated sufficient memory for the @oname string.
 *
 * If the key is available, we'll decrypt the disk name; otherwise, we'll encode
 * it for presentation.  Short names are directly base64-encoded, while long
 * names are encoded in fscrypt_digested_name format.
 *
 * Return: 0 on success, -errno on failure
 */
int fscrypt_fname_disk_to_usr(const struct inode *inode,
			u32 hash, u32 minor_hash,
			const struct fscrypt_str *iname,
			struct fscrypt_str *oname)
{
	const struct qstr qname = FSTR_TO_QSTR(iname);
	struct fscrypt_nokey_name nokey_name;
	u32 size; /* size of the unencoded no-key name */
	int err;

	if (fscrypt_is_dot_dotdot(&qname)) {
		oname->name[0] = '.';
		oname->name[iname->len - 1] = '.';
		oname->len = iname->len;
		return 0;
	}

	if (iname->len < FS_CRYPTO_BLOCK_SIZE)
		return -EUCLEAN;

	if (fscrypt_has_encryption_key(inode))
		return fname_decrypt(inode, iname, oname);

/*
	 * Sanity check that struct fscrypt_nokey_name doesn't have padding
	 * between fields and that its encoded size never exceeds NAME_MAX.
	 */
	BUILD_BUG_ON(offsetofend(struct fscrypt_nokey_name, dirhash) !=
		offsetof(struct fscrypt_nokey_name, bytes));
	BUILD_BUG_ON(offsetofend(struct fscrypt_nokey_name, bytes) !=
		offsetof(struct fscrypt_nokey_name, sha256));
	BUILD_BUG_ON(BASE64_CHARS(FSCRYPT_NOKEY_NAME_MAX) > NAME_MAX);

	if (hash) {
		nokey_name.dirhash[0] = hash;
		nokey_name.dirhash[1] = minor_hash;
	} else {
		nokey_name.dirhash[0] = 0;
		nokey_name.dirhash[1] = 0;

	}
	if (iname->len <= sizeof(nokey_name.bytes)) {
		memcpy(nokey_name.bytes, iname->name, iname->len);
		size = offsetof(struct fscrypt_nokey_name, bytes[iname->len]);
	} else {
		memcpy(nokey_name.bytes, iname->name, sizeof(nokey_name.bytes));
		/* Compute strong hash of remaining part of name. */
		err = fscrypt_do_sha256(&iname->name[sizeof(nokey_name.bytes)],
					iname->len - sizeof(nokey_name.bytes),
					nokey_name.sha256);
		if (err)
			return err;
		size = FSCRYPT_NOKEY_NAME_MAX;
	}
	oname->len = base64_encode((const u8 *)&nokey_name, size, oname->name);
	return 0;
}
EXPORT_SYMBOL(fscrypt_fname_disk_to_usr);

/**
 * fscrypt_setup_filename() - prepare to search a possibly encrypted directory
 * @dir: the directory that will be searched
 * @iname: the user-provided filename being searched for
 * @lookup: 1 if we're allowed to proceed without the key because it's
 *	->lookup() or we're finding the dir_entry for deletion; 0 if we cannot
 *	proceed without the key because we're going to create the dir_entry.
 * @fname: the filename information to be filled in
 *
 * Given a user-provided filename @iname, this function sets @fname->disk_name
 * to the name that would be stored in the on-disk directory entry, if possible.
 * If the directory is unencrypted this is simply @iname.  Else, if we have the
 * directory's encryption key, then @iname is the plaintext, so we encrypt it to
 * get the disk_name.
 *
 * Else, for keyless @lookup operations, @iname is the presented ciphertext, so
 * we decode it to get either the ciphertext disk_name (for short names) or the
 * fscrypt_digested_name (for long names).  Non-@lookup operations will be
 * impossible in this case, so we fail them with ENOKEY.
 *
 * If successful, fscrypt_free_filename() must be called later to clean up.
 *
 * Return: 0 on success, -errno on failure
 */
int fscrypt_setup_filename(struct inode *dir, const struct qstr *iname,
			      int lookup, struct fscrypt_name *fname)
{
    struct fscrypt_nokey_name *nokey_name;
	int ret;

	memset(fname, 0, sizeof(struct fscrypt_name));
	fname->usr_fname = iname;

	if (!IS_ENCRYPTED(dir) || fscrypt_is_dot_dotdot(iname)) {
		fname->disk_name.name = (unsigned char *)iname->name;
		fname->disk_name.len = iname->len;
		return 0;
	}
	ret = fscrypt_get_encryption_info(dir);
	if (ret)
		return ret;

	if (fscrypt_has_encryption_key(dir)) {
		if (!fscrypt_fname_encrypted_size(dir, iname->len,
						  dir->i_sb->s_cop->max_namelen,
						  &fname->crypto_buf.len))
			return -ENAMETOOLONG;
		fname->crypto_buf.name = kmalloc(fname->crypto_buf.len,
						 GFP_NOFS);
		if (!fname->crypto_buf.name)
			return -ENOMEM;

		ret = fname_encrypt(dir, iname, fname->crypto_buf.name,
				    fname->crypto_buf.len);
		if (ret)
			goto errout;
		fname->disk_name.name = fname->crypto_buf.name;
		fname->disk_name.len = fname->crypto_buf.len;
		return 0;
	}
	if (!lookup)
		return -ENOKEY;
	fname->is_ciphertext_name = true;

	/*
	 * We don't have the key and we are doing a lookup; decode the
	 * user-supplied name
	 */
	if (iname->len > BASE64_CHARS(FSCRYPT_NOKEY_NAME_MAX))
		return -ENOENT;

	fname->crypto_buf.name = kmalloc(FSCRYPT_NOKEY_NAME_MAX,
			GFP_KERNEL);
	if (fname->crypto_buf.name == NULL)
		return -ENOMEM;

	ret = base64_decode(iname->name, iname->len, fname->crypto_buf.name);
	if (ret < (int)offsetof(struct fscrypt_nokey_name, bytes[1]) ||
	   (ret > offsetof(struct fscrypt_nokey_name, sha256) &&
	   ret != FSCRYPT_NOKEY_NAME_MAX)) {
		ret = -ENOENT;
		goto errout;
	}
	fname->crypto_buf.len = ret;
	nokey_name = (void *)fname->crypto_buf.name;
	fname->hash = nokey_name->dirhash[0];
	fname->minor_hash = nokey_name->dirhash[1];
	if (ret != FSCRYPT_NOKEY_NAME_MAX) {
		/* The full ciphertext filename is available. */
		fname->disk_name.name = nokey_name->bytes;
		fname->disk_name.len =
			ret - offsetof(struct fscrypt_nokey_name, bytes);
	}
	return 0;

errout:
	kfree(fname->crypto_buf.name);
	return ret;
}
EXPORT_SYMBOL(fscrypt_setup_filename);
