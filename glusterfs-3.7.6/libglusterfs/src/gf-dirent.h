/*
  Copyright (c) 2008-2012 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/


#ifndef _GF_DIRENT_H
#define _GF_DIRENT_H

#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include "iatt.h"
#include "inode.h"
#include "list.h"
#define gf_dirent_size(name) (sizeof (gf_dirent_t) + strlen (name) + 1)

int
gf_deitransform(xlator_t *this, uint64_t y);

int
gf_itransform (xlator_t *this, uint64_t x, uint64_t *y_p, int client_id);

uint64_t
gf_dirent_orig_offset (xlator_t *this, uint64_t offset);


struct _dir_entry_t {
        struct _dir_entry_t *next;
	char                *name;
	char                *link;
	struct iatt          buf;
};
struct qstr {
	uint32_t hash;
	uint32_t len;
	const uint8_t *name;
}
typedef struct {
	volatile int counter;
} atomic_t;
struct _gf_dirent_t {
	union {
		struct list_head             list;
		struct {
			struct _gf_dirent_t *next;
			struct _gf_dirent_t *prev;
		};
	};
	atomic_t 							 d_count;
	struct hlist_node					 d_hash;/*lookup hash list*/
	struct list_head 					 d_lru;/*LRU list*/
	struct list_head 					 d_subdirs;/*all the children are linked together*/
	struct list_head					 d_alias;/*inode alias list,list of all the dentry share the same inode*/
	struct list_head					 d_child;/*child of parent list*/
	const struct dentry_operations 		 *d_op;
	uint64_t                             d_ino;
	uint64_t                             d_off;
	uint64_t 							 d_time;/*used by d_revalidate*/
	uint32_t                             d_len;
	uint32_t                             d_type;
	uint32_t							 d_flags;/*used by lock*/
        struct iatt                          d_stat;
        dict_t                              *dict;
        inode_t                             *inode;
	char                                 d_name[];
};

#define DT_ISDIR(mode) (mode == DT_DIR)

gf_dirent_t *gf_dirent_for_name (const char *name);
gf_dirent_t *entry_copy (gf_dirent_t *source);
void gf_dirent_entry_free (gf_dirent_t *entry);
void gf_dirent_free (gf_dirent_t *entries);
int gf_link_inodes_from_dirent (xlator_t *this, inode_t *parent,
                                gf_dirent_t *entries);
struct dentry_operations {
	int (*d_revalidate)(struct dentry *, struct nameidata *);
	int (*d_hash) (struct dentry *, struct qstr *);
	int (*d_compare) (struct dentry *, struct qstr *, struct qstr *);
	int (*d_delete)(struct dentry *);
	void (*d_release)(struct dentry *);
	void (*d_iput)(struct dentry *, struct inode *);
	//char *(*d_dname)(struct dentry *, char *, int);
}
void
gf_link_inode_from_dirent (xlator_t *this, inode_t *parent, gf_dirent_t *entry);
#endif /* _GF_DIRENT_H */
