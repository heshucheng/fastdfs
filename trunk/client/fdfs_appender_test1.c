/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.csource.org/ for more detail.
**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "fdfs_client.h"
#include "fdfs_global.h"
#include "base64.h"
#include "sockopt.h"
#include "logger.h"
#include "fdfs_http_shared.h"

int writeToFileCallback(void *arg, const int64_t file_size, const char *data, \
                const int current_size)
{
	if (arg == NULL)
	{
		return EINVAL;
	}

	if (fwrite(data, current_size, 1, (FILE *)arg) != 1)
	{
		return errno != 0 ? errno : EIO;
	}

	return 0;
}

int uploadFileCallback(void *arg, const int64_t file_size, int sock)
{
	int64_t total_send_bytes;
	char *filename;

	if (arg == NULL)
	{
		return EINVAL;
	}

	filename = (char *)arg;
	return tcpsendfile(sock, filename, file_size, \
		g_fdfs_network_timeout, &total_send_bytes);
}

int main(int argc, char *argv[])
{
	char *conf_filename;
	char *local_filename;
	TrackerServerInfo *pTrackerServer;
	int result;
	TrackerServerInfo storageServer;
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	char file_id[256];
	char appender_file_id[256];
	FDFSMetaData meta_list[32];
	int meta_count;
	char token[32 + 1];
	char file_url[256];
	char szDatetime[20];
	char szPortPart[16];
	int url_len;
	time_t ts;
	int64_t file_size;
	int store_path_index;
	FDFSFileInfo file_info;
	int upload_type;
	const char *file_ext_name;

	printf("This is FastDFS client test program v%d.%02d\n" \
"\nCopyright (C) 2008, Happy Fish / YuQing\n" \
"\nFastDFS may be copied only under the terms of the GNU General\n" \
"Public License V3, which may be found in the FastDFS source kit.\n" \
"Please visit the FastDFS Home Page http://www.csource.org/ \n" \
"for more detail.\n\n" \
, g_fdfs_version.major, g_fdfs_version.minor);

	if (argc < 3)
	{
		printf("Usage: %s <config_file> <local_filename> " \
			"[FILE | BUFF | CALLBACK]\n", argv[0]);
		return 1;
	}

	log_init();
	g_log_context.log_level = LOG_DEBUG;

	conf_filename = argv[1];
	if ((result=fdfs_client_init(conf_filename)) != 0)
	{
		return result;
	}

	pTrackerServer = tracker_get_connection();
	if (pTrackerServer == NULL)
	{
		fdfs_client_destroy();
		return errno != 0 ? errno : ECONNREFUSED;
	}

	local_filename = argv[2];
	if (argc == 3)
	{
		upload_type = FDFS_UPLOAD_BY_FILE;
	}
	else
	{
		if (strcmp(argv[3], "BUFF") == 0)
		{
			upload_type = FDFS_UPLOAD_BY_BUFF;
		}
		else if (strcmp(argv[3], "CALLBACK") == 0)
		{
			upload_type = FDFS_UPLOAD_BY_CALLBACK;
		}
		else
		{
			upload_type = FDFS_UPLOAD_BY_FILE;
		}
	}

	store_path_index = 0;
	if ((result=tracker_query_storage_store(pTrackerServer, \
			&storageServer, &store_path_index)) != 0)
	{
		fdfs_client_destroy();
		printf("tracker_query_storage fail, " \
			"error no: %d, error info: %s\n", \
			result, STRERROR(result));
		return result;
	}

	printf("group_name=%s, ip_addr=%s, port=%d\n", \
		storageServer.group_name, \
		storageServer.ip_addr, \
		storageServer.port);

	if ((result=tracker_connect_server(&storageServer)) != 0)
	{
		fdfs_client_destroy();
		return result;
	}

	memset(&meta_list, 0, sizeof(meta_list));
	meta_count = 0;
	strcpy(meta_list[meta_count].name, "ext_name");
	strcpy(meta_list[meta_count].value, "jpg");
	meta_count++;
	strcpy(meta_list[meta_count].name, "width");
	strcpy(meta_list[meta_count].value, "160");
	meta_count++;
	strcpy(meta_list[meta_count].name, "height");
	strcpy(meta_list[meta_count].value, "80");
	meta_count++;
	strcpy(meta_list[meta_count].name, "file_size");
	strcpy(meta_list[meta_count].value, "115120");
	meta_count++;

	file_ext_name = fdfs_get_file_ext_name(local_filename);
	*group_name = '\0';
	if (upload_type == FDFS_UPLOAD_BY_FILE)
	{
		result = storage_upload_appender_by_filename1(pTrackerServer, \
				&storageServer, store_path_index, \
				local_filename, file_ext_name, \
				meta_list, meta_count, group_name, file_id);

		printf("storage_upload_appender_by_filename1\n");
	}
	else if (upload_type == FDFS_UPLOAD_BY_BUFF)
	{
		char *file_content;
		if ((result=getFileContent(local_filename, \
					&file_content, &file_size)) == 0)
		{
			result = storage_upload_appender_by_filebuff1( \
					pTrackerServer, &storageServer, \
					store_path_index, file_content, \
					file_size, file_ext_name, \
					meta_list, meta_count, \
					group_name, file_id);
			free(file_content);
		}

		printf("storage_upload_appender_by_filebuff1\n");
	}
	else
	{
		struct stat stat_buf;

		if (stat(local_filename, &stat_buf) == 0 && \
				S_ISREG(stat_buf.st_mode))
		{
			file_size = stat_buf.st_size;
			result = storage_upload_appender_by_callback1( \
					pTrackerServer, &storageServer, \
					store_path_index, uploadFileCallback, \
					local_filename, file_size, \
					file_ext_name, meta_list, meta_count, \
					group_name, file_id);
		}

		printf("storage_upload_appender_by_callback1\n");
	}

	if (result != 0)
	{
		printf("upload file fail, " \
			"error no: %d, error info: %s\n", \
			result, STRERROR(result));
		fdfs_quit(&storageServer);
		tracker_disconnect_server(&storageServer);
		fdfs_client_destroy();
		return result;
	}

	if (g_tracker_server_http_port == 80)
	{
		*szPortPart = '\0';
	}
	else
	{
		sprintf(szPortPart, ":%d", g_tracker_server_http_port);
	}

	url_len = sprintf(file_url, "http://%s%s/%s", \
			pTrackerServer->ip_addr, szPortPart, file_id);
	if (g_anti_steal_token)
	{
		ts = time(NULL);
		fdfs_http_gen_token(&g_anti_steal_secret_key, file_id, \
				ts, token);
		sprintf(file_url + url_len, "?token=%s&ts=%d", token, (int)ts);
	}

	printf("fild_id=%s\n", file_id);

	fdfs_get_file_info1(file_id, &file_info);
	printf("source ip address: %s\n", file_info.source_ip_addr);
	printf("file timestamp=%s\n", formatDatetime(
		file_info.create_timestamp, "%Y-%m-%d %H:%M:%S", \
		szDatetime, sizeof(szDatetime)));
	printf("file size="INT64_PRINTF_FORMAT"\n", file_info.file_size);
	printf("file crc32=%u\n", file_info.crc32);
	printf("file url: %s\n", file_url);

	//sleep(70);
	strcpy(appender_file_id, file_id);
	if (upload_type == FDFS_UPLOAD_BY_FILE)
	{
		result = storage_append_by_filename1(pTrackerServer, \
				&storageServer, local_filename, 
				appender_file_id);

		printf("storage_append_by_filename\n");
	}
	else if (upload_type == FDFS_UPLOAD_BY_BUFF)
	{
		char *file_content;
		if ((result=getFileContent(local_filename, \
				&file_content, &file_size)) == 0)
		{
			result = storage_append_by_filebuff1(pTrackerServer, \
				&storageServer, file_content, \
				file_size, appender_file_id);
			free(file_content);
		}

		printf("storage_append_by_filebuff1\n");
	}
	else
	{
		struct stat stat_buf;

		if (stat(local_filename, &stat_buf) == 0 && \
			S_ISREG(stat_buf.st_mode))
		{
			file_size = stat_buf.st_size;
			result = storage_append_by_callback1(pTrackerServer, \
					&storageServer, uploadFileCallback, \
					local_filename, file_size, \
					appender_file_id);
		}

		printf("storage_append_by_callback1\n");
	}

	if (result != 0)
	{
		printf("append file fail, " \
			"error no: %d, error info: %s\n", \
			result, STRERROR(result));
		fdfs_quit(&storageServer);
		tracker_disconnect_server(&storageServer);
		fdfs_client_destroy();
		return result;
	}
	printf("append file successfully.\n");
	fdfs_get_file_info1(appender_file_id, &file_info);
	printf("source ip address: %s\n", file_info.source_ip_addr);
	printf("file timestamp=%s\n", formatDatetime(
		file_info.create_timestamp, "%Y-%m-%d %H:%M:%S", \
		szDatetime, sizeof(szDatetime)));
	printf("file size="INT64_PRINTF_FORMAT"\n", file_info.file_size);

	fdfs_quit(&storageServer);
	tracker_disconnect_server(&storageServer);

	fdfs_quit(pTrackerServer);

	tracker_close_all_connections();
	fdfs_client_destroy();

	return result;
}

