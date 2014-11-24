/*
 * Copyright (C) 2014 Fujitsu.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

/*
 * Unlike inode.c in kernel, which can use most of the kernel infrastructure
 * like inode/dentry things, in user-land, we can only use inode number to
 * do directly operation on extent buffer, which may cause extra searching,
 * but should not be a huge problem since progs is less performence sensitive.
 */
#include <sys/stat.h>
#include "ctree.h"
#include "transaction.h"
#include "disk-io.h"
#include "time.h"

/*
 * Find a free inode index for later btrfs_add_link().
 * Currently just search from the largest dir_index and +1.
 */
static int btrfs_find_free_dir_index(struct btrfs_root *root, u64 dir_ino,
				     u64 *ret_ino)
{
	struct btrfs_path *path;
	struct btrfs_key key;
	struct btrfs_key found_key;
	u64 ret_val = 2;
	int ret = 0;

	if (!ret_ino)
		return 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = dir_ino;
	key.type = BTRFS_DIR_INDEX_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		goto out;
	ret = 0;
	if (path->slots[0] == 0) {
		ret = btrfs_prev_leaf(root, path);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			/*
			 * This shouldn't happen since there must be a leaf
			 * containing the DIR_ITEM.
			 * Can only happen when the previous leaf is corrupted.
			 */
			ret = -EIO;
			goto out;
		}
	} else {
		path->slots[0]--;
	}
	btrfs_item_key_to_cpu(path->nodes[0], &found_key, path->slots[0]);
	if (found_key.objectid != dir_ino ||
	    found_key.type != BTRFS_DIR_INDEX_KEY)
		goto out;
	ret_val = found_key.offset + 1;
out:
	btrfs_free_path(path);
	if (ret == 0)
		*ret_ino = ret_val;
	return ret;
}

/*
 * Add dir_item/index for 'parent_ino'
 * if add_backref is true, also insert a backref from the ino to parent dir
 * and update the nlink(Kernel version does not do this thing)
 *
 * Currently only supports adding link from an inode to another inode.
 */
int btrfs_add_link(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		   u64 ino, u64 parent_ino, char *name, int namelen,
		   u8 type, u64 *index, int add_backref)
{
	struct btrfs_path *path;
	struct btrfs_key key;
	struct btrfs_inode_item *inode_item;
	u32 nlink;
	u64 inode_size;
	u64 ret_index = 0;
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = btrfs_find_free_dir_index(root, parent_ino, &ret_index);
	if (ret < 0)
		goto out;

	/* Add inode ref */
	if (add_backref) {
		ret = btrfs_insert_inode_ref(trans, root, name, namelen,
					     ino, parent_ino, ret_index);
		if (ret < 0)
			goto out;

		/* Update nlinks for the inode */
		key.objectid = ino;
		key.type = BTRFS_INODE_ITEM_KEY;
		key.offset = 0;
		ret = btrfs_search_slot(trans, root, &key, path, 1, 1);
		if (ret) {
			if (ret > 0)
				ret = -ENOENT;
			goto out;
		}
		inode_item = btrfs_item_ptr(path->nodes[0], path->slots[0],
				    struct btrfs_inode_item);
		nlink = btrfs_inode_nlink(path->nodes[0], inode_item);
		nlink++;
		btrfs_set_inode_nlink(path->nodes[0], inode_item, nlink);
		btrfs_mark_buffer_dirty(path->nodes[0]);
		btrfs_release_path(path);
	}

	/* Add dir_item and dir_index */
	key.objectid = ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_insert_dir_item(trans, root, name, namelen, parent_ino,
				    &key, type, ret_index);
	if (ret < 0)
		goto out;

	/* Update inode size of the parent inode */
	key.objectid = parent_ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(trans, root, &key, path, 1, 1);
	if (ret)
		goto out;
	inode_item = btrfs_item_ptr(path->nodes[0], path->slots[0],
				    struct btrfs_inode_item);
	inode_size = btrfs_inode_size(path->nodes[0], inode_item);
	inode_size += namelen * 2;
	btrfs_set_inode_size(path->nodes[0], inode_item, inode_size);
	btrfs_mark_buffer_dirty(path->nodes[0]);
	btrfs_release_path(path);


out:
	btrfs_free_path(path);
	if (ret == 0 && index)
		*index = ret_index;
	return ret;
}

int btrfs_add_orphan_item(struct btrfs_trans_handle *trans,
			  struct btrfs_root *root,
			  struct btrfs_path *path,
			  u64 ino)
{
	struct btrfs_key key;
	int ret = 0;

	key.objectid = BTRFS_ORPHAN_OBJECTID;
	key.type = BTRFS_ORPHAN_ITEM_KEY;
	key.offset = ino;

	ret = btrfs_insert_empty_item(trans, root, path, &key, 0);
	return ret;
}

/*
 * Unlink an inode, which will remove its backref and corresponding dir_index/
 * dir_item if any of them exists.
 *
 * If an inode's nlink is reduced to 0 and 'add_orphan' is true, it will be
 * added to orphan inode and wairing to be deleted by next kernel mount.
 */
int btrfs_unlink(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		 u64 ino, u64 parent_ino, u64 index, const char *name,
		 int namelen, int add_orphan)
{
	struct btrfs_path *path;
	struct btrfs_key key;
	struct btrfs_inode_item *inode_item;
	struct btrfs_inode_ref *inode_ref;
	struct btrfs_dir_item *dir_item;
	u64 inode_size;
	u32 nlinks;
	int del_inode_ref = 0;
	int del_dir_item = 0;
	int del_dir_index = 0;
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/* check the ref and backref exists */
	inode_ref = btrfs_lookup_inode_ref(trans, root, path, name, namelen,
					   ino, parent_ino, index, 0);
	if (IS_ERR(inode_ref)) {
		ret = PTR_ERR(inode_ref);
		goto out;
	}
	if (inode_ref)
		del_inode_ref = 1;
	btrfs_release_path(path);

	dir_item = btrfs_lookup_dir_item(NULL, root, path, parent_ino,
					 name, namelen, 0);
	if (IS_ERR(dir_item)) {
		ret = PTR_ERR(dir_item);
		goto out;
	}
	if (dir_item)
		del_dir_item = 1;
	btrfs_release_path(path);

	dir_item = btrfs_lookup_dir_index(NULL, root, path, parent_ino,
					  name, namelen, index, 0);
	/*
	 * Since lookup_dir_index() will return -ENOENT when not found,
	 * we need to do extra check.
	 */
	if (IS_ERR(dir_item) && PTR_ERR(dir_item) == -ENOENT)
		dir_item = NULL;
	if (IS_ERR(dir_item)) {
		ret = PTR_ERR(dir_item);
		goto out;
	}
	if (dir_item)
		del_dir_index = 1;
	btrfs_release_path(path);

	if (!del_inode_ref && !del_dir_item && !del_dir_index) {
		/* All not found, shouldn't happen */
		ret = -ENOENT;
		goto out;
	}

	if (del_inode_ref) {
		/* Only decrease nlink when deleting inode_ref */
		key.objectid = ino;
		key.type = BTRFS_INODE_ITEM_KEY;
		key.offset = 0;
		ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
		if (ret) {
			if (ret > 0)
				ret = -ENOENT;
			goto out;
		}
		inode_item = btrfs_item_ptr(path->nodes[0], path->slots[0],
					    struct btrfs_inode_item);
		nlinks = btrfs_inode_nlink(path->nodes[0], inode_item);
		if (nlinks > 0)
			nlinks--;
		btrfs_set_inode_nlink(path->nodes[0], inode_item, nlinks);
		btrfs_mark_buffer_dirty(path->nodes[0]);
		btrfs_release_path(path);

		/* For nlinks == 0, add it to orphan list if needed */
		if (nlinks == 0 && add_orphan) {
			ret = btrfs_add_orphan_item(trans, root, path, ino);
			btrfs_mark_buffer_dirty(path->nodes[0]);
			btrfs_release_path(path);
		}

		ret = btrfs_del_inode_ref(trans, root, name, namelen, ino,
					  parent_ino, &index);
		if (ret < 0)
			goto out;
	}

	if (del_dir_index) {
		dir_item = btrfs_lookup_dir_index(trans, root, path,
						  parent_ino, name, namelen,
						  index, -1);
		if (IS_ERR(dir_item)) {
			ret = PTR_ERR(dir_item);
			goto out;
		}
		if (!dir_item) {
			ret = -ENOENT;
			goto out;
		}
		ret = btrfs_delete_one_dir_name(trans, root, path, dir_item);
		if (ret)
			goto out;
		btrfs_release_path(path);

		/* Update inode size of the parent inode */
		key.objectid = parent_ino;
		key.type = BTRFS_INODE_ITEM_KEY;
		key.offset = 0;
		ret = btrfs_search_slot(trans, root, &key, path, 1, 1);
		if (ret)
			goto out;
		inode_item = btrfs_item_ptr(path->nodes[0], path->slots[0],
					    struct btrfs_inode_item);
		inode_size = btrfs_inode_size(path->nodes[0], inode_item);
		if (inode_size >= namelen)
			inode_size -= namelen;
		btrfs_set_inode_size(path->nodes[0], inode_item, inode_size);
		btrfs_mark_buffer_dirty(path->nodes[0]);
		btrfs_release_path(path);
	}

	if (del_dir_item) {
		dir_item = btrfs_lookup_dir_item(trans, root, path, parent_ino,
						 name, namelen, -1);
		if (IS_ERR(dir_item)) {
			ret = PTR_ERR(dir_item);
			goto out;
		}
		if (!dir_item) {
			ret = -ENOENT;
			goto out;
		}
		ret = btrfs_delete_one_dir_name(trans, root, path, dir_item);
		if (ret < 0)
			goto out;
		btrfs_release_path(path);

		/* Update inode size of the parent inode */
		key.objectid = parent_ino;
		key.type = BTRFS_INODE_ITEM_KEY;
		key.offset = 0;
		ret = btrfs_search_slot(trans, root, &key, path, 1, 1);
		if (ret)
			goto out;
		inode_item = btrfs_item_ptr(path->nodes[0], path->slots[0],
					    struct btrfs_inode_item);
		inode_size = btrfs_inode_size(path->nodes[0], inode_item);
		if (inode_size >= namelen)
			inode_size -= namelen;
		btrfs_set_inode_size(path->nodes[0], inode_item, inode_size);
		btrfs_mark_buffer_dirty(path->nodes[0]);
		btrfs_release_path(path);
	}

out:
	btrfs_free_path(path);
	return ret;
}
