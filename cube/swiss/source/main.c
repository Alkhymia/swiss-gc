#include <stdio.h>
#include <gccore.h>		/*** Wrapper to include common libogc headers ***/
#include <ogcsys.h>		/*** Needed for console support ***/
#include <ogc/color.h>
#include <ogc/exi.h>
#include <ogc/lwp.h>
#include <ogc/usbgecko.h>
#include <ogc/video_types.h>
#include <sdcard/card_cmn.h>
#include <ogc/machine/processor.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <sys/types.h>
#include <unistd.h>
#include <sdcard/gcsd.h>
#include "main.h"
#include "settings.h"
#include "info.h"
#include "swiss.h"
#include "bba.h"
#include "dvd.h"
#include "wkf.h"
#include "exi.h"
#include "httpd.h"
#include "config.h"
#include "gui/FrameBufferMagic.h"
#include "gui/IPLFontWrite.h"
#include "devices/deviceHandler.h"
#include "devices/fat/ata.h"
#include "aram/sidestep.h"
#include "devices/filemeta.h"

#define DEFAULT_FIFO_SIZE    (256*1024)//(64*1024) minimum

extern void __libogc_exit(int status);

GXRModeObj *vmode = NULL;				//Graphics Mode Object
void *gp_fifo = NULL;
u32 *xfb[2] = { NULL, NULL };   //Framebuffers
int whichfb = 0;       		 	    //Frame buffer toggle
u8 driveVersion[8];
file_handle* allFiles;   		//all the files in the current dir
int curMenuLocation = ON_FILLIST; //where are we on the screen?
int files = 0;                  //number of files in a directory
int curMenuSelection = 0;	      //menu selection
int curSelection = 0;		        //game selection
int needsDeviceChange = 0;
int needsRefresh = 0;
SwissSettings swissSettings;
char *knownExtensions[] = {".dol\0", ".iso\0", ".gcm\0", ".mp3\0", ".fzn\0", ".gci\0", ".dol+cli\0"};

int endsWith(char *str, char *end) {
	if(strlen(str) < strlen(end))
		return 0;
	int i;
	for(i = 0; i < strlen(end); i++)
		if(tolower((int)str[strlen(str)-i]) != tolower((int)end[strlen(end)-i]))
			return 0;
	return 1;
}

bool checkExtension(char *filename) {
	if(!swissSettings.hideUnknownFileTypes)
		return true;
	int i;
	for(i = 0; i < sizeof(knownExtensions)/sizeof(char*); i++) {
		if(endsWith(filename, knownExtensions[i])) {
			return true;
		}
	}
	return false;
}

static void ProperScanPADS()	{
	PAD_ScanPads(); 
}

void populateVideoStr(GXRModeObj *vmode) {
	switch(vmode->viTVMode) {
		case VI_TVMODE_NTSC_INT:     videoStr = NtscIntStr;     break;
		case VI_TVMODE_NTSC_DS:      videoStr = NtscDsStr;      break;
		case VI_TVMODE_NTSC_PROG:    videoStr = NtscProgStr;    break;
		case VI_TVMODE_PAL_INT:      videoStr = PalIntStr;      break;
		case VI_TVMODE_PAL_DS:       videoStr = PalDsStr;       break;
		case VI_TVMODE_PAL_PROG:     videoStr = PalProgStr;     break;
		case VI_TVMODE_MPAL_INT:     videoStr = MpalIntStr;     break;
		case VI_TVMODE_MPAL_DS:      videoStr = MpalDsStr;      break;
		case VI_TVMODE_MPAL_PROG:    videoStr = MpalProgStr;    break;
		case VI_TVMODE_EURGB60_INT:  videoStr = Eurgb60IntStr;  break;
		case VI_TVMODE_EURGB60_DS:   videoStr = Eurgb60DsStr;   break;
		case VI_TVMODE_EURGB60_PROG: videoStr = Eurgb60ProgStr; break;
		default:                     videoStr = UnkStr;
	}
}

void initialise_video(GXRModeObj *m) {
	VIDEO_Configure (m);
	if(xfb[0]) free(MEM_K1_TO_K0(xfb[0]));
	if(xfb[1]) free(MEM_K1_TO_K0(xfb[1]));
	xfb[0] = (u32 *) MEM_K0_TO_K1 (SYS_AllocateFramebuffer (m));
	xfb[1] = (u32 *) MEM_K0_TO_K1 (SYS_AllocateFramebuffer (m));
	VIDEO_ClearFrameBuffer (m, xfb[0], COLOR_BLACK);
	VIDEO_ClearFrameBuffer (m, xfb[1], COLOR_BLACK);
	VIDEO_SetNextFramebuffer (xfb[0]);
	VIDEO_SetPostRetraceCallback (ProperScanPADS);
	VIDEO_SetBlack (0);
	VIDEO_Flush ();
	VIDEO_WaitVSync ();
	if (m->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
	else while (VIDEO_GetNextField())   VIDEO_WaitVSync();
	
	// setup the fifo and then init GX
	if(gp_fifo == NULL) {
		gp_fifo = MEM_K0_TO_K1 (memalign (32, DEFAULT_FIFO_SIZE));
		memset (gp_fifo, 0, DEFAULT_FIFO_SIZE);
		GX_Init (gp_fifo, DEFAULT_FIFO_SIZE);
	}
	// clears the bg to color and clears the z buffer
	GX_SetCopyClear ((GXColor) {0, 0, 0, 0xFF}, GX_MAX_Z24);
	// init viewport
	GX_SetViewport (0, 0, m->fbWidth, m->efbHeight, 0, 1);
	// Set the correct y scaling for efb->xfb copy operation
	GX_SetDispCopyYScale ((f32) m->xfbHeight / (f32) m->efbHeight);
	GX_SetDispCopySrc (0, 0, m->fbWidth, m->efbHeight);
	GX_SetDispCopyDst (m->fbWidth, m->xfbHeight);
	GX_SetCopyFilter (m->aa, m->sample_pattern, GX_TRUE, m->vfilter);
	GX_SetFieldMode (m->field_rendering, ((m->viHeight == 2 * m->xfbHeight) ? GX_ENABLE : GX_DISABLE));
	if (m->aa)
		GX_SetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
	else
		GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	GX_SetCullMode (GX_CULL_NONE); // default in rsp init
	GX_CopyDisp (xfb[0], GX_TRUE); // This clears the efb
	GX_CopyDisp (xfb[0], GX_TRUE); // This clears the xfb
}

/* Initialise Video, PAD, DVD, Font */
void* Initialise (void)
{
	VIDEO_Init ();
	PAD_Init ();  
	DVD_Init(); 
	*(volatile unsigned long*)0xcc00643c = 0x00000000; //allow 32mhz exi bus
	
	// Disable IPL modchips to allow access to IPL ROM fonts
	ipl_set_config(6); 
	usleep(1000); //wait for modchip to disable (overkill)
	
	
	__SYS_ReadROM(IPLInfo,256,0);	// Read IPL tag

	// Wii has no IPL tags for "PAL" so let libOGC figure out the video mode
	if(!is_gamecube()) {
		vmode = VIDEO_GetPreferredMode(NULL); //Last mode used
	}
	else {	// Gamecube, determine based on IPL
		int retPAD = 0, retCnt = 10000;
		while(retPAD <= 0 && retCnt >= 0) { retPAD = PAD_ScanPads(); usleep(100); retCnt--; }
		// L Trigger held down ignores the fact that there's a component cable plugged in.
		if(VIDEO_HaveComponentCable() && !(PAD_ButtonsDown(0) & PAD_TRIGGER_L)) {
			if(strstr(IPLInfo,"MPAL")!=NULL) {
				swissSettings.sramVideo = 2;
				vmode = &TVMpal480Prog; //Progressive 480p
			}
			else if((strstr(IPLInfo,"PAL")!=NULL)) {
				swissSettings.sramVideo = 1;
				vmode = &TVPal576ProgScale; //Progressive 576p
			}
			else {
				swissSettings.sramVideo = 0;
				vmode = &TVNtsc480Prog; //Progressive 480p
			}
		}
		else {
			//try to use the IPL region
			if(strstr(IPLInfo,"MPAL")!=NULL) {
				swissSettings.sramVideo = 2;
				vmode = &TVMpal480IntDf;        //PAL-M
			}
			else if(strstr(IPLInfo,"PAL")!=NULL) {
				swissSettings.sramVideo = 1;
				vmode = &TVPal576IntDfScale;         //PAL
			}
			else {
				swissSettings.sramVideo = 0;
				vmode = &TVNtsc480IntDf;        //NTSC
			}
		}
	}
	initialise_video(vmode);
	populateVideoStr(vmode);

	init_font();
	DrawInit();
	whichfb = 0;
	
	drive_version(&driveVersion[0]);
	swissSettings.hasDVDDrive = *(u32*)&driveVersion[0] ? 1 : 0;
	
	if(!driveVersion[0]) {
		// Reset DVD if there was a IPL replacement that hasn't done that for us yet
		uiDrawObj_t *progBox = DrawPublish(DrawProgressBar(true, 0, "Initialise DVD .. (HOLD B if NO DVD Drive)"));
		dvd_reset();	// low-level, basic
		dvd_read_id();
		if(!(PAD_ButtonsHeld(0) & PAD_BUTTON_B)) {
			dvd_set_streaming(*(char*)0x80000008);
		}
		drive_version(&driveVersion[0]);
		swissSettings.hasDVDDrive = *(u32*)&driveVersion[0] ? 2 : 0;
		if(!swissSettings.hasDVDDrive) {
			DrawDispose(progBox);
			progBox = DrawPublish(DrawMessageBox(D_INFO, "No DVD Drive Detected !!"));
			sleep(2);
		}
		DrawDispose(progBox);
	}
	
	return xfb[0];
}

void load_config() {

	// Try to open up the config .ini in case it hasn't been opened already
	if(config_init()) {
		sprintf(txtbuffer,"Loaded %i entries from the config file",config_get_count());
		// TODO notification area this
		uiDrawObj_t *msgBox = DrawPublish(DrawMessageBox(D_INFO,txtbuffer));
		memcpy(&swissSettings, config_get_swiss_settings(), sizeof(SwissSettings));
		DrawDispose(msgBox);
	}
}

int comp(const void *a1, const void *b1)
{
	const file_handle* a = a1;
	const file_handle* b = b1;
	
	if(!a && b) return 1;
	if(a && !b) return -1;
	if(!a && !b) return 0;
	
	if((devices[DEVICE_CUR] == &__device_dvd) && ((dvdDiscTypeInt == GAMECUBE_DISC) || (dvdDiscTypeInt == MULTIDISC_DISC)))
	{
		if(a->size == DISC_SIZE && a->fileBase == 0)
			return -1;
		if(b->size == DISC_SIZE && b->fileBase == 0)
			return 1;
	}
	
	if(a->fileAttrib == IS_DIR && b->fileAttrib == IS_FILE)
		return -1;
	if(a->fileAttrib == IS_FILE && b->fileAttrib == IS_DIR)
		return 1;

	return strcasecmp(a->name, b->name);
}

void sortFiles(file_handle* dir, int num_files)
{
	if(num_files > 0) {
		qsort(&dir[0],num_files,sizeof(file_handle),comp);
	}
}

void free_files() {
	if(allFiles) {
		int i;
		for(i = 0; i < files; i++) {
			if(allFiles[i].meta) {
				if(allFiles[i].meta->banner) {
					free(allFiles[i].meta->banner);
					allFiles[i].meta->banner = NULL;
				}
				memset(allFiles[i].meta, 0, sizeof(file_meta));
				meta_free(allFiles[i].meta);
				allFiles[i].meta = NULL;
			}
		}
		free(allFiles);
		allFiles = NULL;
		files = 0;
	}
}

void scan_files() {
	free_files();
	// Read the directory/device TOC
	if(allFiles){ free(allFiles); allFiles = NULL; }
	print_gecko("Reading directory: %s\r\n",curFile.name);
	files = devices[DEVICE_CUR]->readDir(&curFile, &allFiles, -1);
	memcpy(&curDir, &curFile, sizeof(file_handle));
	sortFiles(allFiles, files);
	print_gecko("Found %i entries\r\n",files);
}

// Keep this list sorted
char *autoboot_dols[] = { "/boot.dol", "/boot2.dol" };
void load_auto_dol() {
	u8 rev_buf[sizeof(GITREVISION) - 1]; // Don't include the NUL termination in the comparison

	memcpy(&curFile, devices[DEVICE_CUR]->initial, sizeof(file_handle));
	scan_files();
	for (int i = 0; i < files; i++) {
		for (int f = 0; f < (sizeof(autoboot_dols) / sizeof(char *)); f++) {
			if (endsWith(allFiles[i].name, autoboot_dols[f])) {
				// Official Swiss releases have the short commit hash appended to
				// the end of the DOL, compare it to our own to make sure we don't
				// bootloop the same version
				devices[DEVICE_CUR]->seekFile(&allFiles[i],
						allFiles[i].size - sizeof(rev_buf),
						DEVICE_HANDLER_SEEK_SET);
				devices[DEVICE_CUR]->readFile(&allFiles[i], rev_buf, sizeof(rev_buf));
				if (memcmp(GITREVISION, rev_buf, sizeof(rev_buf)) != 0) {
					// Emulate some of the menu's behavior to satisfy boot_dol
					curSelection = i;
					memcpy(&curFile, &allFiles[i], sizeof(file_handle));
					boot_dol();
				}

				// If we've made it this far, we've already found an autoboot DOL,
				// the first one (boot.dol) is not cancellable, but the rest of the
				// list is
				if (PAD_ButtonsHeld(0) & PAD_BUTTON_Y) {
					return;
				}
			}
		}
	}
}

void main_loop()
{ 
	
	while(PAD_ButtonsHeld(0) & PAD_BUTTON_A) { VIDEO_WaitVSync (); }
	// We don't care if a subsequent device is "default"
	if(needsDeviceChange) {
		free_files();
		if(devices[DEVICE_CUR]) {
			devices[DEVICE_CUR]->deinit(devices[DEVICE_CUR]->initial);
		}
		devices[DEVICE_CUR] = NULL;
		needsDeviceChange = 0;
		needsRefresh = 1;
		curMenuLocation = ON_FILLIST;
		select_device(DEVICE_CUR);
		if(devices[DEVICE_CUR] != NULL) {
			memcpy(&curFile, devices[DEVICE_CUR]->initial, sizeof(file_handle));
			uiDrawObj_t *msgBox = DrawPublish(DrawProgressBar(true, 0, "Setting up device"));
			// If the user selected a device, make sure it's ready before we browse the filesystem
			sdgecko_setSpeed(EXI_SPEED32MHZ);
			if(!devices[DEVICE_CUR]->init( devices[DEVICE_CUR]->initial )) {
				needsDeviceChange = 1;
				deviceHandler_setDeviceAvailable(devices[DEVICE_CUR], false);
				DrawDispose(msgBox);
				return;
			}
			DrawDispose(msgBox);
			deviceHandler_setDeviceAvailable(devices[DEVICE_CUR], true);	
		}
		else {
			curMenuLocation=ON_OPTIONS;
		}
	}

	uiDrawObj_t *backPanel = DrawContainer();
	uiDrawObj_t *buttonPanel = DrawMenuButtons((curMenuLocation==ON_OPTIONS)?curMenuSelection:-1);
	uiDrawObj_t *filePanel = NULL;
	DrawAddChild(backPanel, buttonPanel);
	DrawPublish(backPanel);
	while(1) {
		if(devices[DEVICE_CUR] != NULL && needsRefresh) {
			curMenuLocation=ON_OPTIONS;
			curSelection=0; curMenuSelection=0;
			scan_files();
			if(files<1) { devices[DEVICE_CUR]->deinit(devices[DEVICE_CUR]->initial); needsDeviceChange=1; break;}
			needsRefresh = 0;
			curMenuLocation=ON_FILLIST;
		}
		DrawUpdateMenuButtons(buttonPanel, (curMenuLocation==ON_OPTIONS)?curMenuSelection:-1);
		if(devices[DEVICE_CUR] != NULL && curMenuLocation==ON_FILLIST) {
			filePanel = renderFileBrowser(&allFiles, files);
			while(PAD_ButtonsHeld(0) & (PAD_BUTTON_B | PAD_BUTTON_A | PAD_BUTTON_RIGHT | PAD_BUTTON_LEFT)) {
				VIDEO_WaitVSync (); 
			}
		}
		else if (curMenuLocation==ON_OPTIONS) {
			u16 btns = PAD_ButtonsHeld(0);
			while (!((btns=PAD_ButtonsHeld(0)) & (PAD_BUTTON_B | PAD_BUTTON_A | PAD_BUTTON_RIGHT | PAD_BUTTON_LEFT))) {
				VIDEO_WaitVSync();
			}
			
			if(btns & PAD_BUTTON_LEFT){	curMenuSelection = (--curMenuSelection < 0) ? (MENU_MAX-1) : curMenuSelection;}
			else if(btns & PAD_BUTTON_RIGHT){curMenuSelection = (curMenuSelection + 1) % MENU_MAX;	}

			if(btns & PAD_BUTTON_A) {
				//handle menu event
				switch(curMenuSelection) {
					case 0:		// Device change
						needsDeviceChange = 1;  //Change from SD->DVD or vice versa
						break;
					case 1:		// Settings
						show_settings(NULL, NULL);
						break;
					case 2:		// Credits
						show_info();
						break;
					case 3:
						if(devices[DEVICE_CUR] != NULL) {
							memcpy(&curFile, devices[DEVICE_CUR]->initial, sizeof(file_handle));
							if(devices[DEVICE_CUR] == &__device_wkf) { 
								wkfReinit(); devices[DEVICE_CUR]->deinit(devices[DEVICE_CUR]->initial);
							}
						}
						needsRefresh=1;
						break;
					case 4:
						__libogc_exit(0);
						break;
				}
			}
			if((btns & PAD_BUTTON_B) && devices[DEVICE_CUR] != NULL) {
				curMenuLocation = ON_FILLIST;
			}
			while(PAD_ButtonsHeld(0) & (PAD_BUTTON_B | PAD_BUTTON_A | PAD_BUTTON_RIGHT | PAD_BUTTON_LEFT)) {
				VIDEO_WaitVSync (); 
			}
		}
		if(needsDeviceChange) {
			break;
		}
	}
	if(filePanel != NULL) {
		DrawDispose(filePanel);
	}
	DrawDispose(backPanel);
}

/****************************************************************************
 * USB Gecko Debugging
 ***************************************************************************/

static bool gecko = false;
static mutex_t gecko_mutex = 0;

static ssize_t __out_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
	if (!gecko || len == 0)
		return len;
	
	if(!ptr || len < 0)
		return -1;

	u32 level;
	LWP_MutexLock(gecko_mutex);
	level = IRQ_Disable();
	usb_sendbuffer(1, ptr, len);
	IRQ_Restore(level);
	LWP_MutexUnlock(gecko_mutex);
	return len;
}
#include <stdio.h>
#include <sys/iosupport.h>
const devoptab_t gecko_out = {
	"stdout",	// device name
	0,			// size of file structure
	NULL,		// device open
	NULL,		// device close
	__out_write,// device write
	NULL,		// device read
	NULL,		// device seek
	NULL,		// device fstat
	NULL,		// device stat
	NULL,		// device link
	NULL,		// device unlink
	NULL,		// device chdir
	NULL,		// device rename
	NULL,		// device mkdir
	0,			// dirStateSize
	NULL,		// device diropen_r
	NULL,		// device dirreset_r
	NULL,		// device dirnext_r
	NULL,		// device dirclose_r
	NULL		// device statvfs_r
};

static void USBGeckoOutput()
{
	gecko = usb_isgeckoalive(1);
	LWP_MutexInit(&gecko_mutex, false);

	devoptab_list[STD_OUT] = &gecko_out;
	devoptab_list[STD_ERR] = &gecko_out;
}


/****************************************************************************
* Main
****************************************************************************/
int main () 
{
	// Setup defaults (if no config is found)
	memset(&swissSettings, 0 , sizeof(SwissSettings));

	// Register all devices supported (order matters for boot devices)
	int i = 0;
	for(i = 0; i < MAX_DEVICES; i++)
		allDevices[i] = NULL;
	i = 0;
	allDevices[i++] = &__device_wkf;
	allDevices[i++] = &__device_wode;
	allDevices[i++] = &__device_sd_a;
	allDevices[i++] = &__device_sd_b;
	allDevices[i++] = &__device_card_a;
	allDevices[i++] = &__device_card_b;
	allDevices[i++] = &__device_dvd;
	allDevices[i++] = &__device_ide_a;
	allDevices[i++] = &__device_ide_b;
	allDevices[i++] = &__device_qoob;
	allDevices[i++] = &__device_smb;
	allDevices[i++] = &__device_sys;
	allDevices[i++] = &__device_usbgecko;
	allDevices[i++] = &__device_ftp;
	allDevices[i++] = &__device_sd_c;
	allDevices[i++] = NULL;
	
	// Set current devices
	devices[DEVICE_CUR] = NULL;
	devices[DEVICE_DEST] = NULL;
	devices[DEVICE_TEMP] = NULL;
	devices[DEVICE_CONFIG] = NULL;
	devices[DEVICE_PATCHES] = NULL;
	
	USBGeckoOutput();
	void *fb;
	fb = Initialise();
	if(!fb) {
		return -1;
	}

	// Sane defaults
	refreshSRAM();
	swissSettings.debugUSB = 1;
	swissSettings.gameVMode = 0;	// Auto video mode
	swissSettings.exiSpeed = 1;		// 32MHz
	swissSettings.uiVMode = 0; 		// Auto UI mode
	swissSettings.enableFileManagement = 0;

	config_copy_swiss_settings(&swissSettings);
	needsDeviceChange = 1;
	needsRefresh = 1;
	
	//debugging stuff
	if(swissSettings.debugUSB) {
		if(usb_isgeckoalive(1)) {
			usb_flush(1);
		}
		print_gecko("Arena Size: %iKb\r\n",(SYS_GetArena1Hi()-SYS_GetArena1Lo())/1024);
		print_gecko("DVD Drive Present? %s\r\n",swissSettings.hasDVDDrive?"Yes":"No");
		print_gecko("GIT Commit: %s\r\n", GITREVISION);
		print_gecko("GIT Revision: %s\r\n", GITVERSION);
	}
	
	// Go through all devices with FEAT_BOOT_DEVICE feature and set it as current if one is available
	for(i = 0; i < MAX_DEVICES; i++) {
		if(allDevices[i] != NULL && (allDevices[i]->features & FEAT_BOOT_DEVICE)) {
			print_gecko("Testing device %s\r\n", allDevices[i]->deviceName);
			if(allDevices[i]->test()) {
				deviceHandler_setDeviceAvailable(allDevices[i], true);
				devices[DEVICE_CUR] = allDevices[i];
				break;
			}
		}
	}
	if(devices[DEVICE_CUR] != NULL) {
		print_gecko("Detected %s\r\n", devices[DEVICE_CUR]->deviceName);
		if(devices[DEVICE_CUR]->init(devices[DEVICE_CUR]->initial)) {
			if(devices[DEVICE_CUR]->features & FEAT_AUTOLOAD_DOL) {
				load_auto_dol();
			}
			memcpy(&curFile, devices[DEVICE_CUR]->initial, sizeof(file_handle));
			needsDeviceChange = 0;
		}
	}

	// Scan here since some devices would already be initialised (faster)
	populateDeviceAvailability();
	
	// load config
	load_config();
	
	if(swissSettings.initNetworkAtStart) {
		// Start up the BBA if it exists
		uiDrawObj_t *msgBox = DrawPublish(DrawProgressBar(true, 0, "Initialising Network"));
		init_network();
		init_httpd_thread();
		DrawDispose(msgBox);
	}
	
	// DVD Motor off setting; Always stop the drive if we only started it to read the ID out
	if((swissSettings.stopMotor && swissSettings.hasDVDDrive) || (swissSettings.hasDVDDrive == 2)) {
		dvd_motor_off();
	}

	// Swiss video mode force
	GXRModeObj *forcedMode = getModeFromSwissSetting(swissSettings.uiVMode);
	
	if((forcedMode != NULL) && (forcedMode != vmode)) {
		initialise_video(forcedMode);
		vmode = forcedMode;
	}

	while(1) {
		main_loop();
	}
	return 0;
}

GXRModeObj *getModeFromSwissSetting(int uiVMode) {
	switch(uiVMode) {
		case 1:
			switch(swissSettings.sramVideo) {
				case 2:  return &TVMpal480IntDf;
				case 1:  return &TVEurgb60Hz480IntDf;
				default: return &TVNtsc480IntDf;
			}
		case 2:
			if(VIDEO_HaveComponentCable()) {
				switch(swissSettings.sramVideo) {
					case 2:  return &TVMpal480Prog;
					case 1:  return &TVEurgb60Hz480Prog;
					default: return &TVNtsc480Prog;
				}
			} else {
				switch(swissSettings.sramVideo) {
					case 2:  return &TVMpal480IntDf;
					case 1:  return &TVEurgb60Hz480IntDf;
					default: return &TVNtsc480IntDf;
				}
			}
		case 3:
			return &TVPal576IntDfScale;
		case 4:
			if(VIDEO_HaveComponentCable()) {
				return &TVPal576ProgScale;
			} else {
				return &TVPal576IntDfScale;
			}
	}
	return vmode;
}

// Checks if devices are available, prints name of device being detected for slow init devices
void populateDeviceAvailability() {
	if(PAD_ButtonsHeld(0) & PAD_BUTTON_B) {
		deviceHandler_setAllDevicesAvailable();
		return;
	}
	uiDrawObj_t *msgBox = DrawPublish(DrawProgressBar(true, 0, "Detecting devices ...\nThis can be skipped by holding B next time"));
	int i;
	for(i = 0; i < MAX_DEVICES; i++) {
		if(allDevices[i] != NULL && !deviceHandler_getDeviceAvailable(allDevices[i])) {
			print_gecko("Checking device availability for device %s\r\n", allDevices[i]->deviceName);
			deviceHandler_setDeviceAvailable(allDevices[i], allDevices[i]->test());
		}
		if(PAD_ButtonsHeld(0) & PAD_BUTTON_B) {
			deviceHandler_setAllDevicesAvailable();
			break;
		}
	}
	DrawDispose(msgBox);
}
