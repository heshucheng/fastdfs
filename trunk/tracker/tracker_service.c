/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.csource.org/ for more detail.
**/

//tracker_service.c

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include "fdfs_define.h"
#include "base64.h"
#include "logger.h"
#include "fdfs_global.h"
#include "sockopt.h"
#include "shared_func.h"
#include "pthread_func.h"
#include "tracker_types.h"
#include "tracker_global.h"
#include "tracker_mem.h"
#include "tracker_proto.h"
#include "tracker_io.h"
#include "tracker_service.h"

#define PKG_LEN_PRINTF_FORMAT  "%d"

static pthread_mutex_t tracker_thread_lock;
static pthread_mutex_t lb_thread_lock;

int g_tracker_thread_count = 0;

static void *work_thread_entrance(void* arg);
static void wait_for_work_threads_exit();

int tracker_service_init()
{
	int result;
	struct thread_data *pThreadData;
	struct thread_data *pDataEnd;
	pthread_t tid;
	pthread_attr_t thread_attr;

	if ((result=init_pthread_lock(&tracker_thread_lock)) != 0)
	{
		return result;
	}

	if ((result=init_pthread_lock(&lb_thread_lock)) != 0)
	{
		return result;
	}

	if ((result=init_pthread_attr(&thread_attr, g_thread_stack_size)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"init_pthread_attr fail, program exit!", __LINE__);
		return result;
	}

	g_thread_data = (struct thread_data *)malloc(sizeof( \
				struct thread_data) * g_work_threads);
	if (g_thread_data == NULL)
	{
		logError("file: "__FILE__", line: %d, " \
			"malloc %d bytes fail, errno: %d, error info: %s", \
			__LINE__, (int)sizeof(struct thread_data) * \
			g_work_threads, errno, strerror(errno));
		return errno != 0 ? errno : ENOMEM;
	}

	g_tracker_thread_count = 0;
	pDataEnd = g_thread_data + g_work_threads;
	for (pThreadData=g_thread_data; pThreadData<pDataEnd; pThreadData++)
	{
		pThreadData->ev_base = event_base_new();
		if (pThreadData->ev_base == NULL)
		{
			result = errno != 0 ? errno : ENOMEM;
			logError("file: "__FILE__", line: %d, " \
				"event_base_new fail.", __LINE__);
			return result;
		}

		if (pipe(pThreadData->pipe_fds) != 0)
		{
			result = errno != 0 ? errno : EPERM;
			logError("file: "__FILE__", line: %d, " \
				"call pipe fail, " \
				"errno: %d, error info: %s", \
				__LINE__, result, strerror(result));
			break;
		}

		if ((result=set_nonblock(pThreadData->pipe_fds[0])) != 0)
		{
			break;
		}

		if ((result=pthread_create(&tid, &thread_attr, \
			work_thread_entrance, pThreadData)) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"create thread failed, startup threads: %d, " \
				"errno: %d, error info: %s", \
				__LINE__, g_tracker_thread_count, \
				result, strerror(result));
			break;
		}
		else
		{
			if ((result=pthread_mutex_lock(&tracker_thread_lock)) != 0)
			{
				logError("file: "__FILE__", line: %d, " \
					"call pthread_mutex_lock fail, " \
					"errno: %d, error info: %s", \
					__LINE__, result, strerror(result));
			}
			g_tracker_thread_count++;
			if ((result=pthread_mutex_unlock(&tracker_thread_lock)) != 0)
			{
				logError("file: "__FILE__", line: %d, " \
					"call pthread_mutex_lock fail, " \
					"errno: %d, error info: %s", \
					__LINE__, result, strerror(result));
			}
		}
	}

	pthread_attr_destroy(&thread_attr);

	return 0;
}

static void wait_for_work_threads_exit()
{
	while (g_tracker_thread_count != 0)
	{
		sleep(1);
	}
}

int tracker_service_destroy()
{
	wait_for_work_threads_exit();
	pthread_mutex_destroy(&tracker_thread_lock);
	pthread_mutex_destroy(&lb_thread_lock);

	return 0;
}

void tracker_accept_loop(int server_sock)
{
	int incomesock;
	struct sockaddr_in inaddr;
	unsigned int sockaddr_len;
	struct thread_data *pThreadData;

	while (g_continue_flag)
	{
		sockaddr_len = sizeof(inaddr);
		incomesock = accept(server_sock, (struct sockaddr*)&inaddr, &sockaddr_len);
		if (incomesock < 0) //error
		{
			if (!(errno == EINTR || errno == EAGAIN))
			{
				logError("file: "__FILE__", line: %d, " \
					"accept failed, " \
					"errno: %d, error info: %s", \
					__LINE__, errno, strerror(errno));
			}

			continue;
		}

		pThreadData = g_thread_data + incomesock % g_work_threads;
		if (write(pThreadData->pipe_fds[1], &incomesock, \
			sizeof(incomesock)) != sizeof(incomesock))
		{
			close(incomesock);
			logError("file: "__FILE__", line: %d, " \
				"call write failed, " \
				"errno: %d, error info: %s", \
				__LINE__, errno, strerror(errno));
		}
	}
}

static void *work_thread_entrance(void* arg)
{
	int result;
	struct thread_data *pThreadData;
	struct event ev_notify;

	pThreadData = (struct thread_data *)arg;
	do
	{
		event_set(&ev_notify, pThreadData->pipe_fds[0], \
			EV_READ | EV_PERSIST, recv_notify_read, NULL);
		if ((result=event_base_set(pThreadData->ev_base, &ev_notify)) != 0)
		{
			logCrit("file: "__FILE__", line: %d, " \
				"event_base_set fail.", __LINE__);
			break;
		}
		if ((result=event_add(&ev_notify, NULL)) != 0)
		{
			logCrit("file: "__FILE__", line: %d, " \
				"event_add fail.", __LINE__);
			break;
		}

		while (g_continue_flag)
		{
			event_base_loop(pThreadData->ev_base, 0);
		}
	} while (0);

	event_base_free(pThreadData->ev_base);

	if ((result=pthread_mutex_lock(&tracker_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_lock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
	}
	g_tracker_thread_count--;
	if ((result=pthread_mutex_unlock(&tracker_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_lock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
	}

	while (!g_thread_kill_done)  //waiting for kill signal
	{
		sleep(1);
	}

	return NULL;
}

/*
storage server list
*/
static int tracker_check_and_sync(struct fast_task_info *pTask, \
			const int status)
{
	FDFSStorageDetail **ppServer;
	FDFSStorageDetail **ppEnd;
	FDFSStorageBrief *pDestServer;
	TrackerClientInfo *pClientInfo;
	
	pClientInfo = (TrackerClientInfo *)pTask->arg;

	if (status != 0 || pClientInfo->pGroup == NULL ||
	pClientInfo->pGroup->chg_count == pClientInfo->pStorage->chg_count)
	{
		pTask->length = sizeof(TrackerHeader);
		return status;
	}

	pDestServer = (FDFSStorageBrief *)(pTask->data + sizeof(TrackerHeader));
	ppEnd = pClientInfo->pGroup->sorted_servers + \
			pClientInfo->pGroup->count;
	for (ppServer=pClientInfo->pGroup->sorted_servers; \
		ppServer<ppEnd; ppServer++)
	{
		pDestServer->status = (*ppServer)->status;
		memcpy(pDestServer->ip_addr, (*ppServer)->ip_addr, \
			IP_ADDRESS_SIZE);
		pDestServer++;
	}

	pTask->length = sizeof(TrackerHeader) + sizeof(FDFSStorageBrief) * \
				pClientInfo->pGroup->count;

	pClientInfo->pStorage->chg_count = pClientInfo->pGroup->chg_count;
	return status;
}

static int tracker_changelog_response(struct fast_task_info *pTask, \
		FDFSStorageDetail *pStorage)
{
	char filename[MAX_PATH_SIZE];
	int64_t changelog_fsize;
	int read_bytes;
	int chg_len;
	int result;
	int fd;

	changelog_fsize = g_changelog_fsize;
	chg_len = changelog_fsize - pStorage->changelog_offset;
	if (chg_len < 0)
	{
		chg_len = 0;
	}

	if (chg_len == 0)
	{
		pTask->length = sizeof(TrackerHeader);
		return 0;
	}

	if (chg_len > sizeof(TrackerHeader) + TRACKER_MAX_PACKAGE_SIZE)
	{
		chg_len = TRACKER_MAX_PACKAGE_SIZE - sizeof(TrackerHeader);
	}

	snprintf(filename, sizeof(filename), "%s/data/%s", g_fdfs_base_path,\
		 STORAGE_SERVERS_CHANGELOG_FILENAME);
	fd = open(filename, O_RDONLY);
	if (fd < 0)
	{
		result = errno != 0 ? errno : EACCES;
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, open changelog file %s fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pTask->client_ip, \
			filename, result, strerror(result));
		pTask->length = sizeof(TrackerHeader);
		return result;
	}

	if (pStorage->changelog_offset > 0 && \
		lseek(fd, pStorage->changelog_offset, SEEK_SET) < 0)
	{
		result = errno != 0 ? errno : EIO;
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, lseek changelog file %s fail, "\
			"errno: %d, error info: %s", \
			__LINE__, pTask->client_ip, \
			filename, result, strerror(result));
		close(fd);
		pTask->length = sizeof(TrackerHeader);
		return result;
	}

	read_bytes = read(fd, pTask->data + sizeof(TrackerHeader), chg_len);
	close(fd);

	if (read_bytes != chg_len)
	{
		result = errno != 0 ? errno : EIO;
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, read changelog file %s fail, "\
			"errno: %d, error info: %s", \
			__LINE__, pTask->client_ip, \
			filename, result, strerror(result));

		close(fd);
		pTask->length = sizeof(TrackerHeader);
		return result;
	}

	pStorage->changelog_offset += chg_len;
	tracker_save_storages();

	pTask->length = sizeof(TrackerHeader) + chg_len;
	return 0;
}

static int tracker_deal_changelog_req(struct fast_task_info *pTask)
{
	char *group_name;
	int result;
	FDFSGroupInfo *pGroup;
	FDFSStorageDetail *pStorage;
	TrackerClientInfo *pClientInfo;
	
	pClientInfo = (TrackerClientInfo *)pTask->arg;

	do
	{
	if (pClientInfo->pGroup != NULL && pClientInfo->pStorage != NULL)
	{  //already logined
		if (pTask->length != sizeof(TrackerHeader))
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip: %s, package size " \
				PKG_LEN_PRINTF_FORMAT" is not correct, " \
				"expect length = %d", __LINE__, \
				TRACKER_PROTO_CMD_STORAGE_CHANGELOG_REQ, \
				pTask->client_ip, pTask->length - \
				sizeof(TrackerHeader), 0);

			result = EINVAL;
			break;
		}

		pStorage = pClientInfo->pStorage;
		result = 0;
	}
	else
	{
		if (pTask->length - sizeof(TrackerHeader) != FDFS_GROUP_NAME_MAX_LEN)
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip: %s, package size " \
				PKG_LEN_PRINTF_FORMAT" is not correct, " \
				"expect length = %d", __LINE__, \
				TRACKER_PROTO_CMD_STORAGE_CHANGELOG_REQ, \
				pTask->client_ip, pTask->length - \
				sizeof(TrackerHeader), FDFS_GROUP_NAME_MAX_LEN);

			result = EINVAL;
			break;
		}

		group_name = pTask->data + sizeof(TrackerHeader);
		*(group_name + FDFS_GROUP_NAME_MAX_LEN) = '\0';
		pGroup = tracker_mem_get_group(group_name);
		if (pGroup == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, invalid group_name: %s", \
				__LINE__, pTask->client_ip, group_name);
			result = ENOENT;
			break;
		}

		pStorage = tracker_mem_get_storage(pGroup, pTask->client_ip);
		if (pStorage == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, group_name: %s, " \
				"storage server: %s not exist", \
				__LINE__, pTask->client_ip, \
				group_name, pTask->client_ip);
			result = ENOENT;
			break;
		}
		
		result = 0;
	}
	} while (0);

	if (result != 0)
	{
		pTask->length = sizeof(TrackerHeader);
		return result;
	}

	return tracker_changelog_response(pTask, pStorage);
}

static int tracker_deal_parameter_req(struct fast_task_info *pTask)
{
	if (pTask->length - sizeof(TrackerHeader) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			PKG_LEN_PRINTF_FORMAT" is not correct, " \
			"expect length = %d", __LINE__, \
			TRACKER_PROTO_CMD_STORAGE_PARAMETER_REQ, \
			pTask->client_ip, pTask->length - \
			sizeof(TrackerHeader), 0);

		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	pTask->length = sizeof(TrackerHeader) + \
		sprintf(pTask->data + sizeof(TrackerHeader), \
			"storage_ip_changed_auto_adjust=%d\n", \
			g_storage_ip_changed_auto_adjust);

	return 0;
}

static int tracker_deal_storage_replica_chg(struct fast_task_info *pTask)
{
	int server_count;
	FDFSStorageBrief *briefServers;
	int nPkgLen;

	nPkgLen = pTask->length - sizeof(TrackerHeader);
	if ((nPkgLen <= 0) || (nPkgLen % sizeof(FDFSStorageBrief) != 0))
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip addr: %s, " \
			"package size "PKG_LEN_PRINTF_FORMAT" " \
			"is not correct", \
			__LINE__, TRACKER_PROTO_CMD_STORAGE_REPLICA_CHG, \
			pTask->client_ip, nPkgLen);
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	server_count = nPkgLen / sizeof(FDFSStorageBrief);
	if (server_count > FDFS_MAX_SERVERS_EACH_GROUP)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip addr: %s, return storage count: %d" \
			" exceed max: %d", __LINE__, \
			pTask->client_ip, server_count, \
			FDFS_MAX_SERVERS_EACH_GROUP);
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	pTask->length = sizeof(TrackerHeader);
	briefServers = (FDFSStorageBrief *)(pTask->data + sizeof(TrackerHeader));
	return tracker_mem_sync_storages(((TrackerClientInfo *)pTask->arg)->pGroup, \
				briefServers, server_count);
}

static int tracker_deal_storage_report_status(struct fast_task_info *pTask)
{
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	FDFSGroupInfo *pGroup;
	FDFSStorageBrief *briefServers;

	if (pTask->length - sizeof(TrackerHeader) != FDFS_GROUP_NAME_MAX_LEN + \
			sizeof(FDFSStorageBrief))
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip addr: %s, " \
			"package size "PKG_LEN_PRINTF_FORMAT" " \
			"is not correct", __LINE__, \
			TRACKER_PROTO_CMD_STORAGE_REPORT_STATUS, \
			pTask->client_ip, pTask->length - sizeof(TrackerHeader));
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	memcpy(group_name, pTask->data + sizeof(TrackerHeader), \
			FDFS_GROUP_NAME_MAX_LEN);
	*(group_name + FDFS_GROUP_NAME_MAX_LEN) = '\0';
	pGroup = tracker_mem_get_group(group_name);
	if (pGroup == NULL)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, invalid group_name: %s", \
			__LINE__, pTask->client_ip, group_name);
		pTask->length = sizeof(TrackerHeader);
		return ENOENT;
	}

	pTask->length = sizeof(TrackerHeader);
	briefServers = (FDFSStorageBrief *)(pTask->data + \
			sizeof(TrackerHeader) + FDFS_GROUP_NAME_MAX_LEN);
	return tracker_mem_sync_storages(pGroup, briefServers, 1);
}

static int tracker_deal_storage_join(struct fast_task_info *pTask)
{
	TrackerStorageJoinBodyResp *pJoinBodyResp;
	TrackerStorageJoinBody *pBody;
	FDFSStorageJoinBody joinBody;
	int result;
	TrackerClientInfo *pClientInfo;
	
	pClientInfo = (TrackerClientInfo *)pTask->arg;

	if (pTask->length - sizeof(TrackerHeader) != \
			sizeof(TrackerStorageJoinBody))
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd: %d, client ip: %s, " \
			"package size "PKG_LEN_PRINTF_FORMAT" " \
			"is not correct, expect length: %d.", \
			__LINE__, TRACKER_PROTO_CMD_STORAGE_JOIN, \
			pTask->client_ip, pTask->length - sizeof(TrackerHeader),
			(int)sizeof(TrackerStorageJoinBody));
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	pBody = (TrackerStorageJoinBody *)(pTask->data + sizeof(TrackerHeader));

	memcpy(joinBody.group_name, pBody->group_name, FDFS_GROUP_NAME_MAX_LEN);
	joinBody.group_name[FDFS_GROUP_NAME_MAX_LEN] = '\0';
	if ((result=fdfs_validate_group_name(joinBody.group_name)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, invalid group_name: %s", \
			__LINE__, pTask->client_ip, \
			joinBody.group_name);
		pTask->length = sizeof(TrackerHeader);
		return result;
	}

	joinBody.storage_port = (int)buff2long(pBody->storage_port);
	if (joinBody.storage_port <= 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, invalid port: %d", \
			__LINE__, pTask->client_ip, \
			joinBody.storage_port);
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	joinBody.storage_http_port = (int)buff2long(pBody->storage_http_port);
	if (joinBody.storage_http_port < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, invalid http port: %d", \
			__LINE__, pTask->client_ip, \
			joinBody.storage_http_port);
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	joinBody.store_path_count = (int)buff2long(pBody->store_path_count);
	if (joinBody.store_path_count <= 0 || joinBody.store_path_count > 256)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, invalid store_path_count: %d", \
			__LINE__, pTask->client_ip, \
			joinBody.store_path_count);
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	joinBody.subdir_count_per_path = (int)buff2long( \
					pBody->subdir_count_per_path);
	if (joinBody.subdir_count_per_path <= 0 || \
	    joinBody.subdir_count_per_path > 256)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, invalid subdir_count_per_path: %d", \
			__LINE__, pTask->client_ip, \
			joinBody.subdir_count_per_path);
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	joinBody.upload_priority = (int)buff2long(pBody->upload_priority);
	joinBody.up_time = (time_t)buff2long(pBody->up_time);

	*(pBody->version + (sizeof(pBody->version) - 1)) = '\0';
	*(pBody->domain_name + (sizeof(pBody->domain_name) - 1)) = '\0';
	strcpy(joinBody.version, pBody->version);
	strcpy(joinBody.domain_name, pBody->domain_name);
	joinBody.init_flag = pBody->init_flag;
	joinBody.status = pBody->status;

	result = tracker_mem_add_group_and_storage(pClientInfo, \
			&joinBody, true);
	if (result != 0)
	{
		pTask->length = sizeof(TrackerHeader);
		return result;
	}

	pJoinBodyResp = (TrackerStorageJoinBodyResp *)(pTask->data + \
				sizeof(TrackerHeader));
	memset(pJoinBodyResp, 0, sizeof(TrackerStorageJoinBodyResp));

	if (pClientInfo->pStorage->psync_src_server != NULL)
	{
		strcpy(pJoinBodyResp->src_ip_addr, \
			pClientInfo->pStorage->psync_src_server->ip_addr);
	}

	pTask->length = sizeof(TrackerHeader) + \
			sizeof(TrackerStorageJoinBodyResp);
	return 0;
}

static int tracker_deal_server_delete_storage(struct fast_task_info *pTask)
{
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	char *pIpAddr;
	FDFSGroupInfo *pGroup;

	if (pTask->length - sizeof(TrackerHeader) <= \
			FDFS_GROUP_NAME_MAX_LEN)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			PKG_LEN_PRINTF_FORMAT" is not correct, " \
			"expect length > %d", __LINE__, \
			TRACKER_PROTO_CMD_SERVER_DELETE_STORAGE, \
			pTask->client_ip, pTask->length - \
			sizeof(TrackerHeader), FDFS_GROUP_NAME_MAX_LEN);
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	pTask->data[pTask->length] = '\0';

	memcpy(group_name, pTask->data + sizeof(TrackerHeader), \
			FDFS_GROUP_NAME_MAX_LEN);
	group_name[FDFS_GROUP_NAME_MAX_LEN] = '\0';
	pIpAddr = pTask->data + sizeof(TrackerHeader) + FDFS_GROUP_NAME_MAX_LEN;
	pGroup = tracker_mem_get_group(group_name);
	if (pGroup == NULL)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, invalid group_name: %s", \
			__LINE__, pTask->client_ip, group_name);
		pTask->length = sizeof(TrackerHeader);
		return ENOENT;
	}

	pTask->length = sizeof(TrackerHeader);
	return tracker_mem_delete_storage(pGroup, pIpAddr);
}

static int tracker_deal_active_test(struct fast_task_info *pTask)
{
	if (pTask->length - sizeof(TrackerHeader) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			PKG_LEN_PRINTF_FORMAT" is not correct, " \
			"expect length 0", __LINE__, \
			FDFS_PROTO_CMD_ACTIVE_TEST, pTask->client_ip, \
			pTask->length - sizeof(TrackerHeader));
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	pTask->length = sizeof(TrackerHeader);
	return 0;
}

static int tracker_deal_storage_report_ip_changed(struct fast_task_info *pTask)
{
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	FDFSGroupInfo *pGroup;
	char *pOldIpAddr;
	char *pNewIpAddr;
	
	if (pTask->length - sizeof(TrackerHeader) != \
			FDFS_GROUP_NAME_MAX_LEN + 2 * IP_ADDRESS_SIZE)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			PKG_LEN_PRINTF_FORMAT" is not correct, " \
			"expect length = %d", __LINE__, \
			TRACKER_PROTO_CMD_STORAGE_REPORT_IP_CHANGED, \
			pTask->client_ip, pTask->length - sizeof(TrackerHeader),\
			FDFS_GROUP_NAME_MAX_LEN + 2 * IP_ADDRESS_SIZE);
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	memcpy(group_name, pTask->data + sizeof(TrackerHeader), \
			FDFS_GROUP_NAME_MAX_LEN);
	*(group_name + FDFS_GROUP_NAME_MAX_LEN) = '\0';

	pOldIpAddr = pTask->data + sizeof(TrackerHeader) + \
			FDFS_GROUP_NAME_MAX_LEN;
	*(pOldIpAddr + (IP_ADDRESS_SIZE - 1)) = '\0';

	pNewIpAddr = pOldIpAddr + IP_ADDRESS_SIZE;
	*(pNewIpAddr + (IP_ADDRESS_SIZE - 1)) = '\0';

	pGroup = tracker_mem_get_group(group_name);
	if (pGroup == NULL)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, invalid group_name: %s", \
			__LINE__, pTask->client_ip, group_name);
		pTask->length = sizeof(TrackerHeader);
		return ENOENT;
	}

	if (strcmp(pNewIpAddr, pTask->client_ip) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, group_name: %s, " \
			"new ip address %s != client ip address %s", \
			__LINE__, pTask->client_ip, group_name, \
			pNewIpAddr, pTask->client_ip);
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	pTask->length = sizeof(TrackerHeader);
	return tracker_mem_storage_ip_changed(pGroup, \
			pOldIpAddr, pNewIpAddr);
}

static int tracker_deal_storage_sync_notify(struct fast_task_info *pTask)
{
	TrackerStorageSyncReqBody body;
	char sync_src_ip_addr[IP_ADDRESS_SIZE];
	bool bSaveStorages;
	TrackerClientInfo *pClientInfo;
	
	pClientInfo = (TrackerClientInfo *)pTask->arg;

	if (pTask->length  - sizeof(TrackerHeader) != \
			sizeof(TrackerStorageSyncReqBody))
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd: %d, client ip: %s, package size " \
			PKG_LEN_PRINTF_FORMAT" is not correct, " \
			"expect length: %d", __LINE__, \
			TRACKER_PROTO_CMD_STORAGE_SYNC_NOTIFY, \
			pTask->client_ip, pTask->length - sizeof(TrackerHeader),
			(int)sizeof(TrackerStorageSyncReqBody));
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	if (*(body.src_ip_addr) == '\0')
	{
	if (pClientInfo->pStorage->status == FDFS_STORAGE_STATUS_INIT || \
	    pClientInfo->pStorage->status == FDFS_STORAGE_STATUS_WAIT_SYNC || \
	    pClientInfo->pStorage->status == FDFS_STORAGE_STATUS_SYNCING)
	{
		pClientInfo->pStorage->status = FDFS_STORAGE_STATUS_ONLINE;
		pClientInfo->pGroup->chg_count++;
		tracker_save_storages();
	}

		pTask->length = sizeof(TrackerHeader);
		return 0;
	}

	bSaveStorages = false;
	if (pClientInfo->pStorage->status == FDFS_STORAGE_STATUS_INIT)
	{
		pClientInfo->pStorage->status = FDFS_STORAGE_STATUS_WAIT_SYNC;
		pClientInfo->pGroup->chg_count++;
		bSaveStorages = true;
	}

	if (pClientInfo->pStorage->psync_src_server == NULL)
	{
		memcpy(sync_src_ip_addr, body.src_ip_addr, IP_ADDRESS_SIZE);
		sync_src_ip_addr[IP_ADDRESS_SIZE-1] = '\0';

		pClientInfo->pStorage->psync_src_server = \
			tracker_mem_get_storage(pClientInfo->pGroup, \
				sync_src_ip_addr);
		if (pClientInfo->pStorage->psync_src_server == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, " \
				"sync src server: %s not exists", \
				__LINE__, pTask->client_ip, \
				sync_src_ip_addr);
			pTask->length = sizeof(TrackerHeader);
			return ENOENT;
		}

		if (pClientInfo->pStorage->psync_src_server->status == \
			FDFS_STORAGE_STATUS_DELETED)
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, " \
				"sync src server: %s already be deleted", \
				__LINE__, pTask->client_ip, \
				sync_src_ip_addr);
			pTask->length = sizeof(TrackerHeader);
			return ENOENT;
		}

		if (pClientInfo->pStorage->psync_src_server->status == \
			FDFS_STORAGE_STATUS_IP_CHANGED)
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, the ip address of " \
				"the sync src server: %s changed", \
				__LINE__, pTask->client_ip, \
				sync_src_ip_addr);
			pTask->length = sizeof(TrackerHeader);
			return ENOENT;
		}

		pClientInfo->pStorage->sync_until_timestamp = \
				(int)buff2long(body.until_timestamp);
		bSaveStorages = true;
	}

	if (bSaveStorages)
	{
		tracker_save_storages();
	}

	pTask->length = sizeof(TrackerHeader);
	return 0;
}

/**
pkg format:
Header
FDFS_GROUP_NAME_MAX_LEN bytes: group_name
**/
static int tracker_deal_server_list_group_storages(struct fast_task_info *pTask)
{
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	char ip_addr[IP_ADDRESS_SIZE];
	char *pStorageIp;
	FDFSGroupInfo *pGroup;
	FDFSStorageDetail **ppServer;
	FDFSStorageDetail **ppEnd;
	FDFSStorageStat *pStorageStat;
	TrackerStorageStat *pStart;
	TrackerStorageStat *pDest;
	FDFSStorageStatBuff *pStatBuff;
	int nPkgLen;

	nPkgLen = pTask->length - sizeof(TrackerHeader);
	if (nPkgLen < FDFS_GROUP_NAME_MAX_LEN || \
		nPkgLen >= FDFS_GROUP_NAME_MAX_LEN + IP_ADDRESS_SIZE)
	{
		logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip: %s, package size " \
				PKG_LEN_PRINTF_FORMAT" is not correct, " \
				"expect length >= %d && <= %d", __LINE__, \
				TRACKER_PROTO_CMD_SERVER_LIST_STORAGE, \
				pTask->client_ip,  \
				nPkgLen, FDFS_GROUP_NAME_MAX_LEN, \
				FDFS_GROUP_NAME_MAX_LEN + IP_ADDRESS_SIZE);
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	memcpy(group_name, pTask->data + sizeof(TrackerHeader), \
			FDFS_GROUP_NAME_MAX_LEN);
	group_name[FDFS_GROUP_NAME_MAX_LEN] = '\0';
	pGroup = tracker_mem_get_group(group_name);
	if (pGroup == NULL)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, invalid group_name: %s", \
			__LINE__, pTask->client_ip, group_name);
		pTask->length = sizeof(TrackerHeader);
		return ENOENT;
	}

	if (nPkgLen > FDFS_GROUP_NAME_MAX_LEN)
	{
		pStorageIp = ip_addr;
		memcpy(pStorageIp, pTask->data + sizeof(TrackerHeader) + \
			FDFS_GROUP_NAME_MAX_LEN, IP_ADDRESS_SIZE);
		*(pStorageIp + (IP_ADDRESS_SIZE - 1)) = '\0';
	}
	else
	{
		pStorageIp = NULL;
	}

	memset(pTask->data + sizeof(TrackerHeader), 0, \
			pTask->size - sizeof(TrackerHeader));
	pDest = pStart = (TrackerStorageStat *)(pTask->data + sizeof(TrackerHeader));
	ppEnd = pGroup->sorted_servers + pGroup->count;
	for (ppServer=pGroup->sorted_servers; ppServer<ppEnd; \
			ppServer++)
	{
		if (pStorageIp != NULL && strcmp(pStorageIp, \
					(*ppServer)->ip_addr) != 0)
		{
			continue;
		}

		pStatBuff = &(pDest->stat_buff);
		pStorageStat = &((*ppServer)->stat);
		pDest->status = (*ppServer)->status;
		memcpy(pDest->ip_addr, (*ppServer)->ip_addr, \
				IP_ADDRESS_SIZE);
		if ((*ppServer)->psync_src_server != NULL)
		{
			memcpy(pDest->src_ip_addr, \
					(*ppServer)->psync_src_server->ip_addr,\
					IP_ADDRESS_SIZE);
		}

		strcpy(pDest->domain_name, (*ppServer)->domain_name);
		strcpy(pDest->version, (*ppServer)->version);
		long2buff((*ppServer)->up_time, pDest->sz_up_time);
		long2buff((*ppServer)->total_mb, pDest->sz_total_mb);
		long2buff((*ppServer)->free_mb, pDest->sz_free_mb);
		long2buff((*ppServer)->upload_priority, \
				pDest->sz_upload_priority);
		long2buff((*ppServer)->storage_port, \
				pDest->sz_storage_port);
		long2buff((*ppServer)->storage_http_port, \
				pDest->sz_storage_http_port);
		long2buff((*ppServer)->store_path_count, \
				pDest->sz_store_path_count);
		long2buff((*ppServer)->subdir_count_per_path, \
				pDest->sz_subdir_count_per_path);
		long2buff((*ppServer)->current_write_path, \
				pDest->sz_current_write_path);

		long2buff(pStorageStat->total_upload_count, \
				pStatBuff->sz_total_upload_count);
		long2buff(pStorageStat->success_upload_count, \
				pStatBuff->sz_success_upload_count);
		long2buff(pStorageStat->total_set_meta_count, \
				pStatBuff->sz_total_set_meta_count);
		long2buff(pStorageStat->success_set_meta_count, \
				pStatBuff->sz_success_set_meta_count);
		long2buff(pStorageStat->total_delete_count, \
				pStatBuff->sz_total_delete_count);
		long2buff(pStorageStat->success_delete_count, \
				pStatBuff->sz_success_delete_count);
		long2buff(pStorageStat->total_download_count, \
				pStatBuff->sz_total_download_count);
		long2buff(pStorageStat->success_download_count, \
				pStatBuff->sz_success_download_count);
		long2buff(pStorageStat->total_get_meta_count, \
				pStatBuff->sz_total_get_meta_count);
		long2buff(pStorageStat->success_get_meta_count, \
				pStatBuff->sz_success_get_meta_count);
		long2buff(pStorageStat->last_source_update, \
				pStatBuff->sz_last_source_update);
		long2buff(pStorageStat->last_sync_update, \
				pStatBuff->sz_last_sync_update);
		long2buff(pStorageStat->last_synced_timestamp, \
				pStatBuff->sz_last_synced_timestamp);
		long2buff(pStorageStat->total_create_link_count, \
				pStatBuff->sz_total_create_link_count);
		long2buff(pStorageStat->success_create_link_count, \
				pStatBuff->sz_success_create_link_count);
		long2buff(pStorageStat->total_delete_link_count, \
				pStatBuff->sz_total_delete_link_count);
		long2buff(pStorageStat->success_delete_link_count, \
				pStatBuff->sz_success_delete_link_count);
		long2buff(pStorageStat->last_heart_beat_time, \
				pStatBuff->sz_last_heart_beat_time);

		pDest++;
	}

	if (pStorageIp != NULL && pDest - pStart == 0)
	{
		pTask->length = sizeof(TrackerHeader);
		return ENOENT;
	}

	pTask->length = sizeof(TrackerHeader) + (pDest - pStart) * \
				sizeof(TrackerStorageStat);
	return 0;
}

/**
pkg format:
Header
FDFS_GROUP_NAME_MAX_LEN bytes: group_name
remain bytes: filename
**/
static int tracker_deal_service_query_fetch_update( \
		struct fast_task_info *pTask, const byte cmd)
{
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	char *filename;
	char *p;
	FDFSGroupInfo *pGroup;
	FDFSStorageDetail **ppServer;
	FDFSStorageDetail **ppServerEnd;
	FDFSStorageDetail *ppStoreServers[FDFS_MAX_SERVERS_EACH_GROUP];
	int filename_len;
	int server_count;
	int result;

	if (pTask->length - sizeof(TrackerHeader) < FDFS_GROUP_NAME_MAX_LEN + 22)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			PKG_LEN_PRINTF_FORMAT" is not correct, " \
			"expect length > %d", \
			__LINE__, cmd, pTask->client_ip,  \
			pTask->length - sizeof(TrackerHeader), \
			FDFS_GROUP_NAME_MAX_LEN+22);
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	pTask->data[pTask->length] = '\0';

	memcpy(group_name, pTask->data + sizeof(TrackerHeader), \
			FDFS_GROUP_NAME_MAX_LEN);
	group_name[FDFS_GROUP_NAME_MAX_LEN] = '\0';
	filename = pTask->data + sizeof(TrackerHeader)+FDFS_GROUP_NAME_MAX_LEN;
	filename_len = pTask->length - sizeof(TrackerHeader) - \
			FDFS_GROUP_NAME_MAX_LEN;

	result = tracker_mem_get_storage_by_filename(cmd, \
			FDFS_DOWNLOAD_TYPE_CALL \
			group_name, filename, filename_len, &pGroup, \
			ppStoreServers, &server_count);

	if (result != 0)
	{
		pTask->length = sizeof(TrackerHeader);
		return result;
	}


	pTask->length = sizeof(TrackerHeader) + \
			TRACKER_QUERY_STORAGE_FETCH_BODY_LEN + \
			(server_count - 1) * (IP_ADDRESS_SIZE - 1);

	p  = pTask->data + sizeof(TrackerHeader);
	memcpy(p, pGroup->group_name, FDFS_GROUP_NAME_MAX_LEN);
	p += FDFS_GROUP_NAME_MAX_LEN;
	memcpy(p, ppStoreServers[0]->ip_addr, IP_ADDRESS_SIZE-1);
	p += IP_ADDRESS_SIZE - 1;
	long2buff(pGroup->storage_port, p);
	p += FDFS_PROTO_PKG_LEN_SIZE;

	if (server_count > 1)
	{
		ppServerEnd = ppStoreServers + server_count;
		for (ppServer=ppStoreServers+1; ppServer<ppServerEnd; \
				ppServer++)
		{
			memcpy(p, (*ppServer)->ip_addr, \
					IP_ADDRESS_SIZE - 1);
			p += IP_ADDRESS_SIZE - 1;
		}
	}

	return 0;
}

static int tracker_deal_service_query_storage( \
		struct fast_task_info *pTask, char cmd)
{
	int expect_pkg_len;
	FDFSGroupInfo *pStoreGroup;
	FDFSGroupInfo **ppFoundGroup;
	FDFSGroupInfo **ppGroup;
	FDFSStorageDetail *pStorageServer;
	char *group_name;
	char *p;
	bool bHaveActiveServer;
	int write_path_index;
	int avg_reserved_mb;

	if (cmd == TRACKER_PROTO_CMD_SERVICE_QUERY_STORE_WITH_GROUP)
	{
		expect_pkg_len = FDFS_GROUP_NAME_MAX_LEN;
	}
	else
	{
		expect_pkg_len = 0;
	}

	if (pTask->length - sizeof(TrackerHeader) != expect_pkg_len)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			PKG_LEN_PRINTF_FORMAT"is not correct, " \
			"expect length: %d", __LINE__, \
			cmd, pTask->client_ip, \
			pTask->length - sizeof(TrackerHeader), expect_pkg_len);
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	if (g_groups.count == 0)
	{
		pTask->length = sizeof(TrackerHeader);
		return ENOENT;
	}

	if (cmd == TRACKER_PROTO_CMD_SERVICE_QUERY_STORE_WITH_GROUP)
	{
		group_name = pTask->data + sizeof(TrackerHeader);
		group_name[FDFS_GROUP_NAME_MAX_LEN] = '\0';

		pStoreGroup = tracker_mem_get_group(group_name);
		if (pStoreGroup == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, invalid group name: %s", \
				__LINE__, pTask->client_ip, group_name);
			pTask->length = sizeof(TrackerHeader);
			return ENOENT;
		}

		if (pStoreGroup->active_count == 0)
		{
			pTask->length = sizeof(TrackerHeader);
			return ENOENT;
		}

		if (pStoreGroup->free_mb <= g_storage_reserved_mb)
		{
			pTask->length = sizeof(TrackerHeader);
			return ENOSPC;
		}
	}
	else if (g_groups.store_lookup == FDFS_STORE_LOOKUP_ROUND_ROBIN
		||g_groups.store_lookup==FDFS_STORE_LOOKUP_LOAD_BALANCE)
	{
		int write_group_index;

		bHaveActiveServer = false;
		write_group_index = g_groups.current_write_group;
		if (write_group_index >= g_groups.count)
		{
			write_group_index = 0;
		}
		ppFoundGroup = g_groups.sorted_groups + write_group_index;
		if ((*ppFoundGroup)->active_count > 0)
		{
			bHaveActiveServer = true;
			if ((*ppFoundGroup)->free_mb > \
					g_storage_reserved_mb)
			{
				pStoreGroup = *ppFoundGroup;
			}
		}

		if (pStoreGroup == NULL)
		{
			FDFSGroupInfo **ppGroupEnd;
			ppGroupEnd = g_groups.sorted_groups + \
				     g_groups.count;
			for (ppGroup=ppFoundGroup+1; \
					ppGroup<ppGroupEnd; ppGroup++)
			{
				if ((*ppGroup)->active_count == 0)
				{
					continue;
				}

				bHaveActiveServer = true;
				if ((*ppGroup)->free_mb > \
						g_storage_reserved_mb)
				{
					pStoreGroup = *ppGroup;
					g_groups.current_write_group = \
					       ppGroup-g_groups.sorted_groups;
					break;
				}
			}

			if (pStoreGroup == NULL)
			{
				for (ppGroup=g_groups.sorted_groups; \
						ppGroup<ppFoundGroup; ppGroup++)
				{
					if ((*ppGroup)->active_count == 0)
					{
						continue;
					}

					bHaveActiveServer = true;
					if ((*ppGroup)->free_mb > \
							g_storage_reserved_mb)
					{
						pStoreGroup = *ppGroup;
						g_groups.current_write_group = \
						       ppGroup-g_groups.sorted_groups;
						break;
					}
				}
			}

			if (pStoreGroup == NULL)
			{
				pTask->length = sizeof(TrackerHeader);
				if (bHaveActiveServer)
				{
					return ENOSPC;
				}
				else
				{
					return ENOENT;
				}
			}
		}

		if (g_groups.store_lookup == FDFS_STORE_LOOKUP_ROUND_ROBIN)
		{
			g_groups.current_write_group++;
			if (g_groups.current_write_group >= g_groups.count)
			{
				g_groups.current_write_group = 0;
			}
		}
	}
	else if (g_groups.store_lookup == FDFS_STORE_LOOKUP_SPEC_GROUP)
	{
		if (g_groups.pStoreGroup == NULL || \
				g_groups.pStoreGroup->active_count == 0)
		{
			pTask->length = sizeof(TrackerHeader);
			return ENOENT;
		}

		if (g_groups.pStoreGroup->free_mb <= \
				g_storage_reserved_mb)
		{
			pTask->length = sizeof(TrackerHeader);
			return ENOSPC;
		}

		pStoreGroup = g_groups.pStoreGroup;
	}
	else
	{
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	if (pStoreGroup->store_path_count <= 0)
	{
		pTask->length = sizeof(TrackerHeader);
		return ENOENT;
	}

	pStorageServer = tracker_get_writable_storage(pStoreGroup);
	if (pStorageServer == NULL)
	{
		pTask->length = sizeof(TrackerHeader);
		return ENOENT;
	}

	write_path_index = pStorageServer->current_write_path;
	if (write_path_index >= pStoreGroup->store_path_count)
	{
		write_path_index = 0;
	}

	avg_reserved_mb = g_storage_reserved_mb / \
			  pStoreGroup->store_path_count;
	if (pStorageServer->path_free_mbs[write_path_index] <= \
			avg_reserved_mb)
	{
		int i;
		for (i=0; i<pStoreGroup->store_path_count; i++)
		{
			if (pStorageServer->path_free_mbs[i] > avg_reserved_mb)
			{
				pStorageServer->current_write_path = i;
				write_path_index = i;
				break;
			}
		}

		if (i == pStoreGroup->store_path_count)
		{
			pTask->length = sizeof(TrackerHeader);
			return ENOSPC;
		}
	}

	if (g_groups.store_path == FDFS_STORE_PATH_ROUND_ROBIN)
	{
		pStorageServer->current_write_path++;
		if (pStorageServer->current_write_path >= \
				pStoreGroup->store_path_count)
		{
			pStorageServer->current_write_path = 0;
		}
	}

	/*
	//printf("pStoreGroup->current_write_server: %d, " \
	"pStoreGroup->active_count=%d\n", \
	pStoreGroup->current_write_server, \
	pStoreGroup->active_count);
	*/

	p = pTask->data + sizeof(TrackerHeader);
	memcpy(p, pStoreGroup->group_name, FDFS_GROUP_NAME_MAX_LEN);
	p += FDFS_GROUP_NAME_MAX_LEN;

	memcpy(p, pStorageServer->ip_addr, IP_ADDRESS_SIZE - 1);
	p += IP_ADDRESS_SIZE - 1;
	
	long2buff(pStoreGroup->storage_port, p);
	p += FDFS_PROTO_PKG_LEN_SIZE;

	*p++ = (char)write_path_index;

	pTask->length = p - pTask->data;

	return 0;
}

static int tracker_deal_server_list_groups(struct fast_task_info *pTask)
{
	FDFSGroupInfo **ppGroup;
	FDFSGroupInfo **ppEnd;
	TrackerGroupStat *groupStats;
	TrackerGroupStat *pDest;

	if (pTask->length - sizeof(TrackerHeader) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			PKG_LEN_PRINTF_FORMAT" is not correct, " \
			"expect length: 0", __LINE__, \
			TRACKER_PROTO_CMD_SERVER_LIST_GROUP, \
			pTask->client_ip, pTask->length - sizeof(TrackerHeader));
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	groupStats = (TrackerGroupStat *)(pTask->data + sizeof(TrackerHeader));
	pDest = groupStats;
	ppEnd = g_groups.sorted_groups + g_groups.count;
	for (ppGroup=g_groups.sorted_groups; ppGroup<ppEnd; ppGroup++)
	{
		memcpy(pDest->group_name, (*ppGroup)->group_name, \
				FDFS_GROUP_NAME_MAX_LEN + 1);
		long2buff((*ppGroup)->free_mb, pDest->sz_free_mb);
		long2buff((*ppGroup)->count, pDest->sz_count);
		long2buff((*ppGroup)->storage_port, \
				pDest->sz_storage_port);
		long2buff((*ppGroup)->storage_http_port, \
				pDest->sz_storage_http_port);
		long2buff((*ppGroup)->active_count, \
				pDest->sz_active_count);
		long2buff((*ppGroup)->current_write_server, \
				pDest->sz_current_write_server);
		long2buff((*ppGroup)->store_path_count, \
				pDest->sz_store_path_count);
		long2buff((*ppGroup)->subdir_count_per_path, \
				pDest->sz_subdir_count_per_path);
		pDest++;
	}

	pTask->length = sizeof(TrackerHeader) + (pDest - groupStats) * \
			sizeof(TrackerGroupStat);

	return 0;
}

static int tracker_deal_storage_sync_src_req(struct fast_task_info *pTask)
{
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	FDFSGroupInfo *pGroup;
	char *dest_ip_addr;
	FDFSStorageDetail *pDestStorage;

	if (pTask->length - sizeof(TrackerHeader) != \
			FDFS_GROUP_NAME_MAX_LEN + IP_ADDRESS_SIZE)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			PKG_LEN_PRINTF_FORMAT" is not correct, " \
			"expect length: %d", __LINE__, \
			TRACKER_PROTO_CMD_STORAGE_SYNC_SRC_REQ, \
			pTask->client_ip, pTask->length-sizeof(TrackerHeader), \
			FDFS_GROUP_NAME_MAX_LEN + IP_ADDRESS_SIZE);
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	memcpy(group_name, pTask->data + sizeof(TrackerHeader), \
			FDFS_GROUP_NAME_MAX_LEN);
	*(group_name + FDFS_GROUP_NAME_MAX_LEN) = '\0';
	pGroup = tracker_mem_get_group(group_name);
	if (pGroup == NULL)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, invalid group_name: %s", \
			__LINE__, pTask->client_ip, group_name);
		pTask->length = sizeof(TrackerHeader);
		return ENOENT;
	}

	dest_ip_addr = pTask->data + sizeof(TrackerHeader) + \
			FDFS_GROUP_NAME_MAX_LEN;
	dest_ip_addr[IP_ADDRESS_SIZE - 1] = '\0';
	pDestStorage = tracker_mem_get_storage(pGroup, dest_ip_addr);
	if (pDestStorage == NULL)
	{
		pTask->length = sizeof(TrackerHeader);
		return ENOENT;
	}

	if (pDestStorage->status == FDFS_STORAGE_STATUS_INIT || \
		pDestStorage->status == FDFS_STORAGE_STATUS_DELETED || \
		pDestStorage->status == FDFS_STORAGE_STATUS_IP_CHANGED)
	{
		pTask->length = sizeof(TrackerHeader);
		return ENOENT;
	}

	pTask->length = sizeof(TrackerHeader);
	if (pDestStorage->psync_src_server != NULL)
	{
		if (pDestStorage->psync_src_server->status == \
				FDFS_STORAGE_STATUS_OFFLINE
			|| pDestStorage->psync_src_server->status == \
				FDFS_STORAGE_STATUS_ONLINE
			|| pDestStorage->psync_src_server->status == \
				FDFS_STORAGE_STATUS_ACTIVE)
		{
			TrackerStorageSyncReqBody *pBody;
			pBody = (TrackerStorageSyncReqBody *)(pTask->data + \
						sizeof(TrackerHeader));
			strcpy(pBody->src_ip_addr, \
					pDestStorage->psync_src_server->ip_addr);
			long2buff(pDestStorage->sync_until_timestamp, \
					pBody->until_timestamp);
			pTask->length += sizeof(TrackerStorageSyncReqBody);
		}
		else
		{
			pDestStorage->psync_src_server = NULL;
			tracker_save_storages();
		}
	}

	return 0;
}

static int tracker_deal_storage_sync_dest_req(struct fast_task_info *pTask)
{
	TrackerStorageSyncReqBody *pBody;
	FDFSStorageDetail *pSrcStorage;
	FDFSStorageDetail **ppServer;
	FDFSStorageDetail **ppServerEnd;
	int sync_until_timestamp;
	int source_count;
	TrackerClientInfo *pClientInfo;
	
	pClientInfo = (TrackerClientInfo *)pTask->arg;

	pSrcStorage = NULL;
	do
	{
	if (pTask->length - sizeof(TrackerHeader) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			PKG_LEN_PRINTF_FORMAT" is not correct, " \
			"expect length: 0", \
			__LINE__, TRACKER_PROTO_CMD_STORAGE_SYNC_DEST_REQ, \
			pTask->client_ip, pTask->length - sizeof(TrackerHeader));
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	if (pClientInfo->pGroup->count <= 1)
	{
		break;
	}

	source_count = 0;
	ppServerEnd = pClientInfo->pGroup->all_servers + \
		      pClientInfo->pGroup->count;
	for (ppServer=pClientInfo->pGroup->all_servers; \
			ppServer<ppServerEnd; ppServer++)
	{
		if (strcmp((*ppServer)->ip_addr, \
				pClientInfo->pStorage->ip_addr) == 0)
		{
			continue;
		}

		if ((*ppServer)->status ==FDFS_STORAGE_STATUS_OFFLINE 
			|| (*ppServer)->status == FDFS_STORAGE_STATUS_ONLINE
			|| (*ppServer)->status == FDFS_STORAGE_STATUS_ACTIVE)
		{
			source_count++;
		}
	}
	if (source_count == 0)
	{
		break;
	}

	pSrcStorage = tracker_get_group_sync_src_server( \
			pClientInfo->pGroup, pClientInfo->pStorage);
	if (pSrcStorage == NULL)
	{
		pTask->length = sizeof(TrackerHeader);
		return ENOENT;
	}

	pBody=(TrackerStorageSyncReqBody *)(pTask->data+sizeof(TrackerHeader));
	sync_until_timestamp = (int)time(NULL);
	strcpy(pBody->src_ip_addr, pSrcStorage->ip_addr);

	long2buff(sync_until_timestamp, pBody->until_timestamp);

	} while (0);

	if (pSrcStorage == NULL)
	{
		pClientInfo->pStorage->status = \
				FDFS_STORAGE_STATUS_ONLINE;
		pClientInfo->pGroup->chg_count++;
		tracker_save_storages();

		pTask->length = sizeof(TrackerHeader);
		return 0;
	}

	pClientInfo->pStorage->psync_src_server = pSrcStorage;
	pClientInfo->pStorage->sync_until_timestamp = sync_until_timestamp;
	pClientInfo->pStorage->status = FDFS_STORAGE_STATUS_WAIT_SYNC;
	pClientInfo->pGroup->chg_count++;

	tracker_save_storages();

	pTask->length = sizeof(TrackerHeader)+sizeof(TrackerStorageSyncReqBody);
	return 0;
}

static int tracker_deal_storage_sync_dest_query(struct fast_task_info *pTask)
{
	FDFSStorageDetail *pSrcStorage;
	TrackerClientInfo *pClientInfo;
	
	pClientInfo = (TrackerClientInfo *)pTask->arg;

	if (pTask->length - sizeof(TrackerHeader) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			PKG_LEN_PRINTF_FORMAT" is not correct, " \
			"expect length: 0", \
			__LINE__, TRACKER_PROTO_CMD_STORAGE_SYNC_DEST_QUERY, \
			pTask->client_ip, pTask->length - sizeof(TrackerHeader));
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	pTask->length = sizeof(TrackerHeader);
	pSrcStorage = pClientInfo->pStorage->psync_src_server;
	if (pSrcStorage != NULL)
	{
		TrackerStorageSyncReqBody *pBody;
		pBody = (TrackerStorageSyncReqBody *)(pTask->data + \
					sizeof(TrackerHeader));
		strcpy(pBody->src_ip_addr, pSrcStorage->ip_addr);

		long2buff(pClientInfo->pStorage->sync_until_timestamp, \
				pBody->until_timestamp);
		pTask->length += sizeof(TrackerStorageSyncReqBody);
	}


	return 0;
}

static void tracker_find_max_free_space_group()
{
	FDFSGroupInfo **ppGroup;
	FDFSGroupInfo **ppGroupEnd;
	FDFSGroupInfo **ppMaxGroup;

	ppMaxGroup = NULL;
	ppGroupEnd = g_groups.sorted_groups + g_groups.count;
	for (ppGroup=g_groups.sorted_groups; \
		ppGroup<ppGroupEnd; ppGroup++)
	{
		if ((*ppGroup)->active_count > 0)
		{
			if (ppMaxGroup == NULL)
			{
				ppMaxGroup = ppGroup;
			}
			else if ((*ppGroup)->free_mb > (*ppMaxGroup)->free_mb)
			{
				ppMaxGroup = ppGroup;
			}
		}
	}

	if (ppMaxGroup == NULL)
	{
		return;
	}

	g_groups.current_write_group = ppMaxGroup - g_groups.sorted_groups;
}

static void tracker_find_min_free_space(FDFSGroupInfo *pGroup)
{
	FDFSStorageDetail **ppServerEnd;
	FDFSStorageDetail **ppServer;

	if (pGroup->active_count == 0)
	{
		return;
	}

	pGroup->free_mb = (*(pGroup->active_servers))->free_mb;
	ppServerEnd = pGroup->active_servers + pGroup->active_count;
	for (ppServer=pGroup->active_servers+1; \
		ppServer<ppServerEnd; ppServer++)
	{
		if ((*ppServer)->free_mb < pGroup->free_mb)
		{
			pGroup->free_mb = (*ppServer)->free_mb;
		}
	}
}

static int tracker_deal_storage_sync_report(struct fast_task_info *pTask)
{
	char *p;
	char *pEnd;
	char *src_ip_addr;
	int status;
	int sync_timestamp;
	int src_index;
	int dest_index;
	int nPkgLen;
	FDFSStorageDetail *pSrcStorage;
	TrackerClientInfo *pClientInfo;
	
	pClientInfo = (TrackerClientInfo *)pTask->arg;

	nPkgLen = pTask->length - sizeof(TrackerHeader);
	if (nPkgLen <= 0 || nPkgLen % (IP_ADDRESS_SIZE + 4) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			PKG_LEN_PRINTF_FORMAT" is not correct", \
			__LINE__, TRACKER_PROTO_CMD_STORAGE_SYNC_REPORT, \
			pTask->client_ip, nPkgLen);

		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	do
	{
	dest_index = tracker_mem_get_storage_index(pClientInfo->pGroup,
			pClientInfo->pStorage);
	if (dest_index < 0 || dest_index >= pClientInfo->pGroup->count)
	{
		status = 0;
		break;
	}

	if (g_groups.store_server == FDFS_STORE_SERVER_ROUND_ROBIN)
	{
		int min_synced_timestamp;

		min_synced_timestamp = 0;
		pEnd = pTask->data + pTask->length;
		for (p=pTask->data + sizeof(TrackerHeader); p<pEnd; \
			p += (IP_ADDRESS_SIZE + 4))
		{
			sync_timestamp = buff2int(p + IP_ADDRESS_SIZE);
			if (sync_timestamp <= 0)
			{
				continue;
			}

			src_ip_addr = p;
			*(src_ip_addr + (IP_ADDRESS_SIZE - 1)) = '\0';
			pSrcStorage = tracker_mem_get_storage( \
					pClientInfo->pGroup, src_ip_addr);
			if (pSrcStorage == NULL)
			{
				continue;
			}
			if (pSrcStorage->status != FDFS_STORAGE_STATUS_ACTIVE)
			{
				continue;
			}

			src_index = tracker_mem_get_storage_index( \
					pClientInfo->pGroup, pSrcStorage);
			if (src_index == dest_index || src_index < 0 || \
					src_index >= pClientInfo->pGroup->count)
			{
				continue;
			}

			pClientInfo->pGroup->last_sync_timestamps \
				[src_index][dest_index] = sync_timestamp;

			if (min_synced_timestamp == 0)
			{
				min_synced_timestamp = sync_timestamp;
			}
			else if (sync_timestamp < min_synced_timestamp)
			{
				min_synced_timestamp = sync_timestamp;
			}
		}

		if (min_synced_timestamp > 0)
		{
			pClientInfo->pStorage->stat.last_synced_timestamp = \
							    min_synced_timestamp;
		}
	}
	else
	{
		int max_synced_timestamp;

		max_synced_timestamp = pClientInfo->pStorage->stat.\
				       last_synced_timestamp;
		pEnd = pTask->data + pTask->length;
		for (p=pTask->data + sizeof(TrackerHeader); p<pEnd; \
			p += (IP_ADDRESS_SIZE + 4))
		{
			sync_timestamp = buff2int(p + IP_ADDRESS_SIZE);
			if (sync_timestamp <= 0)
			{
				continue;
			}

			src_ip_addr = p;
			*(src_ip_addr + (IP_ADDRESS_SIZE - 1)) = '\0';
			pSrcStorage = tracker_mem_get_storage( \
					pClientInfo->pGroup, src_ip_addr);
			if (pSrcStorage == NULL)
			{
				continue;
			}
			if (pSrcStorage->status != FDFS_STORAGE_STATUS_ACTIVE)
			{
				continue;
			}

			src_index = tracker_mem_get_storage_index( \
					pClientInfo->pGroup, pSrcStorage);
			if (src_index == dest_index || src_index < 0 || \
					src_index >= pClientInfo->pGroup->count)
			{
				continue;
			}

			pClientInfo->pGroup->last_sync_timestamps \
				[src_index][dest_index] = sync_timestamp;

			if (sync_timestamp > max_synced_timestamp)
			{
				max_synced_timestamp = sync_timestamp;
			}
		}

		pClientInfo->pStorage->stat.last_synced_timestamp = \
						    max_synced_timestamp;
	}

	if (++g_storage_sync_time_chg_count % \
			TRACKER_SYNC_TO_FILE_FREQ == 0)
	{
		status = tracker_save_sync_timestamps();
	}
	else
	{
		status = 0;
	}
	} while (0);

	return tracker_check_and_sync(pTask, status);
}

static int tracker_deal_storage_df_report(struct fast_task_info *pTask)
{
	int nPkgLen;
	int result;
	int i;
	TrackerStatReportReqBody *pStatBuff;
	int64_t *path_total_mbs;
	int64_t *path_free_mbs;
	int64_t old_free_mb;
	TrackerClientInfo *pClientInfo;
	
	pClientInfo = (TrackerClientInfo *)pTask->arg;

	nPkgLen = pTask->length - sizeof(TrackerHeader);
	if (nPkgLen != sizeof(TrackerStatReportReqBody) * \
			pClientInfo->pGroup->store_path_count)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			PKG_LEN_PRINTF_FORMAT" is not correct, " \
			"expect length: %d", __LINE__, \
			TRACKER_PROTO_CMD_STORAGE_REPORT_DISK_USAGE, \
			pTask->client_ip, nPkgLen, \
			(int)sizeof(TrackerStatReportReqBody) * \
			pClientInfo->pGroup->store_path_count);
		pTask->length = sizeof(TrackerHeader);
		return EINVAL;
	}

	old_free_mb = pClientInfo->pStorage->free_mb;
	path_total_mbs = pClientInfo->pStorage->path_total_mbs;
	path_free_mbs = pClientInfo->pStorage->path_free_mbs;
	pClientInfo->pStorage->total_mb = 0;
	pClientInfo->pStorage->free_mb = 0;

	pStatBuff = (TrackerStatReportReqBody *)(pTask->data + sizeof(TrackerHeader));
	for (i=0; i<pClientInfo->pGroup->store_path_count; i++)
	{
		path_total_mbs[i] = buff2long(pStatBuff->sz_total_mb);
		path_free_mbs[i] = buff2long(pStatBuff->sz_free_mb);

		pClientInfo->pStorage->total_mb += path_total_mbs[i];
		pClientInfo->pStorage->free_mb += path_free_mbs[i];

		if (g_groups.store_path == FDFS_STORE_PATH_LOAD_BALANCE
				&& path_free_mbs[i] > path_free_mbs[ \
				pClientInfo->pStorage->current_write_path])
		{
			pClientInfo->pStorage->current_write_path = i;
		}

		pStatBuff++;
	}

	if ((pClientInfo->pGroup->free_mb == 0) ||
		(pClientInfo->pStorage->free_mb < pClientInfo->pGroup->free_mb))
	{
		pClientInfo->pGroup->free_mb = pClientInfo->pStorage->free_mb;
	}
	else if (pClientInfo->pStorage->free_mb > old_free_mb)
	{
		tracker_find_min_free_space(pClientInfo->pGroup);
	}

	if (g_groups.store_lookup == FDFS_STORE_LOOKUP_LOAD_BALANCE)
	{
		if ((result=pthread_mutex_lock(&lb_thread_lock)) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"call pthread_mutex_lock fail, " \
				"errno: %d, error info: %s", \
				__LINE__, result, strerror(result));
		}
		tracker_find_max_free_space_group();
		if ((result=pthread_mutex_unlock(&lb_thread_lock)) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"call pthread_mutex_unlock fail, " \
				"errno: %d, error info: %s", \
				__LINE__, result, strerror(result));
		}
	}

	/*
	//logInfo("storage: %s:%d, total_mb=%dMB, free_mb=%dMB\n", \
		pClientInfo->pStorage->ip_addr, \
		pClientInfo->pGroup->storage_port, \
		pClientInfo->pStorage->total_mb, \
		pClientInfo->pStorage->free_mb);
	*/


	tracker_mem_active_store_server(pClientInfo->pGroup, \
				pClientInfo->pStorage);

	return tracker_check_and_sync(pTask, 0);
}

static int tracker_deal_storage_beat(struct fast_task_info *pTask)
{
	int nPkgLen;
	int status;
	FDFSStorageStatBuff *pStatBuff;
	FDFSStorageStat *pStat;
	TrackerClientInfo *pClientInfo;
	
	pClientInfo = (TrackerClientInfo *)pTask->arg;

	do 
	{
		nPkgLen = pTask->length - sizeof(TrackerHeader);
		if (nPkgLen == 0)
		{
			status = 0;
			break;
		}

		if (nPkgLen != sizeof(FDFSStorageStatBuff))
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip: %s, package size " \
				PKG_LEN_PRINTF_FORMAT" is not correct, " \
				"expect length: 0 or %d", __LINE__, \
				TRACKER_PROTO_CMD_STORAGE_BEAT, \
				pTask->client_ip, nPkgLen, 
				(int)sizeof(FDFSStorageStatBuff));
			status = EINVAL;
			break;
		}

		pStatBuff = (FDFSStorageStatBuff *)(pTask->data + \
					sizeof(TrackerHeader));
		pStat = &(pClientInfo->pStorage->stat);

		pStat->total_upload_count = \
			buff2long(pStatBuff->sz_total_upload_count);
		pStat->success_upload_count = \
			buff2long(pStatBuff->sz_success_upload_count);
		pStat->total_download_count = \
			buff2long(pStatBuff->sz_total_download_count);
		pStat->success_download_count = \
			buff2long(pStatBuff->sz_success_download_count);
		pStat->total_set_meta_count = \
			buff2long(pStatBuff->sz_total_set_meta_count);
		pStat->success_set_meta_count = \
			buff2long(pStatBuff->sz_success_set_meta_count);
		pStat->total_delete_count = \
			buff2long(pStatBuff->sz_total_delete_count);
		pStat->success_delete_count = \
			buff2long(pStatBuff->sz_success_delete_count);
		pStat->total_get_meta_count = \
			buff2long(pStatBuff->sz_total_get_meta_count);
		pStat->success_get_meta_count = \
			buff2long(pStatBuff->sz_success_get_meta_count);
		pStat->last_source_update = \
			buff2long(pStatBuff->sz_last_source_update);
		pStat->last_sync_update = \
			buff2long(pStatBuff->sz_last_sync_update);
		pStat->total_create_link_count = \
			buff2long(pStatBuff->sz_total_create_link_count);
		pStat->success_create_link_count = \
			buff2long(pStatBuff->sz_success_create_link_count);
		pStat->total_delete_link_count = \
			buff2long(pStatBuff->sz_total_delete_link_count);
		pStat->success_delete_link_count = \
			buff2long(pStatBuff->sz_success_delete_link_count);

		if (++g_storage_stat_chg_count % TRACKER_SYNC_TO_FILE_FREQ == 0)
		{
			status = tracker_save_storages();
		}
		else
		{
			status = 0;
		}

		//printf("g_storage_stat_chg_count=%d\n", g_storage_stat_chg_count);

	} while (0);

	if (status == 0)
	{
		tracker_mem_active_store_server(pClientInfo->pGroup, \
				pClientInfo->pStorage);
		pClientInfo->pStorage->stat.last_heart_beat_time = time(NULL);

	}

	//printf("deal heart beat, status=%d\n", status);
	return tracker_check_and_sync(pTask, status);
}

#define TRACKER_CHECK_LOGINED(pTask) \
	if (((TrackerClientInfo *)pTask->arg)->pGroup == NULL || \
		((TrackerClientInfo *)pTask->arg)->pStorage == NULL) \
	{ \
		pTask->length = sizeof(TrackerHeader); \
		result = EACCES; \
		break; \
	} \


int tracker_deal_task(struct fast_task_info *pTask)
{
	TrackerHeader *pHeader;
	int result;

	pHeader = (TrackerHeader *)pTask->data;
	switch(pHeader->cmd)
	{
		case TRACKER_PROTO_CMD_STORAGE_BEAT:
			TRACKER_CHECK_LOGINED(pTask)
			result = tracker_deal_storage_beat(pTask);
			break;
		case TRACKER_PROTO_CMD_STORAGE_SYNC_REPORT:
			TRACKER_CHECK_LOGINED(pTask)
			result = tracker_deal_storage_sync_report(pTask);
			break;
		case TRACKER_PROTO_CMD_STORAGE_REPORT_DISK_USAGE:
			TRACKER_CHECK_LOGINED(pTask)
			result = tracker_deal_storage_df_report(pTask);
			break;
		case TRACKER_PROTO_CMD_STORAGE_JOIN:
			result = tracker_deal_storage_join(pTask);
			break;
		case TRACKER_PROTO_CMD_STORAGE_REPORT_STATUS:
			result = tracker_deal_storage_report_status(pTask);
			break;
		case TRACKER_PROTO_CMD_STORAGE_REPLICA_CHG:
			TRACKER_CHECK_LOGINED(pTask)
			result = tracker_deal_storage_replica_chg(pTask);
			break;
		case TRACKER_PROTO_CMD_SERVICE_QUERY_FETCH_ONE:
			result = tracker_deal_service_query_fetch_update( \
					pTask, pHeader->cmd);
			break;
		case TRACKER_PROTO_CMD_SERVICE_QUERY_UPDATE:
			result = tracker_deal_service_query_fetch_update( \
					pTask, pHeader->cmd);
			break;
		case TRACKER_PROTO_CMD_SERVICE_QUERY_FETCH_ALL:
			result = tracker_deal_service_query_fetch_update( \
					pTask, pHeader->cmd);
			break;
		case TRACKER_PROTO_CMD_SERVICE_QUERY_STORE_WITHOUT_GROUP:
			result = tracker_deal_service_query_storage( \
					pTask, pHeader->cmd);
			break;
		case TRACKER_PROTO_CMD_SERVICE_QUERY_STORE_WITH_GROUP:
			result = tracker_deal_service_query_storage( \
					pTask, pHeader->cmd);
			break;
		case TRACKER_PROTO_CMD_SERVER_LIST_GROUP:
			result = tracker_deal_server_list_groups(pTask);
			break;
		case TRACKER_PROTO_CMD_SERVER_LIST_STORAGE:
			result = tracker_deal_server_list_group_storages(pTask);
			break;
		case TRACKER_PROTO_CMD_STORAGE_SYNC_SRC_REQ:
			result = tracker_deal_storage_sync_src_req(pTask);
			break;
		case TRACKER_PROTO_CMD_STORAGE_SYNC_DEST_REQ:
			result = tracker_deal_storage_sync_dest_req(pTask);
			break;
		case TRACKER_PROTO_CMD_STORAGE_SYNC_NOTIFY:
			result = tracker_deal_storage_sync_notify(pTask);
			break;
		case TRACKER_PROTO_CMD_STORAGE_SYNC_DEST_QUERY:
			result = tracker_deal_storage_sync_dest_query(pTask);
			break;
		case TRACKER_PROTO_CMD_SERVER_DELETE_STORAGE:
			result = tracker_deal_server_delete_storage(pTask);
			break;
		case TRACKER_PROTO_CMD_STORAGE_REPORT_IP_CHANGED:
			result = tracker_deal_storage_report_ip_changed(pTask);
			break;
		case TRACKER_PROTO_CMD_STORAGE_CHANGELOG_REQ:
			result = tracker_deal_changelog_req(pTask);
			break;
		case TRACKER_PROTO_CMD_STORAGE_PARAMETER_REQ:
			result = tracker_deal_parameter_req(pTask);
			break;
		case FDFS_PROTO_CMD_QUIT:
			result = ECONNRESET;  //for quit loop
			break;
		case FDFS_PROTO_CMD_ACTIVE_TEST:
			result = tracker_deal_active_test(pTask);
			break;
		default:
			logError("file: "__FILE__", line: %d, "  \
					"client ip: %s, unkown cmd: %d", \
					__LINE__, pTask->client_ip, \
					pHeader->cmd);
			result = EINVAL;
			break;
	}

	pHeader = (TrackerHeader *)pTask->data;
	pHeader->status = result;
	pHeader->cmd = TRACKER_PROTO_CMD_RESP;
	long2buff(pTask->length - sizeof(TrackerHeader), pHeader->pkg_len);

	send_add_event(pTask);

	return 0;
}

