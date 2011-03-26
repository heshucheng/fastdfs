/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.csource.org/ for more detail.
**/

//trunk_mem.h

#ifndef _TRUNK_MEM_H_
#define _TRUNK_MEM_H_

#include <pthread.h>
#include "common_define.h"
#include "tracker_types.h"
#include "fast_mblock.h"

#define FDFS_TRUNK_STATUS_FREE  0
#define FDFS_TRUNK_STATUS_HOLD  1

#define FDFS_TRUNK_FILE_INFO_LEN  16

#define FDFS_TRUNK_FILE_ALLOC_SIZE_OFFSET   0
#define FDFS_TRUNK_FILE_FILE_SIZE_OFFSET    4
#define FDFS_TRUNK_FILE_FILE_CRC32_OFFSET   8
#define FDFS_TRUNK_FILE_FILE_MTIME_OFFSET  12
#define FDFS_TRUNK_FILE_HEADER_SIZE	   16

#define TRUNK_CALC_SIZE(file_size) (FDFS_TRUNK_FILE_HEADER_SIZE + file_size)

#ifdef __cplusplus
extern "C" {
#endif

extern int g_slot_min_size;    //slot min size, such as 256 bytes
extern int g_trunk_file_size;  //the trunk file size, such as 64MB
extern int g_store_path_mode;  //store which path mode, fetch from tracker
extern int g_storage_reserved_mb;  //fetch from tracker
extern int g_avg_storage_reserved_mb;  //calc by above var: g_storage_reserved_mb
extern int g_store_path_index;  //store to which path
extern int g_current_trunk_file_id;  //current trunk file id
extern TrackerServerInfo g_trunk_server;  //the trunk server
extern bool g_if_use_trunk_file;   //if use trunk file
extern bool g_if_trunker_self;   //if am i trunk server

typedef struct tagFDFSTrunkHeader {
	int alloc_size;
	int file_size;
	int crc32;
	int mtime;
} FDFSTrunkHeader;

typedef struct tagFDFSTrunkPathInfo {
	unsigned char store_path_index;   //store which path as Mxx
	unsigned char sub_path_high;      //high sub dir index, front part of HH/HH
	unsigned char sub_path_low;       //low sub dir index, tail part of HH/HH
} FDFSTrunkPathInfo;

typedef struct tagFDFSTrunkFileInfo {
	int id;      //trunk file id
	int offset;  //file offset
	int size;    //space size
} FDFSTrunkFileInfo;

typedef struct tagFDFSTrunkFullInfo {
	char status;  //normal or hold
	FDFSTrunkPathInfo path;
	FDFSTrunkFileInfo file;
} FDFSTrunkFullInfo;

typedef struct tagFDFSTrunkNode {
	FDFSTrunkFullInfo trunk;    //trunk info
	struct fast_mblock_node *pMblockNode;   //for free
	struct tagFDFSTrunkNode *next;
} FDFSTrunkNode;

typedef struct {
	int size;
	FDFSTrunkNode *free_trunk_head;
	pthread_mutex_t lock;
} FDFSTrunkSlot;

int storage_trunk_init();

int trunk_alloc_space(const int size, FDFSTrunkFullInfo *pResult);
int trunk_alloc_confirm(const FDFSTrunkFullInfo *pTrunkInfo, const int status);

int trunk_free_space(const FDFSTrunkFullInfo *pTrunkInfo);

#define TRUNK_GET_FILENAME(file_id, filename) \
	sprintf(filename, "%06d", file_id)

void trunk_file_info_encode(const FDFSTrunkFileInfo *pTrunkFile, char *str);
void trunk_file_info_decode(char *str, FDFSTrunkFileInfo *pTrunkFile);

bool trunk_check_size(const int64_t file_size);

char *trunk_get_full_filename(const FDFSTrunkFullInfo *pTrunkInfo, \
		char *full_filename, const int buff_size);

void trunk_pack_header(const FDFSTrunkHeader *pTrunkHeader, char *buff);
void trunk_unpack_header(const char *buff, FDFSTrunkHeader *pTrunkHeader);

#define trunk_init_file(filename) \
	trunk_init_file_ex(filename, g_trunk_file_size)

#define trunk_check_and_init_file(filename) \
	trunk_check_and_init_file_ex(filename, g_trunk_file_size)

int trunk_init_file_ex(const char *filename, const int64_t file_size);

int trunk_check_and_init_file_ex(const char *filename, const int64_t file_size);

#ifdef __cplusplus
}
#endif

#endif
