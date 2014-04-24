/*
 * This file is part of the zlog Library.
 *
 * Copyright (C) 2011 by Hardy Simpson <HardySimpson1984@gmail.com>
 *
 * Licensed under the LGPL v2.1, see the file COPYING in base directory.
 */

#include "fmacros.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>

#include "conf.h"
#include "category_table.h"
#include "record_table.h"
#include "mdc.h"
#include "zc_defs.h"
#include "rule.h"
#include "version.h"

/*******************************************************************************/
extern char *zlog_git_sha1;
/*******************************************************************************/
static pthread_rwlock_t zlog_env_lock = PTHREAD_RWLOCK_INITIALIZER;
zlog_conf_t *zlog_env_conf;
static pthread_key_t zlog_thread_key;
static zc_hashtable_t *zlog_env_categories;
static zc_hashtable_t *zlog_env_records;
static zlog_category_t *zlog_default_category;
static size_t zlog_env_reload_conf_count;
static int zlog_env_is_init = 0;
static int zlog_env_init_version = 0;
/*******************************************************************************/
/* inner no need thread-safe */
static void zlog_fini_inner(void)
{
	/* pthread_key_delete(zlog_thread_key); */
	/* never use pthread_key_delete,
	 * it will cause other thread can't release zlog_thread_t 
	 * after one thread call pthread_key_delete
	 * also key not init will cause a core dump
	 */
	
	if (zlog_env_categories) zlog_category_table_del(zlog_env_categories);
	zlog_env_categories = NULL;
	zlog_default_category = NULL;
	if (zlog_env_records) zlog_record_table_del(zlog_env_records);
	zlog_env_records = NULL;
	if (zlog_env_conf) zlog_conf_del(zlog_env_conf);
	zlog_env_conf = NULL;
	return;
}

static void zlog_clean_rest_thread(void)
{
	zlog_thread_t *a_thread;
	a_thread = pthread_getspecific(zlog_thread_key);
	if (!a_thread) return;
	zlog_thread_del(a_thread);
	return;
}

static int zlog_init_inner(const char *confpath)
{
	int rc = 0;

	/* the 1st time in the whole process do init */
	if (zlog_env_init_version == 0) {
		/* clean up is done by OS when a thread call pthread_exit */
		rc = pthread_key_create(&zlog_thread_key, (void (*) (void *)) zlog_thread_del);
		if (rc) {
			zc_error("pthread_key_create fail, rc[%d]", rc);
			goto err;
		}

		/* if some thread do not call pthread_exit, like main thread
		 * atexit will clean it 
		 */
		rc = atexit(zlog_clean_rest_thread);
		if (rc) {
			zc_error("atexit fail, rc[%d]", rc);
			goto err;
		}
		zlog_env_init_version++;
	} /* else maybe after zlog_fini() and need not create pthread_key */

	zlog_env_conf = zlog_conf_new(confpath);
	if (!zlog_env_conf) {
		zc_error("zlog_conf_new[%s] fail", confpath);
		goto err;
	}

	zlog_env_categories = zlog_category_table_new();
	if (!zlog_env_categories) {
		zc_error("zlog_category_table_new fail");
		goto err;
	}

	zlog_env_records = zlog_record_table_new();
	if (!zlog_env_records) {
		zc_error("zlog_record_table_new fail");
		goto err;
	}

	return 0;
err:
	zlog_fini_inner();
	return -1;
}

/*******************************************************************************/
/*
 * @brief zlog_init()从配置文件confpath中读取配置信息到内存
 *
 * 如果confpath为NULL, 会寻找环境变量ZLOG_CONF_PATH的值作为配置文件名.
 * 如果环境变量ZLOG_CONF_PATH也没有, 所有日志以内置格式写到标准输出上.
 * 每个进程只有第一次调用zlog_init()是有效的, 后面的多余调用都会失败并不做任何事情.
 * @param[in] confpath: 配置文件路径
 *
 * @return 0: 成功 / -1: 失败. 详细错误会被写在由环境变量ZLOG_PROFILE_ERROR指定的错误日志里面
 */
int zlog_init(const char *confpath)
{
	int rc;
	zc_debug("------zlog_init start------");
	zc_debug("------compile time[%s %s], version[%s]------", __DATE__, __TIME__, ZLOG_VERSION);

	rc = pthread_rwlock_wrlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_wrlock fail, rc[%d]", rc);
		return -1;
	}

	if (zlog_env_is_init) {
		zc_error("already init, use zlog_reload pls");
		goto err;
	}


	if (zlog_init_inner(confpath)) {
		zc_error("zlog_init_inner[%s] fail", confpath);
		goto err;
	}

	zlog_env_is_init = 1;
	zlog_env_init_version++;

	zc_debug("------zlog_init success end------");
	rc = pthread_rwlock_unlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_unlock fail, rc=[%d]", rc);
		return -1;
	}
	return 0;
err:
	zc_error("------zlog_init fail end------");
	rc = pthread_rwlock_unlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_unlock fail, rc=[%d]", rc);
		return -1;
	}
	return -1;
}

/*
 * dzlog是忽略分类(zlog_category_t)的一组简单zlog接口. 他采用内置的一个默认分类, 这个分类置于锁的保护下.这些接口也是线程安全的.
 * 忽略了分类, 意味着用户不需要操心创建, 存储, 传输zlog_category_t类型的变量.
 * 当然也可以在用dzlog接口的同时用一般的zlog接口函数, 这样会更爽.
 * dzlog_init()和zlog_init()一样做初始化, 就是多需要一个默认分类名cname的参数.
 * zlog_reload(), zlog_fini() 可以和以前一样使用, 用来刷新配置, 或者清理.
 *
 * @return 0: 成功 / -1: 失败
 * 详细错误会被写在由环境变量ZLOG_PROFILE_ERROR指定的错误日志里面.
 */

int dzlog_init(const char *confpath, const char *cname)
{
	int rc = 0;
	zc_debug("------dzlog_init start------");
	zc_debug("------compile time[%s %s], version[%s]------",
			__DATE__, __TIME__, ZLOG_VERSION);

	rc = pthread_rwlock_wrlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_wrlock fail, rc[%d]", rc);
		return -1;
	}

	if (zlog_env_is_init) {
		zc_error("already init, use zlog_reload pls");
		goto err;
	}

	if (zlog_init_inner(confpath)) {
		zc_error("zlog_init_inner[%s] fail", confpath);
		goto err;
	}

	zlog_default_category = zlog_category_table_fetch_category(
				zlog_env_categories,
				cname,
				zlog_env_conf->rules);
	if (!zlog_default_category) {
		zc_error("zlog_category_table_fetch_category[%s] fail", cname);
		goto err;
	}

	zlog_env_is_init = 1;
	zlog_env_init_version++;

	zc_debug("------dzlog_init success end------");
	rc = pthread_rwlock_unlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_unlock fail, rc=[%d]", rc);
		return -1;
	}
	return 0;
err:
	zc_error("------dzlog_init fail end------");
	rc = pthread_rwlock_unlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_unlock fail, rc=[%d]", rc);
		return -1;
	}
	return -1;
}
/*******************************************************************************/

/*
 * @brief 从confpath重载配置, 并根据这个配置文件来重计算内部的分类规则匹配, 重建每个线程的缓存, 并设置原有的用户自定义输出函数.
 * 如果confpath为NULL, 会重载上一次zlog_init()或者zlog_reload()使用的配置文件.
 * 如果zlog_reload()失败, 上一次的配置依然有效, 所以zlog_reload()具有原子性.
 *
 * @param[in] confpath: 配置文件路径
 *
 * @return 0: 成功 / -1: 失败. 详细错误会被写在由环境变量ZLOG_PROFILE_ERROR指定的错误日志里面
 */
int zlog_reload(const char *confpath)
{
	int rc = 0;
	int i = 0;
	zlog_conf_t *new_conf = NULL;
	zlog_rule_t *a_rule;
	int c_up = 0;

	zc_debug("------zlog_reload start------");
	rc = pthread_rwlock_wrlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_wrlock fail, rc[%d]", rc);
		return -1;
	}

	if (!zlog_env_is_init) {
		zc_error("never call zlog_init() or dzlog_init() before");
		goto quit;
	}

	/* use last conf file */
	if (confpath == NULL) confpath = zlog_env_conf->file;

	/* reach reload period */
	if (confpath == (char*)-1) {
		/* test again, avoid other threads already reloaded */
		if (zlog_env_reload_conf_count > zlog_env_conf->reload_conf_period) {
			confpath = zlog_env_conf->file;
		} else {
			/* do nothing, already done */
			goto quit;
		}
	}

	/* reset counter, whether automaticlly or mannually */
	zlog_env_reload_conf_count = 0;

	new_conf = zlog_conf_new(confpath);
	if (!new_conf) {
		zc_error("zlog_conf_new fail");
		goto err;
	}

	zc_arraylist_foreach(new_conf->rules, i, a_rule) {
		zlog_rule_set_record(a_rule, zlog_env_records);
	}

	if (zlog_category_table_update_rules(zlog_env_categories, new_conf->rules)) {
		c_up = 0;
		zc_error("zlog_category_table_update fail");
		goto err;
	} else {
		c_up = 1;
	}

	zlog_env_init_version++;

	if (c_up) zlog_category_table_commit_rules(zlog_env_categories);
	zlog_conf_del(zlog_env_conf);
	zlog_env_conf = new_conf;
	zc_debug("------zlog_reload success, total init verison[%d] ------", zlog_env_init_version);
	rc = pthread_rwlock_unlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_unlock fail, rc=[%d]", rc);
		return -1;
	}
	return 0;
err:
	/* fail, roll back everything */
	zc_warn("zlog_reload fail, use old conf file, still working");
	if (new_conf) zlog_conf_del(new_conf);
	if (c_up) zlog_category_table_rollback_rules(zlog_env_categories);
	zc_error("------zlog_reload fail, total init version[%d] ------", zlog_env_init_version);
	rc = pthread_rwlock_unlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_unlock fail, rc=[%d]", rc);
		return -1;
	}
	return -1;
quit:
	zc_debug("------zlog_reload do nothing------");
	rc = pthread_rwlock_unlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_unlock fail, rc=[%d]", rc);
		return -1;
	}
	return 0;
}
/*******************************************************************************/
/*
 * @brief 清理所有zlog API申请的内存, 关闭它们打开的文件. 使用次数不限.
 *
 * @return 0: 成功 / -1: 失败
 * 详细错误会被写在由环境变量ZLOG_PROFILE_ERROR指定的错误日志里面.
 */
void zlog_fini(void)
{
	int rc = 0;

	zc_debug("------zlog_fini start------");
	rc = pthread_rwlock_wrlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_wrlock fail, rc[%d]", rc);
		return;
	}

	if (!zlog_env_is_init) {
		zc_error("before finish, must zlog_init() or dzlog_init() fisrt");
		goto exit;
	}

	zlog_fini_inner();
	zlog_env_is_init = 0;

exit:
	zc_debug("------zlog_fini end------");
	rc = pthread_rwlock_unlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_unlock fail, rc=[%d]", rc);
		return;
	}
	return;
}
/*******************************************************************************/
/*
 * @brief 从zlog的全局分类表里面找到分类, 用于以后输出日志, 如果没有的话, 就建一个.
 * 然后它会遍历所有的规则, 寻找和cname匹配的规则并绑定.
 *
 * @return 非NULL: zlog_category_t的指针 / NULL: 失败, 详细错误会被写在由环境变量ZLOG_PROFILE_ERROR指定的错误日志里面
 */
zlog_category_t *zlog_get_category(const char *cname)
{
	int rc = 0;
	zlog_category_t *a_category = NULL;

	zc_assert(cname, NULL);
	zc_debug("------zlog_get_category[%s] start------", cname);
	rc = pthread_rwlock_wrlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_wrlock fail, rc[%d]", rc);
		return NULL;
	}

	if (!zlog_env_is_init) {
		zc_error("never call zlog_init() or dzlog_init() before");
		a_category = NULL;
		goto err;
	}

	a_category = zlog_category_table_fetch_category(
				zlog_env_categories,
				cname,
				zlog_env_conf->rules);
	if (!a_category) {
		zc_error("zlog_category_table_fetch_category[%s] fail", cname);
		goto err;
	}

	zc_debug("------zlog_get_category[%s] success, end------ ", cname);
	rc = pthread_rwlock_unlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_unlock fail, rc=[%d]", rc);
		return NULL;
	}
	return a_category;
err:
	zc_error("------zlog_get_category[%s] fail, end------ ", cname);
	rc = pthread_rwlock_unlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_unlock fail, rc=[%d]", rc);
		return NULL;
	}
	return NULL;
}

/*
 * @brief 是用来改变默认分类用的. 上一个分类会被替换成新的. 同样不用担心内存释放的问题, zlog_fini()最后会清理.
 *
 * @return 0: 成功 / -1: 失败
 * 详细错误会被写在由环境变量ZLOG_PROFILE_ERROR指定的错误日志里面.
 */
int dzlog_set_category(const char *cname)
{
	int rc = 0;
	zc_assert(cname, -1);

	zc_debug("------dzlog_set_category[%s] start------", cname);
	rc = pthread_rwlock_wrlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_wrlock fail, rc[%d]", rc);
		return -1;
	}

	if (!zlog_env_is_init) {
		zc_error("never call zlog_init() or dzlog_init() before");
		goto err;
	}

	zlog_default_category = zlog_category_table_fetch_category(
				zlog_env_categories,
				cname,
				zlog_env_conf->rules);
	if (!zlog_default_category) {
		zc_error("zlog_category_table_fetch_category[%s] fail", cname);
		goto err;
	}

	zc_debug("------dzlog_set_category[%s] end, success------ ", cname);
	rc = pthread_rwlock_unlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_unlock fail, rc=[%d]", rc);
		return -1;
	}
	return 0;
err:
	zc_error("------dzlog_set_category[%s] end, fail------ ", cname);
	rc = pthread_rwlock_unlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_unlock fail, rc=[%d]", rc);
		return -1;
	}
	return -1;
}
/*******************************************************************************/
#define zlog_fetch_thread(a_thread, fail_goto) do {  \
	int rd = 0;  \
	a_thread = pthread_getspecific(zlog_thread_key);  \
	if (!a_thread) {  \
		a_thread = zlog_thread_new(zlog_env_init_version,  \
				zlog_env_conf->buf_size_min, zlog_env_conf->buf_size_max, \
				zlog_env_conf->time_cache_count); \
		if (!a_thread) {  \
			zc_error("zlog_thread_new fail");  \
			goto fail_goto;  \
		}  \
  \
		rd = pthread_setspecific(zlog_thread_key, a_thread);  \
		if (rd) {  \
			zlog_thread_del(a_thread);  \
			zc_error("pthread_setspecific fail, rd[%d]", rd);  \
			goto fail_goto;  \
		}  \
	}  \
  \
	if (a_thread->init_version != zlog_env_init_version) {  \
		/* as mdc is still here, so can not easily del and new */ \
		rd = zlog_thread_rebuild_msg_buf(a_thread, \
				zlog_env_conf->buf_size_min, \
				zlog_env_conf->buf_size_max);  \
		if (rd) {  \
			zc_error("zlog_thread_resize_msg_buf fail, rd[%d]", rd);  \
			goto fail_goto;  \
		}  \
  \
		rd = zlog_thread_rebuild_event(a_thread, zlog_env_conf->time_cache_count);  \
		if (rd) {  \
			zc_error("zlog_thread_resize_msg_buf fail, rd[%d]", rd);  \
			goto fail_goto;  \
		}  \
		a_thread->init_version = zlog_env_init_version;  \
	}  \
} while (0)

/*******************************************************************************/
// MDC操作
// MDC(Mapped Diagnostic Context)是一个每线程拥有的键-值表, 所以和分类没什么关系.
// key和value是字符串, 长度不能超过MAXLEN_PATH(1024). 如果超过MAXLEN_PATH(1024)的话, 会被截断.
// 记住这个表是和线程绑定的, 每个线程有自己的表, 所以在一个线程内的调用不会影响其他线程.

/*
 * @brief 设置MDC
 *
 * @return 0: 成功 / -1: 失败
 * 如果有错误发生, 详细错误会被写在由环境变量ZLOG_PROFILE_ERROR指定的错误日志里面.
 */
int zlog_put_mdc(const char *key, const char *value)
{
	int rc = 0;
	zlog_thread_t *a_thread;

	zc_assert(key, -1);
	zc_assert(value, -1);

	rc = pthread_rwlock_rdlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_wrlock fail, rc[%d]", rc);
		return -1;
	}

	if (!zlog_env_is_init) {
		zc_error("never call zlog_init() or dzlog_init() before");
		goto err;
	}

	zlog_fetch_thread(a_thread, err);

	if (zlog_mdc_put(a_thread->mdc, key, value)) {
		zc_error("zlog_mdc_put fail, key[%s], value[%s]", key, value);
		goto err;
	}

	rc = pthread_rwlock_unlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_unlock fail, rc=[%d]", rc);
		return -1;
	}
	return 0;
err:
	rc = pthread_rwlock_unlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_unlock fail, rc=[%d]", rc);
		return -1;
	}
	return -1;
}

/*
 * @brief 获取MDC
 *
 * @return 非空: value的指针 / NULL: 失败或者没有相应的key
 * 如果有错误发生, 详细错误会被写在由环境变量ZLOG_PROFILE_ERROR指定的错误日志里面.
 */
char *zlog_get_mdc(char *key)
{
	int rc = 0;
	char *value = NULL;
	zlog_thread_t *a_thread;

	zc_assert(key, NULL);

	rc = pthread_rwlock_rdlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_rdlock fail, rc[%d]", rc);
		return NULL;
	}

	if (!zlog_env_is_init) {
		zc_error("never call zlog_init() or dzlog_init() before");
		goto err;
	}

	a_thread = pthread_getspecific(zlog_thread_key);
	if (!a_thread) {
		zc_error("thread not found, maybe not use zlog_put_mdc before");
		goto err;
	}

	value = zlog_mdc_get(a_thread->mdc, key);
	if (!value) {
		zc_error("key[%s] not found in mdc", key);
		goto err;
	}

	rc = pthread_rwlock_unlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_unlock fail, rc=[%d]", rc);
		return NULL;
	}
	return value;
err:
	rc = pthread_rwlock_unlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_unlock fail, rc=[%d]", rc);
		return NULL;
	}
	return NULL;
}

void zlog_remove_mdc(char *key)
{
	int rc = 0;
	zlog_thread_t *a_thread;

	zc_assert(key, );

	rc = pthread_rwlock_rdlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_rdlock fail, rc[%d]", rc);
		return;
	}

	if (!zlog_env_is_init) {
		zc_error("never call zlog_init() or dzlog_init() before");
		goto exit;
	}

	a_thread = pthread_getspecific(zlog_thread_key);
	if (!a_thread) {
		zc_error("thread not found, maybe not use zlog_put_mdc before");
		goto exit;
	}

	zlog_mdc_remove(a_thread->mdc, key);

exit:
	rc = pthread_rwlock_unlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_unlock fail, rc=[%d]", rc);
		return;
	}
	return;
}

void zlog_clean_mdc(void)
{
	int rc = 0;
	zlog_thread_t *a_thread;

	rc = pthread_rwlock_rdlock(&zlog_env_lock);
	if (rc) {;
		zc_error("pthread_rwlock_rdlock fail, rc[%d]", rc);
		return;
	}

	if (!zlog_env_is_init) {
		zc_error("never call zlog_init() or dzlog_init() before");
		goto exit;
	}

	a_thread = pthread_getspecific(zlog_thread_key);
	if (!a_thread) {
		zc_error("thread not found, maybe not use zlog_put_mdc before");
		goto exit;
	}

	zlog_mdc_clean(a_thread->mdc);

exit:
	rc = pthread_rwlock_unlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_unlock fail, rc=[%d]", rc);
		return;
	}
	return;
}

/*******************************************************************************/
/*
 * @brief 写日志函数, 输入的数据对应于配置文件中的%m, category来自于调用zlog_get_category()
 * vzlog()根据format输出, 就像vprintf(3).
 * vzlog()相当于zlog(), 只是它用一个va_list类型的参数args, 而不是一堆类型不同的参数.
 * vzlog()内部使用了va_copy宏, args的内容在vzlog()后保持不变, 可以参考stdarg(3).
 */
void vzlog(zlog_category_t * category,
	const char *file, size_t filelen,
	const char *func, size_t funclen,
	long line, int level,
	const char *format, va_list args)
{
	zlog_thread_t *a_thread;

	/* The bitmap determination here is not under the protection of rdlock.
	 * It may be changed by other CPU by zlog_reload() halfway.
	 *
	 * Old or strange value may be read here,
	 * but it is safe, the bitmap is valid as long as category exist,
	 * And will be the right value after zlog_reload()
	 *
	 * For speed up, if one log will not be ouput,
	 * There is no need to aquire rdlock.
	 */
	if (zlog_category_needless_level(category, level)) return;

	pthread_rwlock_rdlock(&zlog_env_lock);

	if (!zlog_env_is_init) {
		zc_error("never call zlog_init() or dzlog_init() before");
		goto exit;
	}

	zlog_fetch_thread(a_thread, exit);

	zlog_event_set_fmt(a_thread->event,
		category->name, category->name_len,
		file, filelen, func, funclen, line, level,
		format, args);

	if (zlog_category_output(category, a_thread)) {
		zc_error("zlog_output fail, srcfile[%s], srcline[%ld]", file, line);
		goto exit;
	}

	if (zlog_env_conf->reload_conf_period &&
		++zlog_env_reload_conf_count > zlog_env_conf->reload_conf_period ) {
		/* under the protection of lock read env conf */
		goto reload;
	}

exit:
	pthread_rwlock_unlock(&zlog_env_lock);
	return;
reload:
	pthread_rwlock_unlock(&zlog_env_lock);
	/* will be wrlock, so after unlock */
	if (zlog_reload((char *)-1)) {
		zc_error("reach reload-conf-period but zlog_reload fail, zlog-chk-conf [file] see detail");
	}
	return;
}

/*
 * @brief 写日志函数, 输入的数据对应于配置文件中的%m, category来自于调用zlog_get_category()
 * hzlog()有点不一样, 它产生下面这样的输出, 长度为buf_len的内存buf以16进制的形式表示出来. h表示hex
hex_buf_len=[5365]
             0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F      0123456789ABCDEF

0000000001   23 21 20 2f 62 69 6e 2f 62 61 73 68 0a 0a 23 20   #! /bin/bash..#

0000000002   74 65 73 74 5f 68 65 78 20 2d 20 74 65 6d 70 6f   test_hex - tempo

0000000003   72 61 72 79 20 77 72 61 70 70 65 72 20 73 63 72   rary wrapper scr
 * 参数file和line填写为__FILE__和__LINE__这两个宏. 这两个宏标识日志是在哪里发生的.
 * 参数func填写为__func__或者__FUNCTION__, 如果编译器支持的话, 如果不支持, 就填写为"<unkown>".
 */
void hzlog(zlog_category_t *category,
	const char *file, size_t filelen,
	const char *func, size_t funclen,
	long line, int level,
	const void *buf, size_t buflen)
{
	zlog_thread_t *a_thread;

	if (zlog_category_needless_level(category, level)) return;

	pthread_rwlock_rdlock(&zlog_env_lock);

	if (!zlog_env_is_init) {
		zc_error("never call zlog_init() or dzlog_init() before");
		goto exit;
	}

	zlog_fetch_thread(a_thread, exit);

	zlog_event_set_hex(a_thread->event,
		category->name, category->name_len,
		file, filelen, func, funclen, line, level,
		buf, buflen);

	if (zlog_category_output(category, a_thread)) {
		zc_error("zlog_output fail, srcfile[%s], srcline[%ld]", file, line);
		goto exit;
	}

	if (zlog_env_conf->reload_conf_period &&
		++zlog_env_reload_conf_count > zlog_env_conf->reload_conf_period ) {
		/* under the protection of lock read env conf */
		goto reload;
	}

exit:
	pthread_rwlock_unlock(&zlog_env_lock);
	return;
reload:
	pthread_rwlock_unlock(&zlog_env_lock);
	/* will be wrlock, so after unlock */
	if (zlog_reload((char *)-1)) {
		zc_error("reach reload-conf-period but zlog_reload fail, zlog-chk-conf [file] see detail");
	}
	return;
}

/*******************************************************************************/
/* for speed up, copy from vzlog */
void vdzlog(const char *file, size_t filelen,
	const char *func, size_t funclen,
	long line, int level,
	const char *format, va_list args)
{
	zlog_thread_t *a_thread;

	if (zlog_category_needless_level(zlog_default_category, level)) return;

	pthread_rwlock_rdlock(&zlog_env_lock);

	if (!zlog_env_is_init) {
		zc_error("never call zlog_init() or dzlog_init() before");
		goto exit;
	}

	/* that's the differnce, must judge default_category in lock */
	if (!zlog_default_category) {
		zc_error("zlog_default_category is null,"
			"dzlog_init() or dzlog_set_cateogry() is not called above");
		goto exit;
	}

	zlog_fetch_thread(a_thread, exit);

	zlog_event_set_fmt(a_thread->event,
		zlog_default_category->name, zlog_default_category->name_len,
		file, filelen, func, funclen, line, level,
		format, args);

	if (zlog_category_output(zlog_default_category, a_thread)) {
		zc_error("zlog_output fail, srcfile[%s], srcline[%ld]", file, line);
		goto exit;
	}

	if (zlog_env_conf->reload_conf_period &&
		++zlog_env_reload_conf_count > zlog_env_conf->reload_conf_period ) {
		/* under the protection of lock read env conf */
		goto reload;
	}

exit:
	pthread_rwlock_unlock(&zlog_env_lock);
	return;
reload:
	pthread_rwlock_unlock(&zlog_env_lock);
	/* will be wrlock, so after unlock */
	if (zlog_reload((char *)-1)) {
		zc_error("reach reload-conf-period but zlog_reload fail, zlog-chk-conf [file] see detail");
	}
	return;
}

void hdzlog(const char *file, size_t filelen,
	const char *func, size_t funclen,
	long line, int level,
	const void *buf, size_t buflen)
{
	zlog_thread_t *a_thread;

	if (zlog_category_needless_level(zlog_default_category, level)) return;

	pthread_rwlock_rdlock(&zlog_env_lock);

	if (!zlog_env_is_init) {
		zc_error("never call zlog_init() or dzlog_init() before");
		goto exit;
	}

	/* that's the differnce, must judge default_category in lock */
	if (!zlog_default_category) {
		zc_error("zlog_default_category is null,"
			"dzlog_init() or dzlog_set_cateogry() is not called above");
		goto exit;
	}

	zlog_fetch_thread(a_thread, exit);

	zlog_event_set_hex(a_thread->event,
		zlog_default_category->name, zlog_default_category->name_len,
		file, filelen, func, funclen, line, level,
		buf, buflen);

	if (zlog_category_output(zlog_default_category, a_thread)) {
		zc_error("zlog_output fail, srcfile[%s], srcline[%ld]", file, line);
		goto exit;
	}

	if (zlog_env_conf->reload_conf_period &&
		++zlog_env_reload_conf_count > zlog_env_conf->reload_conf_period ) {
		/* under the protection of lock read env conf */
		goto reload;
	}

exit:
	pthread_rwlock_unlock(&zlog_env_lock);
	return;
reload:
	pthread_rwlock_unlock(&zlog_env_lock);
	/* will be wrlock, so after unlock */
	if (zlog_reload((char *)-1)) {
		zc_error("reach reload-conf-period but zlog_reload fail, zlog-chk-conf [file] see detail");
	}
	return;
}

/*******************************************************************************/
/*
 * @brief 写日志函数, 输入的数据对应于配置文件中的%m, category来自于调用zlog_get_category()
 * zlog()根据format输出, 就像printf(3)
 */
void zlog(zlog_category_t * category,
	const char *file, size_t filelen, const char *func, size_t funclen,
	long line, const int level,
	const char *format, ...)
{
	zlog_thread_t *a_thread;
	va_list args;

	if (category && zlog_category_needless_level(category, level)) return;

	pthread_rwlock_rdlock(&zlog_env_lock);

	if (!zlog_env_is_init) {
		zc_error("never call zlog_init() or dzlog_init() before");
		goto exit;
	}

	zlog_fetch_thread(a_thread, exit);

	va_start(args, format);
	zlog_event_set_fmt(a_thread->event, category->name, category->name_len,
		file, filelen, func, funclen, line, level,
		format, args);
	if (zlog_category_output(category, a_thread)) {
		zc_error("zlog_output fail, srcfile[%s], srcline[%ld]", file, line);
		va_end(args);
		goto exit;
	}
	va_end(args);

	if (zlog_env_conf->reload_conf_period &&
		++zlog_env_reload_conf_count > zlog_env_conf->reload_conf_period ) {
		/* under the protection of lock read env conf */
		goto reload;
	}

exit:
	pthread_rwlock_unlock(&zlog_env_lock);
	return;
reload:
	pthread_rwlock_unlock(&zlog_env_lock);
	/* will be wrlock, so after unlock */
	if (zlog_reload((char *)-1)) {
		zc_error("reach reload-conf-period but zlog_reload fail, zlog-chk-conf [file] see detail");
	}
	return;
}

/*******************************************************************************/
void dzlog(const char *file, size_t filelen, const char *func, size_t funclen, long line, int level,
	const char *format, ...)
{
	zlog_thread_t *a_thread;
	va_list args;


	pthread_rwlock_rdlock(&zlog_env_lock);

	if (!zlog_env_is_init) {
		zc_error("never call zlog_init() or dzlog_init() before");
		goto exit;
	}

	/* that's the differnce, must judge default_category in lock */
	if (!zlog_default_category) {
		zc_error("zlog_default_category is null,"
			"dzlog_init() or dzlog_set_cateogry() is not called above");
		goto exit;
	}

	if (zlog_category_needless_level(zlog_default_category, level)) goto exit;

	zlog_fetch_thread(a_thread, exit);

	va_start(args, format);
	zlog_event_set_fmt(a_thread->event,
		zlog_default_category->name, zlog_default_category->name_len,
		file, filelen, func, funclen, line, level,
		format, args);

	if (zlog_category_output(zlog_default_category, a_thread)) {
		zc_error("zlog_output fail, srcfile[%s], srcline[%ld]", file, line);
		va_end(args);
		goto exit;
	}
	va_end(args);

	if (zlog_env_conf->reload_conf_period &&
		++zlog_env_reload_conf_count > zlog_env_conf->reload_conf_period ) {
		/* under the protection of lock read env conf */
		goto reload;
	}

exit:
	pthread_rwlock_unlock(&zlog_env_lock);
	return;
reload:
	pthread_rwlock_unlock(&zlog_env_lock);
	/* will be wrlock, so after unlock */
	if (zlog_reload((char *)-1)) {
		zc_error("reach reload-conf-period but zlog_reload fail, zlog-chk-conf [file] see detail");
	}
	return;
}

/*******************************************************************************/
// 调试和诊断
/*
 * 环境变量ZLOG_PROFILE_ERROR指定zlog本身的错误日志.
 * 环境变量ZLOG_PROFILE_DEBUG指定zlog本身的调试日志.
 */
/*
 * @brief 打印所有内存中的配置信息到ZLOG_PROFILE_ERROR
 *
 * 在运行时, 可以把这个和配置文件比较, 看看有没有问题.
 */
void zlog_profile(void)
{
	int rc = 0;
	rc = pthread_rwlock_rdlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_wrlock fail, rc[%d]", rc);
		return;
	}
	zc_warn("------zlog_profile start------ ");
	zc_warn("is init:[%d]", zlog_env_is_init);
	zc_warn("init version:[%d]", zlog_env_init_version);
	zlog_conf_profile(zlog_env_conf, ZC_WARN);
	zlog_record_table_profile(zlog_env_records, ZC_WARN);
	zlog_category_table_profile(zlog_env_categories, ZC_WARN);
	if (zlog_default_category) {
		zc_warn("-default_category-");
		zlog_category_profile(zlog_default_category, ZC_WARN);
	}
	zc_warn("------zlog_profile end------ ");
	rc = pthread_rwlock_unlock(&zlog_env_lock);
	if (rc) {
		zc_error("pthread_rwlock_unlock fail, rc=[%d]", rc);
		return;
	}
	return;
}
/*******************************************************************************/
/*
 * @brief 用户自定义输出, 绑定动作
 *
 * @return 0: 成功 / -1: 失败
 * 详细错误会被写在由环境变量ZLOG_PROFILE_ERROR指定的错误日志里面.
 */
int zlog_set_record(const char *rname, zlog_record_fn record_output)
{
	int rc = 0;
	int rd = 0;
	zlog_rule_t *a_rule;
	zlog_record_t *a_record;
	int i = 0;

	zc_assert(rname, -1);
	zc_assert(record_output, -1);

	rd = pthread_rwlock_wrlock(&zlog_env_lock);
	if (rd) {
		zc_error("pthread_rwlock_rdlock fail, rd[%d]", rd);
		return -1;
	}

	if (!zlog_env_is_init) {
		zc_error("never call zlog_init() or dzlog_init() before");
		goto zlog_set_record_exit;
	}

	a_record = zlog_record_new(rname, record_output);
	if (!a_record) {
		rc = -1;
		zc_error("zlog_record_new fail");
		goto zlog_set_record_exit;
	}

	rc = zc_hashtable_put(zlog_env_records, a_record->name, a_record);
	if (rc) {
		zlog_record_del(a_record);
		zc_error("zc_hashtable_put fail");
		goto zlog_set_record_exit;
	}

	zc_arraylist_foreach(zlog_env_conf->rules, i, a_rule) {
		zlog_rule_set_record(a_rule, zlog_env_records);
	}

      zlog_set_record_exit:
	rd = pthread_rwlock_unlock(&zlog_env_lock);
	if (rd) {
		zc_error("pthread_rwlock_unlock fail, rd=[%d]", rd);
		return -1;
	}
	return rc;
}
