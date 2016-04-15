/*
  Copyright (c) 2008-2013 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

/*
 * performance/readdir-ahead preloads a local buffer with directory entries
 * on opendir. The optimization involves using maximum sized gluster rpc
 * requests (128k) to minimize overhead of smaller client requests.
 *
 * For example, fuse currently supports a maximum readdir buffer of 4k
 * (regardless of the filesystem client's buffer size). readdir-ahead should
 * effectively convert these smaller requests into fewer, larger sized requests
 * for simple, sequential workloads (i.e., ls).
 *
 * The translator is currently designed to handle the simple, sequential case
 * only. If a non-sequential directory read occurs, readdir-ahead disables
 * preloads on the directory.
 */

#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include "glusterfs.h"
#include "xlator.h"
#include "call-stub.h"
#include "readdir-ahead.h"
#include "readdir-ahead-mem-types.h"
#include "defaults.h"
#include "readdir-ahead-messages.h"
static int rda_fill_fd(call_frame_t *, xlator_t *, fd_t *);

/*
 * Get (or create) the fd context for storing prepopulated directory
 * entries.
 */
static struct
rda_fd_ctx *get_rda_fd_ctx(fd_t *fd, xlator_t *this)
//通过文件描述符获得文件描述符上下文 获取成功将文件描述符表中的文件描述符整数传给ctx结构体
{
	uint64_t val;
	struct rda_fd_ctx *ctx;

	LOCK(&fd->lock);
	//判断是否获取fd_ctx成功，如未成功，则创建
	if (__fd_ctx_get(fd, this, &val) < 0) {
		ctx = GF_CALLOC(1, sizeof(struct rda_fd_ctx),
				gf_rda_mt_rda_fd_ctx);
		if (!ctx)
			goto out;

		LOCK_INIT(&ctx->lock);
		INIT_LIST_HEAD(&ctx->entries.list);
		//对于新创建的ctx，标记为NEW
		ctx->state = RDA_FD_NEW;
		/* ctx offset values initialized to 0 */

		if (__fd_ctx_set(fd, this, (uint64_t) ctx) < 0) {
			GF_FREE(ctx);
			ctx = NULL;
			goto out;
		}
	} else {
	//如果获取成功
		ctx = (struct rda_fd_ctx *) val;
	}
out:
	UNLOCK(&fd->lock);
	return ctx;
}

/*
 * Reset the tracking state of the context.重新设置ctx的状态
 */
static void
rda_reset_ctx(struct rda_fd_ctx *ctx)
{
	ctx->state = RDA_FD_NEW;
	ctx->cur_offset = 0;
	ctx->cur_size = 0;
	ctx->next_offset = 0;
        ctx->op_errno = 0;
	gf_dirent_free(&ctx->entries);//free空间
}

/*
 * Check whether we can handle a request. Offset verification is done by the
 * caller, so we only check whether the preload buffer has completion status
 * (including an error) or has some data to return. 检查是否能够处理一个请求，主要是检查加载的buffer是否是完成状态
 */
static gf_boolean_t
rda_can_serve_readdirp(struct rda_fd_ctx *ctx, size_t request_size)
{
	if ((ctx->state & RDA_FD_EOD) ||
	    (ctx->state & RDA_FD_ERROR) ||
	    (!(ctx->state & RDA_FD_PLUGGED) && (ctx->cur_size > 0)))
		return _gf_true;

	return _gf_false;
}

/*
 * Serve a request from the fd dentry list based on the size of the request
 * buffer. ctx must be locked.
 * 处理来自基于该请求缓冲区大小的目录项链表的一个请求
 */
static int32_t
__rda_serve_readdirp(xlator_t *this, gf_dirent_t *entries, size_t request_size,
		   struct rda_fd_ctx *ctx)
{
	gf_dirent_t *dirent, *tmp;
	size_t dirent_size, size = 0;
	int32_t count = 0;
	struct rda_priv *priv = this->private;
/*
	rda_local *local = NULL;
	local = mem_get0(this->local_pool);

*/
	

//循环获取一个一个目录项，当dirent_size大于请求大小时，跳出循环，不在获取目录项
	list_for_each_entry_safe(dirent, tmp, &ctx->entries.list, list) {//dirent 为遍历指针，tmp缓存遍历指针 ctx->entries.list为要遍历的链表的头结点 
	
		dirent_size = gf_dirent_size(dirent->d_name);
		if (size + dirent_size > request_size)
			break;

		size += dirent_size;
		//删除并初始化
		list_del_init(&dirent->list);//不但从链表中删除节点，还把这个节点的向前向后指针都指向自己，即初始化。
		ctx->cur_size -= dirent_size;

		//将dirent->list代表的list_head加到entries->list代表的list_head头部 
		list_add_tail(&dirent->list, &entries->list);
		ctx->cur_offset = dirent->d_off;//记录读取到哪一个目录项，接下来ctx应该读取的目录项位置
		count++;
	}

	if (ctx->cur_size <= priv->rda_low_wmark)//rda_low_wmark 低于该值 则阻塞
		ctx->state |= RDA_FD_PLUGGED;

	return count;
}

static int32_t
rda_readdirp_stub(call_frame_t *frame, xlator_t *this, fd_t *fd, size_t size,
		  off_t offset, dict_t *xdata)
{
	gf_dirent_t entries;
	int32_t ret;
	struct rda_fd_ctx *ctx;
        int op_errno = 0;

	ctx = get_rda_fd_ctx(fd, this);
	//初始化链表
	INIT_LIST_HEAD(&entries.list);
	//即将获得目录项count给ret
	ret = __rda_serve_readdirp(this, &entries, size, ctx);

	if (!ret && (ctx->state & RDA_FD_ERROR)) {
		ret = -1;
		ctx->state &= ~RDA_FD_ERROR;

		/*
		 * the preload has stopped running in the event of an error, so
		 * pass all future requests along
		 * 出错即bypass
		 */
		ctx->state |= RDA_FD_BYPASS;
	}

        /*
         * Use the op_errno sent by lower layers as xlators above will check
         * the op_errno for identifying whether readdir is completed or not.
         * 上层Xlator通过较低层发送的op_errno来判断readdir是否完成与否
         */
        op_errno = ctx->op_errno;

	STACK_UNWIND_STRICT(readdirp, frame, ret, op_errno, &entries, xdata);
	gf_dirent_free(&entries);

	return 0;
}

static int32_t
rda_readdirp(call_frame_t *frame, xlator_t *this, fd_t *fd, size_t size,
	     off_t off, dict_t *xdata)
{
	struct rda_fd_ctx *ctx;
	call_stub_t *stub;
	int fill = 0;

	ctx = get_rda_fd_ctx(fd, this);
	if (!ctx)
		goto err;

	if (ctx->state & RDA_FD_BYPASS)
		goto bypass;

	LOCK(&ctx->lock);

	/* recheck now that we have the lock */
	if (ctx->state & RDA_FD_BYPASS) {
		UNLOCK(&ctx->lock);
		goto bypass;
	}

	/*
	 * If a new read comes in at offset 0 and the buffer has been
	 * completed, reset the context and kickstart the filler again.
	 */
	if (!off && (ctx->state & RDA_FD_EOD) && (ctx->cur_size == 0)) {
		rda_reset_ctx(ctx);
		fill = 1;
	}

	/*
	 * If a readdir occurs at an unexpected offset or we already have a
	 * request pending, admit defeat and just get out of the way.
	 */
	if (off != ctx->cur_offset || ctx->stub) {
		ctx->state |= RDA_FD_BYPASS;
		UNLOCK(&ctx->lock);
		goto bypass;
	}

	stub = fop_readdirp_stub(frame, rda_readdirp_stub, fd, size, off, xdata);
	if (!stub) {
		UNLOCK(&ctx->lock);
		goto err;
	}

	/*
	 * If we haven't bypassed the preload, this means we can either serve
	 * the request out of the preload or the request that enables us to do
	 * so is in flight...
	 */
	if (rda_can_serve_readdirp(ctx, size))
		call_resume(stub);
	else
		ctx->stub = stub;

	UNLOCK(&ctx->lock);

	if (fill)
		rda_fill_fd(frame, this, fd);

	return 0;

bypass:
	STACK_WIND(frame, default_readdirp_cbk, FIRST_CHILD(this),
		   FIRST_CHILD(this)->fops->readdirp, fd, size, off, xdata);
	return 0;

err:
	STACK_UNWIND_STRICT(readdirp, frame, -1, ENOMEM, NULL, NULL);
	return 0;
}

static int32_t
rda_fill_fd_cbk(call_frame_t *frame, void *cookie, xlator_t *this,
		 int32_t op_ret, int32_t op_errno, gf_dirent_t *entries,
		 dict_t *xdata)
{
	gf_dirent_t *dirent, *tmp;
	struct rda_local *local = frame->local;//用来储存该translatro特定的上下文
	struct rda_fd_ctx *ctx = local->ctx;
	struct rda_priv *priv = this->private;
	int fill = 1;

	LOCK(&ctx->lock);

	/* Verify that the preload buffer is still pending on this data. */
	if (ctx->next_offset != local->offset) {
		gf_msg(this->name, GF_LOG_ERROR,
                       0, READDIR_AHEAD_MSG_OUT_OF_SEQUENCE,
                       "Out of sequence directory preload.");
		ctx->state |= (RDA_FD_BYPASS|RDA_FD_ERROR);
		ctx->op_errno = EUCLEAN;

		goto out;
	}

	if (entries) {
		list_for_each_entry_safe(dirent, tmp, &entries->list, list) {
			list_del_init(&dirent->list);
			/* must preserve entry order */
			list_add_tail(&dirent->list, &ctx->entries.list);

			ctx->cur_size += gf_dirent_size(dirent->d_name);
			ctx->next_offset = dirent->d_off;
		}
	}

	if (ctx->cur_size >= priv->rda_high_wmark)
		ctx->state &= ~RDA_FD_PLUGGED;

	if (!op_ret) {
		/* we've hit eod */
		ctx->state &= ~RDA_FD_RUNNING;
		ctx->state |= RDA_FD_EOD;
                ctx->op_errno = op_errno;
	} else if (op_ret == -1) {
		/* kill the preload and pend the error */
		ctx->state &= ~RDA_FD_RUNNING;
		ctx->state |= RDA_FD_ERROR;
		ctx->op_errno = op_errno;
	}

	/*
	 * NOTE: The strict bypass logic in readdirp() means a pending request
	 * is always based on ctx->cur_offset.
	 */
	if (ctx->stub &&
	    rda_can_serve_readdirp(ctx, ctx->stub->args.size)) {
		call_resume(ctx->stub);
		ctx->stub = NULL;
	}

out:
	/*
	 * If we have been marked for bypass and have no pending stub, clear the
	 * run state so we stop preloading the context with entries.
	 */
	if ((ctx->state & RDA_FD_BYPASS) && !ctx->stub)
		ctx->state &= ~RDA_FD_RUNNING;

	if (!(ctx->state & RDA_FD_RUNNING)) {
		fill = 0;
		STACK_DESTROY(ctx->fill_frame->root);
		ctx->fill_frame = NULL;
	}

	UNLOCK(&ctx->lock);

	if (fill)
		rda_fill_fd(frame, this, local->fd);

	return 0;
}

/*
 * Start prepopulating the fd context with directory entries.
 */
static int
rda_fill_fd(call_frame_t *frame, xlator_t *this, fd_t *fd)
{
	call_frame_t *nframe = NULL;
	struct rda_local *local = NULL;
	struct rda_fd_ctx *ctx;
	off_t offset;
	struct rda_priv *priv = this->private;

	ctx = get_rda_fd_ctx(fd, this);
	if (!ctx)
		goto err;

	LOCK(&ctx->lock);

	if (ctx->state & RDA_FD_NEW) {
		ctx->state &= ~RDA_FD_NEW;
		ctx->state |= RDA_FD_RUNNING;
		if (priv->rda_low_wmark)
			ctx->state |= RDA_FD_PLUGGED;
	}

	offset = ctx->next_offset;

	if (!ctx->fill_frame) {
		nframe = copy_frame(frame);
		if (!nframe) {
			UNLOCK(&ctx->lock);
			goto err;
		}

		local = mem_get0(this->local_pool);
		if (!local) {
			UNLOCK(&ctx->lock);
			goto err;
		}

		local->ctx = ctx;
		local->fd = fd;
		nframe->local = local;

		ctx->fill_frame = nframe;
	} else {
		nframe = ctx->fill_frame;
		local = nframe->local;
	}

	local->offset = offset;

	UNLOCK(&ctx->lock);

	STACK_WIND(nframe, rda_fill_fd_cbk, FIRST_CHILD(this),
		   FIRST_CHILD(this)->fops->readdirp, fd, priv->rda_req_size,
		   offset, NULL);

	return 0;

err:
	if (nframe)
		FRAME_DESTROY(nframe);

	return -1;
}

static int32_t
rda_opendir_cbk(call_frame_t *frame, void *cookie, xlator_t *this,
		    int32_t op_ret, int32_t op_errno, fd_t *fd, dict_t *xdata)
{
	if (!op_ret)
		rda_fill_fd(frame, this, fd);

	STACK_UNWIND_STRICT(opendir, frame, op_ret, op_errno, fd, xdata);
	return 0;
}

static int32_t
rda_opendir(call_frame_t *frame, xlator_t *this, loc_t *loc, fd_t *fd,
		dict_t *xdata)
{
	STACK_WIND(frame, rda_opendir_cbk, FIRST_CHILD(this),
		   FIRST_CHILD(this)->fops->opendir, loc, fd, xdata);
	return 0;
}

static int32_t
rda_releasedir(xlator_t *this, fd_t *fd)
{
	uint64_t val;
	struct rda_fd_ctx *ctx;

	if (fd_ctx_del(fd, this, &val) < 0)
		return -1;

	ctx = (struct rda_fd_ctx *) val;
	if (!ctx)
		return 0;

	rda_reset_ctx(ctx);

	if (ctx->fill_frame)
		STACK_DESTROY(ctx->fill_frame->root);

	if (ctx->stub)
		gf_msg(this->name, GF_LOG_ERROR, 0,
		        READDIR_AHEAD_MSG_DIR_RELEASE_PENDING_STUB,
                       "released a directory with a pending stub");

	GF_FREE(ctx);
	return 0;
}

int32_t
mem_acct_init(xlator_t *this)
{
	int ret = -1;

	if (!this)
		goto out;

	ret = xlator_mem_acct_init(this, gf_rda_mt_end + 1);

	if (ret != 0)
		gf_msg(this->name, GF_LOG_ERROR, ENOMEM,
                       READDIR_AHEAD_MSG_NO_MEMORY, "Memory accounting init"
		       "failed");

out:
	return ret;
}

int
reconfigure(xlator_t *this, dict_t *options)
{
	struct rda_priv *priv = this->private;

	GF_OPTION_RECONF("rda-request-size", priv->rda_req_size, options,
			 uint32, err);
	GF_OPTION_RECONF("rda-low-wmark", priv->rda_low_wmark, options, size_uint64,
			 err);
	GF_OPTION_RECONF("rda-high-wmark", priv->rda_high_wmark, options, size_uint64,
			 err);

	return 0;
err:
	return -1;
}

int
init(xlator_t *this)
{
	struct rda_priv *priv = NULL;

        GF_VALIDATE_OR_GOTO("readdir-ahead", this, err);

        if (!this->children || this->children->next) {
                gf_msg(this->name,  GF_LOG_ERROR, 0,
                        READDIR_AHEAD_MSG_XLATOR_CHILD_MISCONFIGURED,
                        "FATAL: readdir-ahead not configured with exactly one"
                        " child");
                goto err;
        }

        if (!this->parents) {
                gf_msg(this->name, GF_LOG_WARNING, 0,
                        READDIR_AHEAD_MSG_VOL_MISCONFIGURED,
                        "dangling volume. check volfile ");
        }

	priv = GF_CALLOC(1, sizeof(struct rda_priv), gf_rda_mt_rda_priv);
	if (!priv)
		goto err;
	this->private = priv;

	this->local_pool = mem_pool_new(struct rda_local, 32);
	if (!this->local_pool)
		goto err;

	GF_OPTION_INIT("rda-request-size", priv->rda_req_size, uint32, err);
	GF_OPTION_INIT("rda-low-wmark", priv->rda_low_wmark, size_uint64, err);
	GF_OPTION_INIT("rda-high-wmark", priv->rda_high_wmark, size_uint64, err);

	return 0;

err:
	if (this->local_pool)
		mem_pool_destroy(this->local_pool);
	if (priv)
		GF_FREE(priv);

        return -1;
}


void
fini(xlator_t *this)
{
        GF_VALIDATE_OR_GOTO ("readdir-ahead", this, out);

	GF_FREE(this->private);

out:
        return;
}

struct xlator_fops fops = {
	.opendir	= rda_opendir,
	.readdirp	= rda_readdirp,
};

struct xlator_cbks cbks = {
	.releasedir	= rda_releasedir,
};

struct volume_options options[] = {
	{ .key = {"rda-request-size"},
	  .type = GF_OPTION_TYPE_INT,
	  .min = 4096,
	  .max = 131072,
	  .default_value = "131072",
	  .description = "readdir-ahead request size",
	},
	{ .key = {"rda-low-wmark"},
	  .type = GF_OPTION_TYPE_SIZET,
	  .min = 0,
	  .max = 10 * GF_UNIT_MB,
	  .default_value = "4096",
	  .description = "the value under which we plug",
	},
	{ .key = {"rda-high-wmark"},
	  .type = GF_OPTION_TYPE_SIZET,
	  .min = 0,
	  .max = 100 * GF_UNIT_MB,
	  .default_value = "131072",
	  .description = "the value over which we unplug",
	},
        { .key = {NULL} },
};

