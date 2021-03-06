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
#include "fdfs_define.h"
#include "logger.h"
#include "shared_func.h"
#include "fdfs_global.h"
#include "sockopt.h"
#include "tracker_types.h"
#include "tracker_proto.h"
#include "tracker_client.h"
#include "client_global.h"

int tracker_get_all_connections_ex(TrackerServerGroup *pTrackerGroup)
{
	TrackerServerInfo *pServer;
	TrackerServerInfo *pEnd;
	int success_count;

	success_count = 0;
	pEnd = pTrackerGroup->servers + pTrackerGroup->server_count;
	for (pServer=pTrackerGroup->servers; pServer<pEnd; pServer++)
	{
		if (pServer->sock >= 0)
		{
			success_count++;
		}
		else if (tracker_connect_server(pServer) == 0)
		{
			fdfs_active_test(pServer);
			success_count++;
		}
	}

	return success_count > 0 ? 0 : ENOTCONN;
}

void tracker_close_all_connections_ex(TrackerServerGroup *pTrackerGroup)
{
	TrackerServerInfo *pServer;
	TrackerServerInfo *pEnd;

	pEnd = pTrackerGroup->servers + pTrackerGroup->server_count;
	for (pServer=pTrackerGroup->servers; pServer<pEnd; pServer++)
	{
		tracker_disconnect_server(pServer);
	}
}

TrackerServerInfo *tracker_get_connection_ex(TrackerServerGroup *pTrackerGroup)
{
	TrackerServerInfo *pCurrentServer;
	TrackerServerInfo *pResult;
	TrackerServerInfo *pServer;
	TrackerServerInfo *pEnd;
	int server_index;

	server_index = pTrackerGroup->server_index;
	if (server_index >= pTrackerGroup->server_count)
	{
		server_index = 0;
	}

	pResult = NULL;

	do
	{
	pCurrentServer = pTrackerGroup->servers + server_index;
	if (pCurrentServer->sock >= 0 ||
		tracker_connect_server(pCurrentServer) == 0)
	{
		pResult = pCurrentServer;
		break;
	}

	pEnd = pTrackerGroup->servers + pTrackerGroup->server_count;
	for (pServer=pCurrentServer+1; pServer<pEnd; pServer++)
	{
		if (pServer->sock >= 0 || tracker_connect_server(pServer) == 0)
		{
			pResult = pServer;
			pTrackerGroup->server_index = pServer - \
							pTrackerGroup->servers;
			break;
		}
	}

	if (pResult != NULL)
	{
		break;
	}

	for (pServer=pTrackerGroup->servers; pServer<pCurrentServer; pServer++)
	{
		if (pServer->sock >= 0 || tracker_connect_server(pServer) == 0)
		{
			pResult = pServer;
			pTrackerGroup->server_index = pServer - \
							pTrackerGroup->servers;
			break;
		}
	}
	} while (0);

	pTrackerGroup->server_index++;
	if (pTrackerGroup->server_index >= pTrackerGroup->server_count)
	{
		pTrackerGroup->server_index = 0;
	}

	return pResult;
}

int tracker_get_connection_r_ex(TrackerServerGroup *pTrackerGroup, \
		TrackerServerInfo *pTrackerServer)
{
	TrackerServerInfo *pCurrentServer;
	TrackerServerInfo *pServer;
	TrackerServerInfo *pEnd;
	int server_index;
	int result;

	server_index = pTrackerGroup->server_index;
	if (server_index >= pTrackerGroup->server_count)
	{
		server_index = 0;
	}

	do
	{
	pCurrentServer = pTrackerGroup->servers + server_index;
	memcpy(pTrackerServer, pCurrentServer, sizeof(TrackerServerInfo));
	pTrackerServer->sock = -1;
	if ((result=tracker_connect_server(pTrackerServer)) == 0)
	{
		break;
	}

	pEnd = pTrackerGroup->servers + pTrackerGroup->server_count;
	for (pServer=pCurrentServer+1; pServer<pEnd; pServer++)
	{
		memcpy(pTrackerServer, pServer, sizeof(TrackerServerInfo));
		pTrackerServer->sock = -1;
		if ((result=tracker_connect_server(pTrackerServer)) == 0)
		{
			pTrackerGroup->server_index = pServer - \
							pTrackerGroup->servers;
			break;
		}
	}

	if (result == 0)
	{
		break;
	}

	for (pServer=pTrackerGroup->servers; pServer<pCurrentServer; pServer++)
	{
		memcpy(pTrackerServer, pServer, sizeof(TrackerServerInfo));
		pTrackerServer->sock = -1;
		if ((result=tracker_connect_server(pTrackerServer)) == 0)
		{
			pTrackerGroup->server_index = pServer - \
							pTrackerGroup->servers;
			break;
		}
	}
	} while (0);

	pTrackerGroup->server_index++;
	if (pTrackerGroup->server_index >= pTrackerGroup->server_count)
	{
		pTrackerGroup->server_index = 0;
	}

	return result;
}

int tracker_list_servers(TrackerServerInfo *pTrackerServer, \
		const char *szGroupName, const char *szStorageIp, \
		FDFSStorageInfo *storage_infos, const int max_storages, \
		int *storage_count)
{
	char out_buff[sizeof(TrackerHeader) + FDFS_GROUP_NAME_MAX_LEN + \
			IP_ADDRESS_SIZE];
	TrackerHeader *pHeader;
	int result;
	int name_len;
	int ip_len;
	TrackerStorageStat stats[FDFS_MAX_GROUPS];
	char *pInBuff;
	TrackerStorageStat *pSrc;
	TrackerStorageStat *pEnd;
	FDFSStorageStat *pStorageStat;
	FDFSStorageInfo *pDest;
	FDFSStorageStatBuff *pStatBuff;
	int64_t in_bytes;

	if (pTrackerServer->sock < 0)
	{
		if ((result=tracker_connect_server(pTrackerServer)) != 0)
		{
			return result;
		}
	}

	memset(out_buff, 0, sizeof(out_buff));
	pHeader = (TrackerHeader *)out_buff;
	name_len = strlen(szGroupName);
	if (name_len > FDFS_GROUP_NAME_MAX_LEN)
	{
		name_len = FDFS_GROUP_NAME_MAX_LEN;
	}
	memcpy(out_buff + sizeof(TrackerHeader), szGroupName, name_len);

	if (szStorageIp == NULL)
	{
		ip_len = 0;
	}
	else
	{
		ip_len = strlen(szStorageIp);
		if (ip_len >= IP_ADDRESS_SIZE)
		{
			ip_len = IP_ADDRESS_SIZE - 1;
		}

		memcpy(out_buff+sizeof(TrackerHeader)+FDFS_GROUP_NAME_MAX_LEN,\
			szStorageIp, ip_len);
	}

	long2buff(FDFS_GROUP_NAME_MAX_LEN + ip_len, pHeader->pkg_len);
	pHeader->cmd = TRACKER_PROTO_CMD_SERVER_LIST_STORAGE;
	if ((result=tcpsenddata_nb(pTrackerServer->sock, out_buff, \
		sizeof(TrackerHeader) + FDFS_GROUP_NAME_MAX_LEN + ip_len, \
		g_fdfs_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
		        "send data to tracker server %s:%d fail, " \
			"errno: %d, error info: %s", __LINE__, \
			pTrackerServer->ip_addr, \
			pTrackerServer->port, \
			result, STRERROR(result));
	}
	else
	{
		pInBuff = (char *)stats;
		result = fdfs_recv_response(pTrackerServer, &pInBuff, \
					sizeof(stats), &in_bytes);
	}

	if (result != 0)
	{
		*storage_count = 0;
		close(pTrackerServer->sock);
		pTrackerServer->sock = -1;

		return result;
	}

	if (in_bytes % sizeof(TrackerStorageStat) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"tracker server %s:%d response data " \
			"length: "INT64_PRINTF_FORMAT" is invalid", \
			__LINE__, pTrackerServer->ip_addr, \
			pTrackerServer->port, in_bytes);
		*storage_count = 0;
		return EINVAL;
	}

	*storage_count = in_bytes / sizeof(TrackerStorageStat);
	if (*storage_count > max_storages)
	{
		logError("file: "__FILE__", line: %d, " \
		 	"tracker server %s:%d insufficent space, " \
			"max storage count: %d, expect count: %d", \
			__LINE__, pTrackerServer->ip_addr, \
			pTrackerServer->port, max_storages, *storage_count);
		*storage_count = 0;
		return ENOSPC;
	}

	memset(storage_infos, 0, sizeof(FDFSStorageInfo) * max_storages);
	pDest = storage_infos;
	pEnd = stats + (*storage_count);
	for (pSrc=stats; pSrc<pEnd; pSrc++)
	{
		pStatBuff = &(pSrc->stat_buff);
		pStorageStat = &(pDest->stat);

		pDest->status = pSrc->status;
		memcpy(pDest->ip_addr, pSrc->ip_addr, \
				IP_ADDRESS_SIZE - 1);
		memcpy(pDest->src_ip_addr, pSrc->src_ip_addr, \
				IP_ADDRESS_SIZE - 1);
		strcpy(pDest->domain_name, pSrc->domain_name);
		strcpy(pDest->version, pSrc->version);
		pDest->join_time = buff2long(pSrc->sz_join_time);
		pDest->up_time = buff2long(pSrc->sz_up_time);
		pDest->total_mb = buff2long(pSrc->sz_total_mb);
		pDest->free_mb = buff2long(pSrc->sz_free_mb);
		pDest->upload_priority = buff2long(pSrc->sz_upload_priority);
		pDest->store_path_count = buff2long(pSrc->sz_store_path_count);
		pDest->subdir_count_per_path = buff2long( \
					pSrc->sz_subdir_count_per_path);
		pDest->storage_port = buff2long(pSrc->sz_storage_port);
		pDest->storage_http_port = buff2long(pSrc->sz_storage_http_port);
		pDest->current_write_path = buff2long( \
					pSrc->sz_current_write_path);

		pStorageStat->total_upload_count = buff2long( \
			pStatBuff->sz_total_upload_count);
		pStorageStat->success_upload_count = buff2long( \
			pStatBuff->sz_success_upload_count);
		pStorageStat->total_append_count = buff2long( \
			pStatBuff->sz_total_append_count);
		pStorageStat->success_append_count = buff2long( \
			pStatBuff->sz_success_append_count);
		pStorageStat->total_set_meta_count = buff2long( \
			pStatBuff->sz_total_set_meta_count);
		pStorageStat->success_set_meta_count = buff2long( \
			pStatBuff->sz_success_set_meta_count);
		pStorageStat->total_delete_count = buff2long( \
			pStatBuff->sz_total_delete_count);
		pStorageStat->success_delete_count = buff2long( \
			pStatBuff->sz_success_delete_count);
		pStorageStat->total_download_count = buff2long( \
			pStatBuff->sz_total_download_count);
		pStorageStat->success_download_count = buff2long( \
			pStatBuff->sz_success_download_count);
		pStorageStat->total_get_meta_count = buff2long( \
			pStatBuff->sz_total_get_meta_count);
		pStorageStat->success_get_meta_count = buff2long( \
			pStatBuff->sz_success_get_meta_count);
		pStorageStat->last_source_update = buff2long( \
			pStatBuff->sz_last_source_update);
		pStorageStat->last_sync_update = buff2long( \
			pStatBuff->sz_last_sync_update);
		pStorageStat->last_synced_timestamp = buff2long( \
			pStatBuff->sz_last_synced_timestamp);
		pStorageStat->total_create_link_count = buff2long( \
			pStatBuff->sz_total_create_link_count);
		pStorageStat->success_create_link_count = buff2long( \
			pStatBuff->sz_success_create_link_count);
		pStorageStat->total_delete_link_count = buff2long( \
			pStatBuff->sz_total_delete_link_count);
		pStorageStat->success_delete_link_count = buff2long( \
			pStatBuff->sz_success_delete_link_count);
		pStorageStat->total_upload_bytes = buff2long( \
			pStatBuff->sz_total_upload_bytes);
		pStorageStat->success_upload_bytes = buff2long( \
			pStatBuff->sz_success_upload_bytes);
		pStorageStat->total_append_bytes = buff2long( \
			pStatBuff->sz_total_append_bytes);
		pStorageStat->success_append_bytes = buff2long( \
			pStatBuff->sz_success_append_bytes);
		pStorageStat->total_download_bytes = buff2long( \
			pStatBuff->sz_total_download_bytes);
		pStorageStat->success_download_bytes = buff2long( \
			pStatBuff->sz_success_download_bytes);
		pStorageStat->total_sync_in_bytes = buff2long( \
			pStatBuff->sz_total_sync_in_bytes);
		pStorageStat->success_sync_in_bytes = buff2long( \
			pStatBuff->sz_success_sync_in_bytes);
		pStorageStat->total_sync_out_bytes = buff2long( \
			pStatBuff->sz_total_sync_out_bytes);
		pStorageStat->success_sync_out_bytes = buff2long( \
			pStatBuff->sz_success_sync_out_bytes);
		pStorageStat->total_file_open_count = buff2long( \
			pStatBuff->sz_total_file_open_count);
		pStorageStat->success_file_open_count = buff2long( \
			pStatBuff->sz_success_file_open_count);
		pStorageStat->total_file_read_count = buff2long( \
			pStatBuff->sz_total_file_read_count);
		pStorageStat->success_file_read_count = buff2long( \
			pStatBuff->sz_success_file_read_count);
		pStorageStat->total_file_write_count = buff2long( \
			pStatBuff->sz_total_file_write_count);
		pStorageStat->success_file_write_count = buff2long( \
			pStatBuff->sz_success_file_write_count);
		pStorageStat->last_heart_beat_time = buff2long( \
			pStatBuff->sz_last_heart_beat_time);
		pDest->if_trunk_server = pSrc->if_trunk_server;
		pDest++;
	}

	return 0;
}

int tracker_list_one_group(TrackerServerInfo *pTrackerServer, \
		const char *group_name, FDFSGroupStat *pDest)
{
	TrackerHeader *pHeader;
	char out_buff[sizeof(TrackerHeader) + FDFS_GROUP_NAME_MAX_LEN];
	TrackerGroupStat src;
	char *pInBuff;
	int result;
	int64_t in_bytes;

	if (pTrackerServer->sock < 0)
	{
		if ((result=tracker_connect_server(pTrackerServer)) != 0)
		{
			return result;
		}
	}

	memset(out_buff, 0, sizeof(out_buff));
	pHeader = (TrackerHeader *)out_buff;
	snprintf(out_buff + sizeof(TrackerHeader), sizeof(out_buff) - \
			sizeof(TrackerHeader),  "%s", group_name);
	pHeader->cmd = TRACKER_PROTO_CMD_SERVER_LIST_ONE_GROUP;
	long2buff(FDFS_GROUP_NAME_MAX_LEN, pHeader->pkg_len);
	if ((result=tcpsenddata_nb(pTrackerServer->sock, out_buff, \
			sizeof(out_buff), g_fdfs_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"send data to tracker server %s:%d fail, " \
			"errno: %d, error info: %s", __LINE__, \
			pTrackerServer->ip_addr, \
			pTrackerServer->port, \
			result, STRERROR(result));
	}
	else
	{
		pInBuff = (char *)&src;
		result = fdfs_recv_response(pTrackerServer, \
			&pInBuff, sizeof(TrackerGroupStat), &in_bytes);
	}

	if (result != 0)
	{
		close(pTrackerServer->sock);
		pTrackerServer->sock = -1;

		return result;
	}

	if (in_bytes != sizeof(TrackerGroupStat))
	{
		logError("file: "__FILE__", line: %d, " \
			"tracker server %s:%d response data " \
			"length: "INT64_PRINTF_FORMAT" is invalid", \
			__LINE__, pTrackerServer->ip_addr, \
			pTrackerServer->port, in_bytes);
		return EINVAL;
	}

	memset(pDest, 0, sizeof(FDFSGroupStat));
	memcpy(pDest->group_name, src.group_name, FDFS_GROUP_NAME_MAX_LEN);
	pDest->free_mb = buff2long(src.sz_free_mb);
	pDest->trunk_free_mb = buff2long(src.sz_trunk_free_mb);
	pDest->count= buff2long(src.sz_count);
	pDest->storage_port= buff2long(src.sz_storage_port);
	pDest->storage_http_port= buff2long(src.sz_storage_http_port);
	pDest->active_count = buff2long(src.sz_active_count);
	pDest->current_write_server = buff2long(src.sz_current_write_server);
	pDest->store_path_count = buff2long(src.sz_store_path_count);
	pDest->subdir_count_per_path = buff2long(src.sz_subdir_count_per_path);
	pDest->current_trunk_file_id = buff2long(src.sz_current_trunk_file_id);

	return 0;
}

int tracker_list_groups(TrackerServerInfo *pTrackerServer, \
		FDFSGroupStat *group_stats, const int max_groups, \
		int *group_count)
{
	TrackerHeader header;
	TrackerGroupStat stats[FDFS_MAX_GROUPS];
	char *pInBuff;
	TrackerGroupStat *pSrc;
	TrackerGroupStat *pEnd;
	FDFSGroupStat *pDest;
	int result;
	int64_t in_bytes;

	if (pTrackerServer->sock < 0)
	{
		if ((result=tracker_connect_server(pTrackerServer)) != 0)
		{
			return result;
		}
	}

	memset(&header, 0, sizeof(header));
	header.cmd = TRACKER_PROTO_CMD_SERVER_LIST_ALL_GROUPS;
	header.status = 0;
	if ((result=tcpsenddata_nb(pTrackerServer->sock, &header, \
			sizeof(header), g_fdfs_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"send data to tracker server %s:%d fail, " \
			"errno: %d, error info: %s", __LINE__, \
			pTrackerServer->ip_addr, \
			pTrackerServer->port, \
			result, STRERROR(result));
	}
	else
	{
		pInBuff = (char *)stats;
		result = fdfs_recv_response(pTrackerServer, \
			&pInBuff, sizeof(stats), &in_bytes);
	}

	if (result != 0)
	{
		*group_count = 0;
		close(pTrackerServer->sock);
		pTrackerServer->sock = -1;

		return result;
	}

	if (in_bytes % sizeof(TrackerGroupStat) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"tracker server %s:%d response data " \
			"length: "INT64_PRINTF_FORMAT" is invalid", \
			__LINE__, pTrackerServer->ip_addr, \
			pTrackerServer->port, in_bytes);
		*group_count = 0;
		return EINVAL;
	}

	*group_count = in_bytes / sizeof(TrackerGroupStat);
	if (*group_count > max_groups)
	{
		logError("file: "__FILE__", line: %d, " \
			"tracker server %s:%d insufficent space, " \
			"max group count: %d, expect count: %d", \
			__LINE__, pTrackerServer->ip_addr, \
			pTrackerServer->port, max_groups, *group_count);
		*group_count = 0;
		return ENOSPC;
	}

	memset(group_stats, 0, sizeof(FDFSGroupStat) * max_groups);
	pDest = group_stats;
	pEnd = stats + (*group_count);
	for (pSrc=stats; pSrc<pEnd; pSrc++)
	{
		memcpy(pDest->group_name, pSrc->group_name, \
				FDFS_GROUP_NAME_MAX_LEN);
		pDest->free_mb = buff2long(pSrc->sz_free_mb);
		pDest->trunk_free_mb = buff2long(pSrc->sz_trunk_free_mb);
		pDest->count= buff2long(pSrc->sz_count);
		pDest->storage_port= buff2long(pSrc->sz_storage_port);
		pDest->storage_http_port= buff2long(pSrc->sz_storage_http_port);
		pDest->active_count = buff2long(pSrc->sz_active_count);
		pDest->current_write_server = buff2long( \
				pSrc->sz_current_write_server);
		pDest->store_path_count = buff2long( \
				pSrc->sz_store_path_count);
		pDest->subdir_count_per_path = buff2long( \
				pSrc->sz_subdir_count_per_path);
		pDest->current_trunk_file_id = buff2long( \
				pSrc->sz_current_trunk_file_id);

		pDest++;
	}

	return 0;
}

int tracker_do_query_storage(TrackerServerInfo *pTrackerServer, \
		TrackerServerInfo *pStorageServer, const byte cmd, \
		const char *group_name, const char *filename)
{
	TrackerHeader *pHeader;
	char out_buff[sizeof(TrackerHeader) + FDFS_GROUP_NAME_MAX_LEN + 128];
	char in_buff[sizeof(TrackerHeader) + TRACKER_QUERY_STORAGE_FETCH_BODY_LEN];
	char *pInBuff;
	int64_t in_bytes;
	int result;
	int filename_len;

	if (pTrackerServer->sock < 0)
	{
		if ((result=tracker_connect_server(pTrackerServer)) != 0)
		{
			return result;
		}
	}

	memset(pStorageServer, 0, sizeof(TrackerServerInfo));
	pStorageServer->sock = -1;

	memset(out_buff, 0, sizeof(out_buff));
	pHeader = (TrackerHeader *)out_buff;
	snprintf(out_buff + sizeof(TrackerHeader), sizeof(out_buff) - \
			sizeof(TrackerHeader),  "%s", group_name);
	filename_len = snprintf(out_buff + sizeof(TrackerHeader) + \
			FDFS_GROUP_NAME_MAX_LEN, \
			sizeof(out_buff) - sizeof(TrackerHeader) - \
			FDFS_GROUP_NAME_MAX_LEN,  "%s", filename);
	
	long2buff(FDFS_GROUP_NAME_MAX_LEN + filename_len, pHeader->pkg_len);
	pHeader->cmd = cmd;
	if ((result=tcpsenddata_nb(pTrackerServer->sock, out_buff, \
		sizeof(TrackerHeader) + FDFS_GROUP_NAME_MAX_LEN + 
		filename_len, g_fdfs_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"send data to tracker server %s:%d fail, " \
			"errno: %d, error info: %s", __LINE__, \
			pTrackerServer->ip_addr, \
			pTrackerServer->port, \
			result, STRERROR(result));
	}
	else
	{
		pInBuff = in_buff;
		result = fdfs_recv_response(pTrackerServer, \
			&pInBuff, sizeof(in_buff), &in_bytes);
	}

	if (result != 0)
	{
		close(pTrackerServer->sock);
		pTrackerServer->sock = -1;

		return result;
	}

	if (in_bytes != TRACKER_QUERY_STORAGE_FETCH_BODY_LEN)
	{
		logError("file: "__FILE__", line: %d, " \
			"tracker server %s:%d response data " \
			"length: "INT64_PRINTF_FORMAT" is invalid, " \
			"expect length: %d", __LINE__, \
			pTrackerServer->ip_addr, \
			pTrackerServer->port, in_bytes, \
			TRACKER_QUERY_STORAGE_FETCH_BODY_LEN);
		return EINVAL;
	}

	memcpy(pStorageServer->group_name, in_buff, \
			FDFS_GROUP_NAME_MAX_LEN);
	memcpy(pStorageServer->ip_addr, in_buff + \
			FDFS_GROUP_NAME_MAX_LEN, IP_ADDRESS_SIZE-1);
	pStorageServer->port = (int)buff2long(in_buff + \
			FDFS_GROUP_NAME_MAX_LEN + IP_ADDRESS_SIZE - 1);
	return 0;
}

int tracker_query_storage_list(TrackerServerInfo *pTrackerServer, \
		TrackerServerInfo *pStorageServer, const int nMaxServerCount, \
		int *server_count, const char *group_name, const char *filename)
{
	TrackerHeader *pHeader;
	TrackerServerInfo *pServer;
	TrackerServerInfo *pServerEnd;
	char out_buff[sizeof(TrackerHeader) + FDFS_GROUP_NAME_MAX_LEN + 128];
	char in_buff[sizeof(TrackerHeader) + \
		TRACKER_QUERY_STORAGE_FETCH_BODY_LEN + \
		FDFS_MAX_SERVERS_EACH_GROUP * IP_ADDRESS_SIZE];
	char *pInBuff;
	int64_t in_bytes;
	int result;
	int filename_len;

	if (pTrackerServer->sock < 0)
	{
		if ((result=tracker_connect_server(pTrackerServer)) != 0)
		{
			return result;
		}
	}

	memset(out_buff, 0, sizeof(out_buff));
	pHeader = (TrackerHeader *)out_buff;
	snprintf(out_buff + sizeof(TrackerHeader), sizeof(out_buff) - \
			sizeof(TrackerHeader),  "%s", group_name);
	filename_len = snprintf(out_buff + sizeof(TrackerHeader) + \
			FDFS_GROUP_NAME_MAX_LEN, \
			sizeof(out_buff) - sizeof(TrackerHeader) - \
			FDFS_GROUP_NAME_MAX_LEN,  "%s", filename);
	
	long2buff(FDFS_GROUP_NAME_MAX_LEN + filename_len, pHeader->pkg_len);
	pHeader->cmd = TRACKER_PROTO_CMD_SERVICE_QUERY_FETCH_ALL;
	if ((result=tcpsenddata_nb(pTrackerServer->sock, out_buff, \
		sizeof(TrackerHeader) + FDFS_GROUP_NAME_MAX_LEN + 
		filename_len, g_fdfs_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"send data to tracker server %s:%d fail, " \
			"errno: %d, error info: %s", __LINE__, \
			pTrackerServer->ip_addr, \
			pTrackerServer->port, \
			result, STRERROR(result));
	}
	else
	{
		pInBuff = in_buff;
		result = fdfs_recv_response(pTrackerServer, \
				&pInBuff, sizeof(in_buff), &in_bytes);
	}

	if (result != 0)
	{
		close(pTrackerServer->sock);
		pTrackerServer->sock = -1;

		return result;
	}

	if ((in_bytes - TRACKER_QUERY_STORAGE_FETCH_BODY_LEN) % \
		(IP_ADDRESS_SIZE - 1) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"tracker server %s:%d response data " \
			"length: "INT64_PRINTF_FORMAT" is invalid", \
			__LINE__, pTrackerServer->ip_addr, \
			pTrackerServer->port, in_bytes);
		return EINVAL;
	}

	*server_count = 1 + (in_bytes - TRACKER_QUERY_STORAGE_FETCH_BODY_LEN) /
			(IP_ADDRESS_SIZE - 1);
	if (nMaxServerCount < *server_count)
	{
		logError("file: "__FILE__", line: %d, " \
			"tracker server %s:%d response storage server " \
			 "count: %d, exceeds max server count: %d!", __LINE__, \
			pTrackerServer->ip_addr, pTrackerServer->port, \
			*server_count, nMaxServerCount);
		return ENOSPC;
	}

	memset(pStorageServer, 0, nMaxServerCount * sizeof(TrackerServerInfo));
	pStorageServer->sock = -1;

	memcpy(pStorageServer->group_name, pInBuff, \
			FDFS_GROUP_NAME_MAX_LEN);
	pInBuff += FDFS_GROUP_NAME_MAX_LEN;
	memcpy(pStorageServer->ip_addr, pInBuff, IP_ADDRESS_SIZE - 1);
	pInBuff += IP_ADDRESS_SIZE - 1;
	pStorageServer->port = (int)buff2long(pInBuff);
	pInBuff += FDFS_PROTO_PKG_LEN_SIZE;

	pServerEnd = pStorageServer + (*server_count);
	for (pServer=pStorageServer+1; pServer<pServerEnd; pServer++)
	{
		pServer->sock = -1;
		pServer->port = pStorageServer->port;
		memcpy(pServer->ip_addr, pInBuff, IP_ADDRESS_SIZE - 1);
		pInBuff += IP_ADDRESS_SIZE - 1;
	}

	return 0;
}

int tracker_query_storage_store_without_group(TrackerServerInfo *pTrackerServer,
		TrackerServerInfo *pStorageServer, int *store_path_index)
{
	TrackerHeader header;
	char in_buff[sizeof(TrackerHeader) + \
		TRACKER_QUERY_STORAGE_STORE_BODY_LEN];
	char *pInBuff;
	int64_t in_bytes;
	int result;

	if (pTrackerServer->sock < 0)
	{
		if ((result=tracker_connect_server(pTrackerServer)) != 0)
		{
			return result;
		}
	}

	memset(pStorageServer, 0, sizeof(TrackerServerInfo));
	pStorageServer->sock = -1;

	memset(&header, 0, sizeof(header));
	header.cmd = TRACKER_PROTO_CMD_SERVICE_QUERY_STORE_WITHOUT_GROUP_ONE;
	if ((result=tcpsenddata_nb(pTrackerServer->sock, &header, \
			sizeof(header), g_fdfs_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"send data to tracker server %s:%d fail, " \
			"errno: %d, error info: %s", __LINE__, \
			pTrackerServer->ip_addr, \
			pTrackerServer->port, \
			result, STRERROR(result));
	}
	else
	{
		pInBuff = in_buff;
		result = fdfs_recv_response(pTrackerServer, \
				&pInBuff, sizeof(in_buff), &in_bytes);
	}

	if (result != 0)
	{
		close(pTrackerServer->sock);
		pTrackerServer->sock = -1;

		return result;
	}

	if (in_bytes != TRACKER_QUERY_STORAGE_STORE_BODY_LEN)
	{
		logError("file: "__FILE__", line: %d, " \
			"tracker server %s:%d response data " \
			"length: "INT64_PRINTF_FORMAT" is invalid, " \
			"expect length: %d", __LINE__, \
			pTrackerServer->ip_addr, pTrackerServer->port, \
			in_bytes, TRACKER_QUERY_STORAGE_STORE_BODY_LEN);
		return EINVAL;
	}

	memcpy(pStorageServer->group_name, in_buff, \
			FDFS_GROUP_NAME_MAX_LEN);
	memcpy(pStorageServer->ip_addr, in_buff + \
			FDFS_GROUP_NAME_MAX_LEN, IP_ADDRESS_SIZE-1);
	pStorageServer->port = (int)buff2long(in_buff + \
				FDFS_GROUP_NAME_MAX_LEN + IP_ADDRESS_SIZE - 1);
	*store_path_index = *(in_buff + FDFS_GROUP_NAME_MAX_LEN + \
			 IP_ADDRESS_SIZE - 1 + FDFS_PROTO_PKG_LEN_SIZE);

	return 0;
}

int tracker_query_storage_store_with_group(TrackerServerInfo *pTrackerServer, \
		const char *group_name, TrackerServerInfo *pStorageServer, \
		int *store_path_index)
{
	TrackerHeader *pHeader;
	char out_buff[sizeof(TrackerHeader) + FDFS_GROUP_NAME_MAX_LEN];
	char in_buff[sizeof(TrackerHeader) + \
		TRACKER_QUERY_STORAGE_STORE_BODY_LEN];
	char *pInBuff;
	int64_t in_bytes;
	int result;

	if (pTrackerServer->sock < 0)
	{
		if ((result=tracker_connect_server(pTrackerServer)) != 0)
		{
			return result;
		}
	}

	memset(pStorageServer, 0, sizeof(TrackerServerInfo));
	pStorageServer->sock = -1;

	pHeader = (TrackerHeader *)out_buff;
	memset(out_buff, 0, sizeof(out_buff));
	snprintf(out_buff + sizeof(TrackerHeader), sizeof(out_buff) - \
			sizeof(TrackerHeader),  "%s", group_name);
	
	long2buff(FDFS_GROUP_NAME_MAX_LEN, pHeader->pkg_len);
	pHeader->cmd = TRACKER_PROTO_CMD_SERVICE_QUERY_STORE_WITH_GROUP_ONE;
	if ((result=tcpsenddata_nb(pTrackerServer->sock, out_buff, \
			sizeof(TrackerHeader) + FDFS_GROUP_NAME_MAX_LEN, \
			g_fdfs_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"send data to tracker server %s:%d fail, " \
			"errno: %d, error info: %s", __LINE__, \
			pTrackerServer->ip_addr, \
			pTrackerServer->port, \
			result, STRERROR(result));
	}
	else
	{
		pInBuff = in_buff;
		result = fdfs_recv_response(pTrackerServer, \
				&pInBuff, sizeof(in_buff), &in_bytes);
	}

	if (result != 0)
	{
		close(pTrackerServer->sock);
		pTrackerServer->sock = -1;

		return result;
	}

	if (in_bytes != TRACKER_QUERY_STORAGE_STORE_BODY_LEN)
	{
		logError("file: "__FILE__", line: %d, " \
			"tracker server %s:%d response data " \
			"length: "INT64_PRINTF_FORMAT" is invalid, " \
			"expect length: %d", __LINE__, \
			pTrackerServer->ip_addr, pTrackerServer->port, \
			in_bytes, TRACKER_QUERY_STORAGE_STORE_BODY_LEN);
		return EINVAL;
	}

	memcpy(pStorageServer->group_name, in_buff, \
			FDFS_GROUP_NAME_MAX_LEN);
	memcpy(pStorageServer->ip_addr, in_buff + \
			FDFS_GROUP_NAME_MAX_LEN, IP_ADDRESS_SIZE-1);
	pStorageServer->port = (int)buff2long(in_buff + \
				FDFS_GROUP_NAME_MAX_LEN + IP_ADDRESS_SIZE - 1);
	*store_path_index = *(in_buff + FDFS_GROUP_NAME_MAX_LEN + \
			 IP_ADDRESS_SIZE - 1 + FDFS_PROTO_PKG_LEN_SIZE);

	return 0;
}

int tracker_query_storage_store_list_with_group( \
	TrackerServerInfo *pTrackerServer, const char *group_name, \
	TrackerServerInfo *storageServers, const int nMaxServerCount, \
	int *storage_count, int *store_path_index)
{
	TrackerServerInfo *pStorageServer;
	TrackerServerInfo *pServerEnd;
	TrackerHeader *pHeader;
	char out_buff[sizeof(TrackerHeader) + FDFS_GROUP_NAME_MAX_LEN];
	char in_buff[sizeof(TrackerHeader) + FDFS_MAX_SERVERS_EACH_GROUP * \
			TRACKER_QUERY_STORAGE_STORE_BODY_LEN];
	char returned_group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	char *pInBuff;
	char *p;
	int64_t in_bytes;
	int out_len;
	int ipPortsLen;
	int result;

	*storage_count = 0;
	if (pTrackerServer->sock < 0)
	{
		if ((result=tracker_connect_server(pTrackerServer)) != 0)
		{
			return result;
		}
	}

	pHeader = (TrackerHeader *)out_buff;
	memset(out_buff, 0, sizeof(out_buff));

	if (group_name == NULL)
	{
	pHeader->cmd = TRACKER_PROTO_CMD_SERVICE_QUERY_STORE_WITHOUT_GROUP_ALL;
	out_len = 0;
	}
	else
	{
	pHeader->cmd = TRACKER_PROTO_CMD_SERVICE_QUERY_STORE_WITH_GROUP_ALL;
	snprintf(out_buff + sizeof(TrackerHeader), sizeof(out_buff) - \
			sizeof(TrackerHeader),  "%s", group_name);
	out_len = FDFS_GROUP_NAME_MAX_LEN;
	}

	long2buff(out_len, pHeader->pkg_len);
	if ((result=tcpsenddata_nb(pTrackerServer->sock, out_buff, \
		sizeof(TrackerHeader) + out_len, g_fdfs_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"send data to tracker server %s:%d fail, " \
			"errno: %d, error info: %s", __LINE__, \
			pTrackerServer->ip_addr, \
			pTrackerServer->port, \
			result, STRERROR(result));
	}
	else
	{
		pInBuff = in_buff;
		result = fdfs_recv_response(pTrackerServer, \
				&pInBuff, sizeof(in_buff), &in_bytes);
	}

	if (result != 0)
	{
		close(pTrackerServer->sock);
		pTrackerServer->sock = -1;

		return result;
	}

	if (in_bytes < TRACKER_QUERY_STORAGE_STORE_BODY_LEN)
	{
		logError("file: "__FILE__", line: %d, " \
			"tracker server %s:%d response data " \
			"length: "INT64_PRINTF_FORMAT" is invalid, " \
			"expect length >= %d", __LINE__, \
			pTrackerServer->ip_addr, pTrackerServer->port, \
			in_bytes, TRACKER_QUERY_STORAGE_STORE_BODY_LEN);
		return EINVAL;
	}

#define RECORD_LENGTH  (IP_ADDRESS_SIZE - 1 + FDFS_PROTO_PKG_LEN_SIZE)

	ipPortsLen = in_bytes - (FDFS_GROUP_NAME_MAX_LEN + 1);
	if (ipPortsLen % RECORD_LENGTH != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"tracker server %s:%d response data " \
			"length: "INT64_PRINTF_FORMAT" is invalid", \
			__LINE__, pTrackerServer->ip_addr, \
			pTrackerServer->port, in_bytes);
		return EINVAL;
	}

	*storage_count = ipPortsLen / RECORD_LENGTH;
	if (nMaxServerCount < *storage_count)
	{
		logError("file: "__FILE__", line: %d, " \
			"tracker server %s:%d response storage server " \
			 "count: %d, exceeds max server count: %d!", \
			__LINE__, pTrackerServer->ip_addr, \
			pTrackerServer->port, *storage_count, nMaxServerCount);
		return ENOSPC;
	}

	memset(storageServers, 0, sizeof(TrackerServerInfo) * nMaxServerCount);

	memcpy(returned_group_name, in_buff, FDFS_GROUP_NAME_MAX_LEN);
	p = in_buff + FDFS_GROUP_NAME_MAX_LEN;
	*(returned_group_name + FDFS_GROUP_NAME_MAX_LEN) = '\0';

	pServerEnd = storageServers + (*storage_count);
	for (pStorageServer=storageServers; pStorageServer<pServerEnd; \
		pStorageServer++)
	{
		pStorageServer->sock = -1;
		memcpy(pStorageServer->group_name, returned_group_name, \
				FDFS_GROUP_NAME_MAX_LEN);

		memcpy(pStorageServer->ip_addr, p, IP_ADDRESS_SIZE - 1);
		p += IP_ADDRESS_SIZE - 1;

		pStorageServer->port = (int)buff2long(p);
		p += FDFS_PROTO_PKG_LEN_SIZE;
	}

	*store_path_index = *p;

	return 0;
}

int tracker_delete_storage(TrackerServerGroup *pTrackerGroup, \
		const char *group_name, const char *ip_addr)
{
	TrackerHeader *pHeader;
	TrackerServerInfo tracker_server;
	TrackerServerInfo *pServer;
	TrackerServerInfo *pEnd;
	FDFSStorageInfo storage_infos[1];
	char out_buff[sizeof(TrackerHeader) + FDFS_GROUP_NAME_MAX_LEN + \
			IP_ADDRESS_SIZE];
	char in_buff[1];
	char *pInBuff;
	int64_t in_bytes;
	int result;
	int ipaddr_len;
	int storage_count;
	int enoent_count;

	enoent_count = 0;
	pEnd = pTrackerGroup->servers + pTrackerGroup->server_count;
	for (pServer=pTrackerGroup->servers; pServer<pEnd; pServer++)
	{
		memcpy(&tracker_server, pServer, sizeof(TrackerServerInfo));
		tracker_server.sock = -1;
		if ((result=tracker_connect_server(&tracker_server)) != 0)
		{
			return result;
		}

		result = tracker_list_servers(&tracker_server, \
				group_name, ip_addr, storage_infos, 1, \
				&storage_count);
		close(tracker_server.sock);
		if (result != 0 && result != ENOENT)
		{
			return result;
		}

		if (result == ENOENT || storage_count == 0)
		{
			enoent_count++;
			continue;
		}

		if (storage_infos[0].status == FDFS_STORAGE_STATUS_ONLINE
		   || storage_infos[0].status == FDFS_STORAGE_STATUS_ACTIVE)
		{
			return EBUSY;
		}
	}
	if (enoent_count == pTrackerGroup->server_count)
	{
		return ENOENT;
	}

	memset(out_buff, 0, sizeof(out_buff));
	pHeader = (TrackerHeader *)out_buff;
	snprintf(out_buff + sizeof(TrackerHeader), sizeof(out_buff) - \
			sizeof(TrackerHeader),  "%s", group_name);
	ipaddr_len = snprintf(out_buff + sizeof(TrackerHeader) + \
			FDFS_GROUP_NAME_MAX_LEN, \
			sizeof(out_buff) - sizeof(TrackerHeader) - \
			FDFS_GROUP_NAME_MAX_LEN,  "%s", ip_addr);
	
	long2buff(FDFS_GROUP_NAME_MAX_LEN + ipaddr_len, pHeader->pkg_len);
	pHeader->cmd = TRACKER_PROTO_CMD_SERVER_DELETE_STORAGE;

	enoent_count = 0;
	result = 0;
	for (pServer=pTrackerGroup->servers; pServer<pEnd; pServer++)
	{
		memcpy(&tracker_server, pServer, sizeof(TrackerServerInfo));
		tracker_server.sock = -1;
		if ((result=tracker_connect_server(&tracker_server)) != 0)
		{
			return result;
		}

		if ((result=tcpsenddata_nb(tracker_server.sock, out_buff, \
			sizeof(TrackerHeader) + FDFS_GROUP_NAME_MAX_LEN + 
			ipaddr_len, g_fdfs_network_timeout)) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"send data to tracker server %s:%d fail, " \
				"errno: %d, error info: %s", __LINE__, \
				tracker_server.ip_addr, tracker_server.port, \
				result, STRERROR(result));
		}
		else
		{
			pInBuff = in_buff;
			result = fdfs_recv_response(&tracker_server, \
					&pInBuff, 0, &in_bytes);
		}

		close(tracker_server.sock);
		if (result != 0)
		{
			if (result == ENOENT)
			{
				enoent_count++;
			}
			else if (result == EALREADY)
			{
			}
			else
			{
				return result;
			}
		}
	}

	if (enoent_count == pTrackerGroup->server_count)
	{
		return ENOENT;
	}

	return result == ENOENT ? 0 : result;
}

int tracker_get_storage_status(TrackerServerGroup *pTrackerGroup, \
		const char *group_name, const char *ip_addr, int *status)
{
	TrackerServerInfo tracker_server;
	TrackerServerInfo *pServer;
	TrackerServerInfo *pEnd;
	FDFSStorageInfo storage_infos[1];
	int result;
	int storage_count;

	*status = -1;
	pEnd = pTrackerGroup->servers + pTrackerGroup->server_count;
	for (pServer=pTrackerGroup->servers; pServer<pEnd; pServer++)
	{
		memcpy(&tracker_server, pServer, sizeof(TrackerServerInfo));
		tracker_server.sock = -1;
		if ((result=tracker_connect_server(&tracker_server)) != 0)
		{
			return result;
		}

		result = tracker_list_servers(&tracker_server, \
				group_name, ip_addr, storage_infos, 1, \
				&storage_count);
		close(tracker_server.sock);
		if (result != 0 && result != ENOENT)
		{
			return result;
		}

		if (result == ENOENT || storage_count == 0)
		{
			continue;
		}

		if (storage_infos[0].status > *status)
		{
			*status = storage_infos[0].status;
		}
	}
	if (*status == -1)
	{
		return ENOENT;
	}

	return 0;
}

