
/*
 * @file test_hello.c
 *
 * @author Akagi201
 * @date 2014/04/23
 *
 * a simple demo
 *
 */

/*
 * This file is part of the zlog Library.
 *
 * Copyright (C) 2011 by Hardy Simpson <HardySimpson1984@gmail.com>
 *
 * Licensed under the LGPL v2.1, see the file COPYING in base directory.
 */

#include <stdio.h>
#include "zlog.h"

int main(int argc, char** argv)
{
	int rc = 0;
	zlog_category_t *zc = NULL;

	// 初始化库, 给出配置文件路径
	rc = zlog_init("test_hello.conf");
	if (rc) {
		printf("init failed\n");
		return -1;
	}

	// 获取分类名, 表示要打印哪些日志
	zc = zlog_get_category("my_cat");
	if (!zc) {
		printf("get cat fail\n");
		zlog_fini();
		return -2;
	}

	zlog_info(zc, "hello, zlog");

	zlog_fini();
	
	return 0;
}
