/* deviceHandler-wiikeyfusion.c
	- device implementation for Wiikey Fusion (FAT filesystem)
	by emu_kidid
 */

#include <fat.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ogc/dvd.h>
#include <ogc/machine/processor.h>
#include <sdcard/gcsd.h>
#include "deviceHandler.h"
#include "deviceHandler-FAT.h"
#include "gui/FrameBufferMagic.h"
#include "gui/IPLFontWrite.h"
#include "swiss.h"
#include "main.h"
#include "wkf.h"
#include "patcher.h"

const DISC_INTERFACE* wkf = &__io_wkf;
int wkfFragSetupReq = 0;
FATFS *wkffs = NULL;

file_handle initial_WKF =
	{ "wkf:/",       // directory
	  0ULL,     // fileBase (u64)
	  0,        // offset
	  0,        // size
	  IS_DIR,
	  0,
	  0
	};


device_info initial_WKF_info = {
	0,
	0
};
	
device_info* deviceHandler_WKF_info() {
	return &initial_WKF_info;
}

s32 deviceHandler_WKF_readDir(file_handle* ffile, file_handle** dir, u32 type){	
	return deviceHandler_FAT_readDir(ffile, dir, type);
}


s32 deviceHandler_WKF_seekFile(file_handle* file, u32 where, u32 type){
	if(type == DEVICE_HANDLER_SEEK_SET) file->offset = where;
	else if(type == DEVICE_HANDLER_SEEK_CUR) file->offset += where;
	return file->offset;
}


s32 deviceHandler_WKF_readFile(file_handle* file, void* buffer, u32 length){
	return deviceHandler_FAT_readFile(file, buffer, length);	// Same as FAT
}


s32 deviceHandler_WKF_writeFile(file_handle* file, void* buffer, u32 length){
	return -1;
}


s32 deviceHandler_WKF_setupFile(file_handle* file, file_handle* file2) {
	
	// If there are 2 discs, we only allow 21 fragments per disc.
	int maxFrags = (VAR_FRAG_SIZE/12), i = 0;
	vu32 *fragList = (vu32*)VAR_FRAG_LIST;
	s32 frags = 0, totFrags = 0;
	
	memset((void*)VAR_FRAG_LIST, 0, VAR_FRAG_SIZE);

	// Check if there are any fragments in our patch location for this game
	if(devices[DEVICE_PATCHES] != NULL) {
		print_gecko("Save Patch device found\r\n");
		
		// Look for patch files, if we find some, open them and add them as fragments
		file_handle patchFile;
		char gameID[8];
		memset(&gameID, 0, 8);
		strncpy((char*)&gameID, (char*)&GCMDisk, 4);
		
		for(i = 0; i < maxFrags; i++) {
			u32 patchInfo[4];
			patchInfo[0] = 0; patchInfo[1] = 0; 
			memset(&patchFile, 0, sizeof(file_handle));
			sprintf(&patchFile.name[0], "%sswiss_patches/%s/%i",devices[DEVICE_PATCHES]->initial->name,gameID, i);
			print_gecko("Looking for file %s\r\n", &patchFile.name);
			FILINFO fno;
			if(f_stat(&patchFile.name[0], &fno) != FR_OK) {
				break;	// Patch file doesn't exist, don't bother with fragments
			}
			
			devices[DEVICE_PATCHES]->seekFile(&patchFile,fno.fsize-16,DEVICE_HANDLER_SEEK_SET);
			if((devices[DEVICE_PATCHES]->readFile(&patchFile, &patchInfo, 16) == 16) && (patchInfo[2] == SWISS_MAGIC)) {
				if(!(frags = getFragments(&patchFile, &fragList[totFrags*3], maxFrags, patchInfo[0], patchInfo[1] | 0x80000000, DEVICE_PATCHES))) {
					return 0;
				}
				totFrags+=frags;
				devices[DEVICE_PATCHES]->closeFile(&patchFile);
			}
			else {
				break;
			}
		}
		// Check for igr.dol
		memset(&patchFile, 0, sizeof(file_handle));
		sprintf(&patchFile.name[0], "%sigr.dol", devices[DEVICE_PATCHES]->initial->name);

		FILINFO fno;
		if(f_stat(&patchFile.name[0], &fno) == FR_OK) {
			print_gecko("IGR Boot DOL exists\r\n");
			if((frags = getFragments(&patchFile, &fragList[totFrags*3], maxFrags, 0x60000000, 0, DEVICE_PATCHES))) {
				totFrags+=frags;
				devices[DEVICE_PATCHES]->closeFile(&patchFile);
				*(vu32*)VAR_IGR_DOL_SIZE = fno.fsize;
			}
		}
		// Copy the current speed
		*(vu32*)VAR_EXI_BUS_SPD = 192;
		// Card Type
		*(vu8*)VAR_SD_SHIFT = (9 * sdgecko_getAddressingType(((devices[DEVICE_PATCHES]->location == LOC_MEMCARD_SLOT_A) ? 0:1))) & 0xFF;
		// Copy the actual freq
		*(vu32*)VAR_EXI_FREQ = EXI_SPEED16MHZ;	// play it safe
		// Device slot (0 or 1) // This represents 0xCC0068xx in number of u32's so, slot A = 0xCC006800, B = 0xCC006814
		*(vu32*)VAR_EXI_SLOT = ((devices[DEVICE_PATCHES]->location == LOC_MEMCARD_SLOT_A) ? 0:1) * 5;
	}

	
	// No fragment room left for the actual game, fail.
	if(totFrags+1 == maxFrags) {
		return 0;
	}
	
	// If disc 1 is fragmented, make a note of the fragments and their sizes
	if(!(frags = getFragments(file, &fragList[totFrags*3], maxFrags, 0, 0, DEVICE_CUR))) {
		return 0;
	}
	totFrags += frags;
	
	// If there is a disc 2 and it's fragmented, make a note of the fragments and their sizes
	if(file2) {
		// No fragment room left for the second disc, fail.
		if(totFrags+1 == maxFrags) {
			return 0;
		}
		// TODO fix 2 disc patched games
		if(!(frags = getFragments(file2, &fragList[(maxFrags*3)], maxFrags, 0, 0, DEVICE_CUR))) {
			return 0;
		}
		totFrags += frags;
	}
		
	// Disk 1 base sector
	*(vu32*)VAR_DISC_1_LBA = fragList[2];
	// Disk 2 base sector
	*(vu32*)VAR_DISC_2_LBA = file2 ? fragList[2 + (maxFrags*3)]:fragList[2];
	// Currently selected disk base sector
	*(vu32*)VAR_CUR_DISC_LBA = fragList[2];
	
	wkfFragSetupReq = (file2 && frags > 2) ? 1 : frags>1;
	print_frag_list(file2 != 0);
	return 1;
}

s32 deviceHandler_WKF_init(file_handle* file){
	
	wkfReinit();
	if(wkffs != NULL) {
		f_mount(0, "wkf:/", 0);	// Unmount
		free(wkffs);
		wkffs = NULL;
	}
	wkffs = (FATFS*)malloc(sizeof(FATFS));
	int ret = 0;
	
	if(((ret=f_mount(wkffs, "wkf:/", 0)) == FR_OK) && deviceHandler_getStatEnabled()) {	
		sprintf(txtbuffer, "Reading filesystem info for wkf:/");
		uiDrawObj_t *msgBox = DrawPublish(DrawProgressBar(true, 0, txtbuffer));
		
		DWORD free_clusters, free_sectors, total_sectors = 0;
		if(f_getfree("wkf:/", &free_clusters, &wkffs) == FR_OK) {
			total_sectors = (wkffs->n_fatent - 2) * wkffs->csize;
			free_sectors = free_clusters * wkffs->csize;
			initial_WKF_info.freeSpaceInKB = (u32)((free_sectors)>>1);
			initial_WKF_info.totalSpaceInKB = (u32)((total_sectors)>>1);
		}
		DrawDispose(msgBox);
	}
	else {
		initial_WKF_info.freeSpaceInKB = initial_WKF_info.totalSpaceInKB = 0;	
	}

	return ret == FR_OK;
}

s32 deviceHandler_WKF_deinit(file_handle* file) {
	int ret = 0;
	if(file && file->ffsFp) {
		ret = f_close(file->ffsFp);
		free(file->ffsFp);
		file->ffsFp = 0;
	}
	return ret;
}

s32 deviceHandler_WKF_deleteFile(file_handle* file) {
	return -1;
}

s32 deviceHandler_WKF_closeFile(file_handle* file) {
    return 0;
}

bool deviceHandler_WKF_test() {
	return swissSettings.hasDVDDrive && (__wkfSpiReadId() != 0 && __wkfSpiReadId() != 0xFFFFFFFF);
}

DEVICEHANDLER_INTERFACE __device_wkf = {
	DEVICE_ID_B,
	"Wiikey / Wasp Fusion",
	"Supported File System(s): FAT16, FAT32, exFAT",
	{TEX_WIIKEY, 102, 80},
	FEAT_READ|FEAT_BOOT_GCM|FEAT_AUTOLOAD_DOL|FEAT_FAT_FUNCS|FEAT_BOOT_DEVICE|FEAT_CAN_READ_PATCHES,
	LOC_DVD_CONNECTOR,
	&initial_WKF,
	(_fn_test)&deviceHandler_WKF_test,
	(_fn_info)&deviceHandler_WKF_info,
	(_fn_init)&deviceHandler_WKF_init,
	(_fn_readDir)&deviceHandler_WKF_readDir,
	(_fn_readFile)&deviceHandler_WKF_readFile,
	(_fn_writeFile)NULL,
	(_fn_deleteFile)NULL,
	(_fn_seekFile)&deviceHandler_WKF_seekFile,
	(_fn_setupFile)&deviceHandler_WKF_setupFile,
	(_fn_closeFile)&deviceHandler_WKF_closeFile,
	(_fn_deinit)&deviceHandler_WKF_deinit
};
