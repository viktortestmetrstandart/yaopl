#ifndef __BDM_SUPPORT_H
#define __BDM_SUPPORT_H

#include "include/iosupport.h"

#define BDM_MODE_UPDATE_DELAY MENU_UPD_DELAY_GENREFRESH

#include "include/mcemu.h"

typedef struct
{
    int active;       /* Activation flag */
    u64 start_sector; /* Start sector of vmc file */
    int flags;        /* Card flag */
    vmc_spec_t specs; /* Card specifications */
} bdm_vmc_infos_t;

#define MAX_BDM_DEVICES 5

#define BDM_TYPE_UNKNOWN -1
#define BDM_TYPE_USB     0
#define BDM_TYPE_ILINK   1
#define BDM_TYPE_SDC     2
#define BDM_TYPE_ATA     3
#define BDM_TYPE_UDPBD   4

typedef struct
{
    int massDeviceIndex; // Underlying device index backing the mass fs partition, ex: usb0 = 0, usb1 = 1, etc.
    int bdmUnit;         // Current massN: unit index (0..MAX_BDM_DEVICES-1), or -1 if disconnected
    char bdmPrefix[40];  // Contains the full path to the folder where all the games are.
    int bdmULSizePrev;
    time_t bdmModifiedCDPrev;
    time_t bdmModifiedDVDPrev;
    int bdmGameCount;
    base_game_info_t *bdmGames;
    char bdmDriver[32];
    int bdmDeviceType;      // Target/actual BDM device type (BDM_TYPE_*)
    int bdmDeviceTick;      // Used alongside BdmGeneration to tell if device data needs to be refreshed
    int bdmHddIsLBA48;      // 1 if the HDD supports LBA48, 0 if the HDD only supports LBA28
    int ataHighestUDMAMode; // Highest UDMA mode supported by the HDD
    unsigned char ThemesLoaded;
    unsigned char LanguagesLoaded;
    unsigned char ForceRefresh;
    unsigned char bdmConnected;
    unsigned char bdmProbing;
} bdm_device_data_t;

void bdmInit(item_list_t *itemList);
int bdmFindPartition(char *target, const char *name, int write);
int bdmFindSettingsDevice(char *target);
int bdmIsReadOnlyPath(const char *path);
void bdmLoadBasicModules(void);
void bdmLoadModules(void);
int bdmEnsureDriverLoaded(int targetType);
int bdmUDPBDLoaded(void);
void bdmLaunchGame(item_list_t *itemList, int id, config_set_t *configSet);

void bdmInitSemaphore();
void bdmEnumerateDevices();

item_list_t *bdmGetDriverObject(int targetType, int slotMode);
int bdmIsDriverConnected(int targetType);
int bdmIsDriverProbing(int targetType);
void bdmResetDriverProbe(int targetType);

void bdmResolveLBA_UDMA(bdm_device_data_t *pDeviceData);
int bdmHDDIsPresent(u32 timeoutMs);

#endif
