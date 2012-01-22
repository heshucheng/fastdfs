/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.csource.org/ for more detail.
**/


#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/statvfs.h>
#include <sys/param.h>
#include "fdfs_define.h"
#include "logger.h"
#include "shared_func.h"
#include "fdfs_global.h"
#include "tracker_types.h"
#include "tracker_proto.h"
#include "storage_global.h"
#include "storage_param_getter.h"
#include "trunk_mem.h"
#include "trunk_sync.h"

int storage_get_params_from_tracker()
{
	IniContext iniContext;
	int result;
	char *pSpaceSize;
	int64_t reserved_storage_space;
	bool use_trunk_file;

	if ((result=fdfs_get_ini_context_from_tracker(&g_tracker_group, \
		&iniContext, &g_continue_flag, \
		g_client_bind_addr, g_bind_addr)) != 0)
	{
		return result;
	}

	g_storage_ip_changed_auto_adjust = iniGetBoolValue(NULL, \
			"storage_ip_changed_auto_adjust", \
			&iniContext, false);

	g_store_path_mode = iniGetIntValue(NULL, "store_path", &iniContext, \
				FDFS_STORE_PATH_ROUND_ROBIN);

	pSpaceSize = iniGetStrValue(NULL, "reserved_storage_space", &iniContext);
	if (pSpaceSize == NULL)
	{
		g_storage_reserved_mb = FDFS_DEF_STORAGE_RESERVED_MB;
	}
	else if ((result=parse_bytes(pSpaceSize, 1, \
			&reserved_storage_space)) != 0)
	{
		iniFreeContext(&iniContext);
		return result;
	}
	else
	{
		g_storage_reserved_mb = reserved_storage_space / FDFS_ONE_MB;
	}
	g_avg_storage_reserved_mb = g_storage_reserved_mb / g_fdfs_path_count;

	use_trunk_file = iniGetBoolValue(NULL, "use_trunk_file", \
				&iniContext, false);
	g_slot_min_size = iniGetIntValue(NULL, "slot_min_size", \
				&iniContext, 256);
	g_trunk_file_size = iniGetIntValue(NULL, "trunk_file_size", \
				&iniContext, 64 * 1024 * 1024);
	g_slot_max_size = iniGetIntValue(NULL, "slot_max_size", \
				&iniContext, g_trunk_file_size / 2);

	g_trunk_create_file_advance = iniGetBoolValue(NULL, \
			"trunk_create_file_advance", &iniContext, false);
	if ((result=get_time_item_from_conf(&iniContext, \
               	"trunk_create_file_time_base", \
		&g_trunk_create_file_time_base, 2, 0)) != 0)
	{
		iniFreeContext(&iniContext);
		return result;
	}
	g_trunk_create_file_interval = iniGetIntValue(NULL, \
			"trunk_create_file_interval", &iniContext, \
			86400);
	g_trunk_create_file_space_threshold = iniGetInt64Value(NULL, \
			"trunk_create_file_space_threshold", \
			&iniContext, 0);

	iniFreeContext(&iniContext);

	if (use_trunk_file && !g_if_use_trunk_file)
	{
		if ((result=trunk_sync_init()) != 0)
		{
			return result;
		}
	}
	g_if_use_trunk_file = use_trunk_file;

	logInfo("file: "__FILE__", line: %d, " \
		"storage_ip_changed_auto_adjust=%d, " \
		"store_path=%d, " \
		"reserved_storage_space=%d MB, " \
		"use_trunk_file=%d, " \
		"slot_min_size=%d, " \
		"slot_max_size=%d MB, " \
		"trunk_file_size=%d MB, " \
		"trunk_create_file_advance=%d, " \
		"trunk_create_file_time_base=%02d:%02d, " \
		"trunk_create_file_interval=%d, " \
		"trunk_create_file_space_threshold=%d GB", \
		__LINE__, g_storage_ip_changed_auto_adjust, \
		g_store_path_mode, g_storage_reserved_mb, \
		g_if_use_trunk_file, g_slot_min_size, \
		g_slot_max_size / FDFS_ONE_MB, \
		g_trunk_file_size / FDFS_ONE_MB, \
		g_trunk_create_file_advance, \
		g_trunk_create_file_time_base.hour, \
		g_trunk_create_file_time_base.minute, \
		g_trunk_create_file_interval, \
		(int)(g_trunk_create_file_space_threshold / \
		(FDFS_ONE_MB * 1024)));

	return 0;
}

