#include "include/opl.h"
#include "include/lang.h"
#include "include/gui.h"
#include "include/supportbase.h"
#include "include/bdmsupport.h"
#include "include/util.h"
#include "include/themes.h"
#include "include/textures.h"
#include "include/ioman.h"
#include "include/system.h"
#include "include/ethsupport.h"
#include "include/extern_irx.h"
#include "include/cheatman.h"
#include "include/sound.h"
#include "modules/iopcore/common/cdvd_config.h"

#include <usbhdfsd-common.h>

#include <ps2sdkapi.h>
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <delaythread.h>

static int basicBdmModLoaded = 0;
static int usbModLoaded = 0;
static int iLinkModLoaded = 0;
static int mx4sioModLoaded = 0;
static int hddModLoaded = 0;
static int udpbdModLoaded = 0;
static s32 bdmLoadModuleLock = -1;
int bdmDeviceModeStarted;

// 4 dedicated driver-bound slots: [0]=UDPBD, [1]=USB, [2]=MX4SIO, [3]=ATA(BDM HDD)
#define BDM_SLOT_UDPBD 0
#define BDM_SLOT_USB   1
#define BDM_SLOT_SDC   2
#define BDM_SLOT_ATA   3
#define BDM_SLOT_COUNT 4

static item_list_t bdmDriverList[BDM_SLOT_COUNT];
static int bdmDriverListInitialized = 0;

void bdmInitDevicesData(void);
int bdmUpdateDeviceData(item_list_t *itemList);

static int bdmDriverTypeToSlot(int targetType)
{
    switch (targetType) {
        case BDM_TYPE_UDPBD:
            return BDM_SLOT_UDPBD;
        case BDM_TYPE_USB:
            return BDM_SLOT_USB;
        case BDM_TYPE_SDC:
            return BDM_SLOT_SDC;
        case BDM_TYPE_ATA:
            return BDM_SLOT_ATA;
        default:
            return -1;
    }
}

static const char *bdmTypeToDriverStr(int targetType)
{
    switch (targetType) {
        case BDM_TYPE_UDPBD:
            return "udp";
        case BDM_TYPE_USB:
            return "usb";
        case BDM_TYPE_SDC:
            return "sdc";
        case BDM_TYPE_ATA:
            return "ata";
        default:
            return "";
    }
}

static int bdmGetDriverName(int deviceId, char *driver, int size)
{
    char path[16];
    int dir;

    memset(driver, 0, size);
    sprintf(path, "mass%d:/", deviceId);
    if ((dir = fileXioDopen(path)) < 0)
        return 0;

    fileXioIoctl2(dir, USBMASS_IOCTL_GET_DRIVERNAME, NULL, 0, driver, size - 1);
    fileXioDclose(dir);
    return 1;
}

static int bdmFindUnitByType(int targetType)
{
    char driver[32];
    const char *want = bdmTypeToDriverStr(targetType);
    int wantLen = strlen(want);

    if (wantLen == 0)
        return -1;

    for (int i = 0; i < MAX_BDM_DEVICES; i++) {
        if (bdmGetDriverName(i, driver, sizeof(driver))) {
            if (!strcmp(driver, want) && (int)strlen(driver) == wantLen)
                return i;
        }
    }
    return -1;
}

int bdmIsReadOnlyPath(const char *path)
{
    if (!strncmp(path, "mass", 4) && path[4] >= '0' && path[4] < '0' + MAX_BDM_DEVICES && path[5] == ':') {
        int unit = path[4] - '0';
        for (int i = 0; i < BDM_SLOT_COUNT; i++) {
            bdm_device_data_t *p = (bdm_device_data_t *)bdmDriverList[i].priv;
            if (p && p->bdmUnit == unit)
                return (bdmDriverList[i].flags & MODE_FLAG_READ_ONLY) != 0;
        }
    }
    return 0;
}

static int bdmCanHoldSettings(int deviceId)
{
    char driver[32];

    if (!bdmGetDriverName(deviceId, driver, sizeof(driver)))
        return 1;

    return !strcmp(driver, "usb") || !strcmp(driver, "ata");
}

int bdmFindSettingsDevice(char *target)
{
    char driver[32];
    int i;

    for (i = 0; i < MAX_BDM_DEVICES; i++) {
        if (bdmGetDriverName(i, driver, sizeof(driver)) && bdmCanHoldSettings(i)) {
            sprintf(target, "mass%d:", i);
            return 1;
        }
    }

    return 0;
}

int bdmFindPartition(char *target, const char *name, int write)
{
    int i, fd, found;
    char path[256];

    for (i = 0; i < MAX_BDM_DEVICES; i++) {
        if (write && !bdmCanHoldSettings(i))
            continue;

        sprintf(path, "mass%d:%s", i, name);
        if ((found = (fd = open(path, O_RDONLY)) >= 0))
            close(fd);

        if (write) {
            if ((fd = open(path, found ? O_WRONLY : (O_WRONLY | O_CREAT), 0666)) >= 0) {
                close(fd);
                if (!found)
                    remove(path);
            }
            found = (fd >= 0);
        }

        if (found) {
            sprintf(target, "mass%d:", i);
            return 1;
        }
    }

    sprintf(target, "mass0:");
    return 0;
}

static unsigned int BdmGeneration = 0;

static void bdmEventHandler(void *packet, void *opt)
{
    BdmGeneration++;
}

void bdmLoadBasicModules(void)
{
    WaitSema(bdmLoadModuleLock);

    if (!basicBdmModLoaded) {
        LOG("[BDM]:\n");
        sysLoadModuleBuffer(&bdm_irx, size_bdm_irx, 0, NULL);

        LOG("[BDMFS_FATFS]:\n");
        sysLoadModuleBuffer(&bdmfs_fatfs_irx, size_bdmfs_fatfs_irx, 0, NULL);

        LOG("[BDMEVENT]:\n");
        sysLoadModuleBuffer(&bdmevent_irx, size_bdmevent_irx, 0, NULL);
        SifAddCmdHandler(0, &bdmEventHandler, NULL);

        basicBdmModLoaded = 1;
    }

    SignalSema(bdmLoadModuleLock);
}

int bdmEnsureDriverLoaded(int targetType)
{
    bdmLoadBasicModules();

    WaitSema(bdmLoadModuleLock);

    if (targetType == BDM_TYPE_USB && !usbModLoaded) {
        LOG("[USBD]:\n");
        sysLoadModuleBuffer(&usbd_irx, size_usbd_irx, 0, NULL);
        LOG("[USBMASS_BD]:\n");
        sysLoadModuleBuffer(&usbmass_bd_irx, size_usbmass_bd_irx, 0, NULL);
        usbModLoaded = 1;
    } else if (targetType == BDM_TYPE_SDC && !mx4sioModLoaded) {
        LOG("[MX4SIO_BD]:\n");
        sysLoadModuleBuffer(&mx4sio_bd_irx, size_mx4sio_bd_irx, 0, NULL);
        mx4sioModLoaded = 1;
    } else if (targetType == BDM_TYPE_ATA && !hddModLoaded) {
        LOG("bdmEnsureDriverLoaded loading hdd drivers...\n");
        hddLoadModules();
        hddModLoaded = 1;
    } else if (targetType == BDM_TYPE_ILINK && !iLinkModLoaded) {
        LOG("[ILINKMAN]:\n");
        sysLoadModuleBuffer(&iLinkman_irx, size_iLinkman_irx, 0, NULL);
        LOG("[IEEE1394_BD]:\n");
        sysLoadModuleBuffer(&IEEE1394_bd_irx, size_IEEE1394_bd_irx, 0, NULL);
        iLinkModLoaded = 1;
    } else if (targetType == BDM_TYPE_UDPBD && !udpbdModLoaded) {
        if (ethSMAPIsLoaded()) {
            LOG("UDPBD: the network adapter is used by SMB\n");
        } else {
            char ipArg[20];
            int ipArgLen = snprintf(ipArg, sizeof(ipArg), "ip=%d.%d.%d.%d", ps2_ip[0], ps2_ip[1], ps2_ip[2], ps2_ip[3]) + 1;

            // ATA resets DEV9 ATA interface when starting: if HDD is enabled, load ATA before UDPBD.
            if (gEnableHDD && !hddModLoaded) {
                hddLoadModules();
                hddModLoaded = 1;
            }

            sysInitDev9();
            LOG("[UDPBD_SMAP]:\n");
            sysLoadModuleBuffer(&udpbd_smap_irx, size_udpbd_smap_irx, 0, NULL);
            LOG("[UDPBD_MINISTACK]: %s\n", ipArg);
            sysLoadModuleBuffer(&udpbd_ministack_irx, size_udpbd_ministack_irx, ipArgLen, ipArg);
            LOG("[UDPBD]:\n");
            sysLoadModuleBuffer(&udpbd_irx, size_udpbd_irx, 0, NULL);
        }
        udpbdModLoaded = 1;
    }

    SignalSema(bdmLoadModuleLock);
    return 0;
}

int bdmUDPBDLoaded(void)
{
    return udpbdModLoaded;
}

// Used by miniInit / autoLaunchBDMGame and sysReset when SYS_LOAD_USB_MODULES is passed
void bdmLoadModules(void)
{
    bdmLoadBasicModules();
    bdmEnsureDriverLoaded(BDM_TYPE_USB);
    if (gEnableMX4SIO)
        bdmEnsureDriverLoaded(BDM_TYPE_SDC);
    if (gEnableHDD)
        bdmEnsureDriverLoaded(BDM_TYPE_ATA);
    if (gEnableUDPBD)
        bdmEnsureDriverLoaded(BDM_TYPE_UDPBD);
}

static int bdmWaitDriverUnit(int targetType, u32 timeoutMs)
{
    const u32 stepMs = 100;
    u32 elapsed = 0;

    while (1) {
        int unit = bdmFindUnitByType(targetType);
        if (unit >= 0)
            return unit;
        if (elapsed >= timeoutMs)
            return -1;
        DelayThread(stepMs * 1000);
        elapsed += stepMs;
    }
}

void bdmInit(item_list_t *itemList)
{
    LOG("BDMSUPPORT Init (mode=%d)\n", itemList->mode);

    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    pDeviceData->bdmULSizePrev = -2;
    pDeviceData->bdmModifiedCDPrev = 0;
    pDeviceData->bdmModifiedDVDPrev = 0;
    pDeviceData->bdmGameCount = 0;
    pDeviceData->bdmGames = NULL;
    pDeviceData->bdmUnit = -1;
    pDeviceData->bdmConnected = 0;
    pDeviceData->bdmProbing = 1;
    pDeviceData->ForceRefresh = 1;
    configGetInt(configGetByType(CONFIG_OPL), "usb_frames_delay", &itemList->delay);
    itemList->enabled = 1;
}

static int bdmNeedsUpdate(item_list_t *itemList)
{
    char path[256];
    int result = 0;
    struct stat st;

    bdmDeviceModeStarted = 1;

    // Only poll the currently selected active tab!
    if (itemList->mode != gActiveDeviceMode)
        return 0;

    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    if (pDeviceData == NULL)
        return 0;

    int targetType = pDeviceData->bdmDeviceType;

    // Lazy load ONLY this device's IRX driver when its tab becomes active
    bdmEnsureDriverLoaded(targetType);

    if (pDeviceData->ForceRefresh) {
        pDeviceData->ForceRefresh = 0;
        pDeviceData->bdmProbing = 1;

        if (targetType == BDM_TYPE_USB)
            bdmWaitDriverUnit(BDM_TYPE_USB, 1100);
        else if (targetType == BDM_TYPE_SDC)
            bdmWaitDriverUnit(BDM_TYPE_SDC, 500);
        else if (targetType == BDM_TYPE_ATA)
            bdmWaitDriverUnit(BDM_TYPE_ATA, 1200);

        pDeviceData->bdmProbing = 0;
        pDeviceData->bdmDeviceTick = BdmGeneration;

        result = bdmUpdateDeviceData(itemList);
        if (result == 0 && !pDeviceData->bdmConnected)
            return 0;
        return 1;
    }

    pDeviceData->bdmProbing = 0;

    // For UDPBD while waiting for server, keep polling every cycle; once connected (or for local USB/SDC/ATA), use BdmGeneration
    if (targetType != BDM_TYPE_UDPBD || pDeviceData->bdmConnected) {
        if (pDeviceData->bdmULSizePrev != -2 && pDeviceData->bdmDeviceTick == (int)BdmGeneration)
            return 0;
    }
    pDeviceData->bdmDeviceTick = BdmGeneration;

    if ((result = bdmUpdateDeviceData(itemList)) == 0)
        return 0;

    if (result == -1) {
        sfxPlay(SFX_BD_DISCONNECT);
        sbDropThemesLanguages(pDeviceData->bdmPrefix, &pDeviceData->ThemesLoaded, &pDeviceData->LanguagesLoaded);
        return result;
    } else if (result == 1) {
        sfxPlay(SFX_BD_CONNECT);
    }

    sprintf(path, "%sCD", pDeviceData->bdmPrefix);
    if (stat(path, &st) != 0)
        st.st_mtime = 0;
    if (pDeviceData->bdmModifiedCDPrev != st.st_mtime) {
        pDeviceData->bdmModifiedCDPrev = st.st_mtime;
        result = 1;
    }

    sprintf(path, "%sDVD", pDeviceData->bdmPrefix);
    if (stat(path, &st) != 0)
        st.st_mtime = 0;
    if (pDeviceData->bdmModifiedDVDPrev != st.st_mtime) {
        pDeviceData->bdmModifiedDVDPrev = st.st_mtime;
        result = 1;
    }

    if (!sbIsSameSize(pDeviceData->bdmPrefix, pDeviceData->bdmULSizePrev))
        result = 1;

    if (!pDeviceData->LanguagesLoaded) {
        sprintf(path, "%sLNG", pDeviceData->bdmPrefix);
        if (lngAddLanguages(path, "/", itemList->mode) > 0)
            pDeviceData->LanguagesLoaded = 1;
    }

    sbCreateFolders(pDeviceData->bdmPrefix, 1);

    return result;
}

static int bdmUpdateGameList(item_list_t *itemList)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    if (pDeviceData == NULL || !pDeviceData->bdmConnected) {
        if (pDeviceData && pDeviceData->bdmGames) {
            free(pDeviceData->bdmGames);
            pDeviceData->bdmGames = NULL;
        }
        if (pDeviceData)
            pDeviceData->bdmGameCount = 0;
        return 0;
    }

    sbReadList(&pDeviceData->bdmGames, pDeviceData->bdmPrefix, &pDeviceData->bdmULSizePrev, &pDeviceData->bdmGameCount);
    return pDeviceData->bdmGameCount;
}

static int bdmGetGameCount(item_list_t *itemList)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    return pDeviceData ? pDeviceData->bdmGameCount : 0;
}

static void *bdmGetGame(item_list_t *itemList, int id)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    return (void *)&pDeviceData->bdmGames[id];
}

static char *bdmGetGameName(item_list_t *itemList, int id)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    return pDeviceData->bdmGames[id].name;
}

static int bdmGetGameNameLength(item_list_t *itemList, int id)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    return ((pDeviceData->bdmGames[id].format != GAME_FORMAT_USBLD) ? ISO_GAME_NAME_MAX + 1 : UL_GAME_NAME_MAX + 1);
}

static char *bdmGetGameStartup(item_list_t *itemList, int id)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    return pDeviceData->bdmGames[id].startup;
}

static void bdmDeleteGame(item_list_t *itemList, int id)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    sbDelete(&pDeviceData->bdmGames, pDeviceData->bdmPrefix, "/", pDeviceData->bdmGameCount, id);
    pDeviceData->bdmULSizePrev = -2;
    pDeviceData->ForceRefresh = 1;
}

static void bdmRenameGame(item_list_t *itemList, int id, char *newName)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    sbRename(&pDeviceData->bdmGames, pDeviceData->bdmPrefix, "/", pDeviceData->bdmGameCount, id, newName);
    pDeviceData->bdmULSizePrev = -2;
    pDeviceData->ForceRefresh = 1;
}

void bdmLaunchGame(item_list_t *itemList, int id, config_set_t *configSet)
{
    int i, fd, iop_fd, index, compatmask = 0;
    int EnablePS2Logo = 0;
    int result;
    u64 isoTotalBytes = 0;
    u64 startingLBA;
    unsigned int startCluster;
    char partname[256], filename[32];
    base_game_info_t *game;
    struct cdvdman_settings_bdm *settings;
    u32 layer1_start, layer1_offset;
    unsigned short int layer1_part;

    bdm_device_data_t *pDeviceData = NULL;

    if (gAutoLaunchBDMGame == NULL) {
        pDeviceData = (bdm_device_data_t *)itemList->priv;
        game = &pDeviceData->bdmGames[id];
    } else {
        pDeviceData = gAutoLaunchDeviceData;
        game = gAutoLaunchBDMGame;
    }

    char vmc_name[32], vmc_path[256], have_error = 0;
    int vmc_id, vmc_frags, size_mcemu_irx = 0;
    bdm_vmc_infos_t bdm_vmc_infos;
    vmc_superblock_t vmc_superblock;

    for (vmc_id = 0; vmc_id < 2; vmc_id++) {
        memset(&bdm_vmc_infos, 0, sizeof(bdm_vmc_infos_t));
        have_error = 0;
        vmc_frags = 0;
        configGetVMC(configSet, vmc_name, sizeof(vmc_name), vmc_id);
        if (vmc_name[0] && itemList != NULL && (itemList->flags & MODE_FLAG_READ_ONLY)) {
            LOG("BDMSUPPORT VMC %s not used: the device is read-only\n", vmc_name);
            vmc_name[0] = '\0';
        }
        if (vmc_name[0]) {
            have_error = 1;
            int vmcSizeInMb = sysCheckVMC(pDeviceData->bdmPrefix, "/", vmc_name, 0, &vmc_superblock);
            if (vmcSizeInMb > 0) {
                bdm_vmc_infos.flags = vmc_superblock.mc_flag & 0xFF;
                bdm_vmc_infos.flags |= 0x100;
                bdm_vmc_infos.specs.page_size = vmc_superblock.page_size;
                bdm_vmc_infos.specs.block_size = vmc_superblock.pages_per_block;
                bdm_vmc_infos.specs.card_size = vmc_superblock.pages_per_cluster * vmc_superblock.clusters_per_card;

                sprintf(vmc_path, "%sVMC/%s.bin", pDeviceData->bdmPrefix, vmc_name);

                fd = open(vmc_path, O_RDONLY);
                if (fd >= 0) {
                    iop_fd = ps2sdk_get_iop_fd(fd);
                    if (fileXioIoctl2(iop_fd, USBMASS_IOCTL_GET_LBA, NULL, 0, &startingLBA, sizeof(startingLBA)) == 0 && (startCluster = (unsigned int)fileXioIoctl(iop_fd, USBMASS_IOCTL_GET_CLUSTER, vmc_path)) != 0) {
                        int vmcSectorCount = vmcSizeInMb * ((1024 * 1024) / 512);
                        if (startingLBA + vmcSectorCount > 0x100000000) {
                            LOG("BDMSUPPORT VMC bad LBA range\n");
                            have_error = 2;
                        } else if (fileXioIoctl(iop_fd, USBMASS_IOCTL_CHECK_CHAIN, "") == 1) {
                            LOG("BDMSUPPORT Cluster Chain OK\n");
                            have_error = 0;
                            bdm_vmc_infos.active = 1;
                            bdm_vmc_infos.start_sector = startingLBA;
                            LOG("BDMSUPPORT VMC slot %d start: 0x%08x%08x\n", vmc_id, ((u32 *)&startingLBA)[1], ((u32 *)&startingLBA)[0]);
                        } else {
                            vmc_frags = fileXioIoctl2(iop_fd, USBMASS_IOCTL_GET_FRAGLIST, NULL, 0, NULL, 0);
                            LOG("BDMSUPPORT Cluster Chain NG: %d fragments\n", vmc_frags);
                            have_error = 2;
                        }
                    }

                    close(fd);
                }
            }
        }

        if (gAutoLaunchBDMGame == NULL) {
            if (have_error) {
                char error[256];
                if (vmc_frags > 1)
                    snprintf(error, sizeof(error), _l(_STR_ERR_VMC_FRAGMENTS_CONTINUE), vmc_name, vmc_frags, (vmc_id + 1));
                else if (have_error == 2)
                    snprintf(error, sizeof(error), _l(_STR_ERR_VMC_FRAGMENTED_CONTINUE), vmc_name, (vmc_id + 1));
                else
                    snprintf(error, sizeof(error), _l(_STR_ERR_VMC_CONTINUE), vmc_name, (vmc_id + 1));
                if (!guiMsgBox(error, 1, NULL)) {
                    if (vmc_frags > 1)
                        guiMsgBox(_l(_STR_VMC_DEFRAGMENT_HINT), 0, NULL);
                    return;
                }
            }
        } else
            LOG("VMC error\n");

        static int mcemuSlots[2];
        int slotWord = sbMcemuSlotWord(&bdm_mcemu_irx, size_bdm_mcemu_irx, vmc_id, mcemuSlots);
        if (slotWord >= 0) {
            if (bdm_vmc_infos.active)
                size_mcemu_irx = size_bdm_mcemu_irx;
            memcpy(&((u32 *)&bdm_mcemu_irx)[slotWord], &bdm_vmc_infos, sizeof(bdm_vmc_infos_t));
        }
    }

    void *irx = NULL;
    int irx_size = 0;
    if (!strcmp(pDeviceData->bdmDriver, "ata") && strlen(pDeviceData->bdmDriver) == 3) {
        irx = &bdm_ata_cdvdman_irx;
        irx_size = size_bdm_ata_cdvdman_irx;
    } else if (!strcmp(pDeviceData->bdmDriver, "udp")) {
        irx = &bdm_udp_cdvdman_irx;
        irx_size = size_bdm_udp_cdvdman_irx;
    } else {
        irx = &bdm_cdvdman_irx;
        irx_size = size_bdm_cdvdman_irx;
    }

    compatmask = sbPrepare(game, configSet, irx_size, irx, &index);
    settings = (struct cdvdman_settings_bdm *)((u8 *)irx + index);
    if (settings == NULL)
        return;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
    memset(&settings->frags[0], 0, sizeof(bd_fragment_t) * BDM_MAX_FRAGS);
#pragma GCC diagnostic pop
    u8 iTotalFragCount = 0;

    struct cdvdman_fragfile *iso_frag = &settings->fragfile[0];
    iso_frag->frag_start = 0;
    iso_frag->frag_count = 0;
    for (i = 0; i < game->parts; i++) {
        sbCreatePath(game, partname, pDeviceData->bdmPrefix, "/", i);
        fd = open(partname, O_RDONLY);
        iop_fd = ps2sdk_get_iop_fd(fd);
        if (fd < 0) {
            sbUnprepare(&settings->common);
            guiMsgBox(_l(_STR_ERR_FILE_INVALID), 0, NULL);
            return;
        }

        int iFragCount = fileXioIoctl2(iop_fd, USBMASS_IOCTL_GET_FRAGLIST, NULL, 0, (void *)&settings->frags[iTotalFragCount], sizeof(bd_fragment_t) * (BDM_MAX_FRAGS - iTotalFragCount));
        if (iFragCount < 0 || iFragCount > BDM_MAX_FRAGS - iTotalFragCount) {
            close(fd);
            sbUnprepare(&settings->common);
            guiMsgBox(_l(_STR_ERR_FRAGMENTED), 0, NULL);
            return;
        }
        iso_frag->frag_count += iFragCount;
        iTotalFragCount += iFragCount;

        isoTotalBytes += lseek64(fd, 0, SEEK_END);

        if ((gPS2Logo) && (i == 0))
            EnablePS2Logo = CheckPS2Logo(fd, 0);

        close(fd);
    }

    sbCreatePath(game, partname, pDeviceData->bdmPrefix, "/", 0);
    layer1_start = sbGetISO9660MaxLBA(partname);
    settings->common.mediaLsnCount = sbGetMediaLsnCount(partname, isoTotalBytes);

    switch (game->format) {
        case GAME_FORMAT_USBLD:
            layer1_part = layer1_start / 0x80000;
            layer1_offset = layer1_start % 0x80000;
            sbCreatePath(game, partname, pDeviceData->bdmPrefix, "/", layer1_part);
            break;
        default:
            layer1_part = 0;
            layer1_offset = layer1_start;
    }

    if (sbProbeISO9660(partname, game, layer1_offset) != 0) {
        layer1_start = 0;
        LOG("DVD detected.\n");
    } else {
        layer1_start -= 16;
        LOG("DVD-DL layer 1 @ part %u sector 0x%lx.\n", layer1_part, layer1_offset);
    }
    settings->common.layer1_start = layer1_start;
    settings->common.zso_cache = bdmCacheSize;

    if ((result = sbLoadCheats(pDeviceData->bdmPrefix, game->startup, configSet)) < 0) {
        if (gAutoLaunchBDMGame == NULL) {
            switch (result) {
                case -ENOENT:
                    guiWarning(_l(_STR_NO_CHEATS_FOUND), 10);
                    break;
                default:
                    guiWarning(_l(_STR_ERR_CHEATS_LOAD_FAILED), 10);
            }
        } else
            LOG("Cheats error\n");
    }

    if (gRememberLastPlayed) {
        configSetStr(configGetByType(CONFIG_LAST), "last_played", game->startup);
        saveConfig(CONFIG_LAST, 0);
    }

    if (configGetStrCopy(configSet, CONFIG_ITEM_ALTSTARTUP, filename, sizeof(filename)) == 0)
        strcpy(filename, game->startup);

    char bdmCurrentDriver[32];
    snprintf(bdmCurrentDriver, sizeof(bdmCurrentDriver), "%s", pDeviceData->bdmDriver);
    settings->bdDeviceId = pDeviceData->massDeviceIndex;

    if (!strcmp(bdmCurrentDriver, "ata") && strlen(bdmCurrentDriver) == 3) {
        int dmaType = 0, dmaMode = 7;
        configGetInt(configSet, CONFIG_ITEM_DMA, &dmaMode);

        if (dmaMode < 3)
            dmaType = 0x20;
        else {
            dmaType = 0x40;
            if (pDeviceData->ataHighestUDMAMode > 0)
                dmaMode = pDeviceData->ataHighestUDMAMode;
            else
                dmaMode -= 3;
        }

        hddSetTransferMode(dmaType, dmaMode);
        hddSetIdleTimeout(gHDDSpindown * 12);
        settings->hddIsLBA48 = pDeviceData->bdmHddIsLBA48;
    }

    if (gAutoLaunchBDMGame == NULL)
        deinit(NO_EXCEPTION, itemList->mode);
    else {
        miniDeinit(configSet);

        free(gAutoLaunchBDMGame);
        gAutoLaunchBDMGame = NULL;

        free(gAutoLaunchDeviceData);
        gAutoLaunchDeviceData = NULL;
    }

    LOG("bdm pre sysLaunchLoaderElf\n");
    if (!strcmp(bdmCurrentDriver, "usb")) {
        settings->common.fakemodule_flags |= FAKE_MODULE_FLAG_USBD;
        sysLaunchLoaderElf(filename, "BDM_USB_MODE", irx_size, irx, size_mcemu_irx, bdm_mcemu_irx, EnablePS2Logo, compatmask);
    } else if (!strcmp(bdmCurrentDriver, "sd") && strlen(bdmCurrentDriver) == 2) {
        settings->common.fakemodule_flags |= 0;
        sysLaunchLoaderElf(filename, "BDM_ILK_MODE", irx_size, irx, size_mcemu_irx, bdm_mcemu_irx, EnablePS2Logo, compatmask);
    } else if (!strcmp(bdmCurrentDriver, "sdc") && strlen(bdmCurrentDriver) == 3) {
        settings->common.fakemodule_flags |= 0;
        sysLaunchLoaderElf(filename, "BDM_M4S_MODE", irx_size, irx, size_mcemu_irx, bdm_mcemu_irx, EnablePS2Logo, compatmask);
    } else if (!strcmp(bdmCurrentDriver, "ata") && strlen(bdmCurrentDriver) == 3) {
        settings->common.fakemodule_flags |= FAKE_MODULE_FLAG_DEV9;
        settings->common.fakemodule_flags |= FAKE_MODULE_FLAG_ATAD;
        sysLaunchLoaderElf(filename, "BDM_ATA_MODE", irx_size, irx, size_mcemu_irx, bdm_mcemu_irx, EnablePS2Logo, compatmask);
    } else if (!strcmp(bdmCurrentDriver, "udp")) {
        settings->common.fakemodule_flags |= FAKE_MODULE_FLAG_DEV9;
        settings->common.fakemodule_flags |= FAKE_MODULE_FLAG_SMAP;
        sysLaunchLoaderElf(filename, "BDM_UDP_MODE", irx_size, irx, size_mcemu_irx, bdm_mcemu_irx, EnablePS2Logo, compatmask);
    }
}

static config_set_t *bdmGetConfig(item_list_t *itemList, int id)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    char cfgDir[256], *slash;

    if (itemList->flags & MODE_FLAG_READ_ONLY) {
        snprintf(cfgDir, sizeof(cfgDir), "%s", configGetByType(CONFIG_GAME)->filename);
        if ((slash = strrchr(cfgDir, '/')) != NULL) {
            strcpy(slash + 1, "CFG/");
            return sbPopulateConfig(&pDeviceData->bdmGames[id], pDeviceData->bdmPrefix, "/", cfgDir);
        }
    }

    return sbPopulateConfig(&pDeviceData->bdmGames[id], pDeviceData->bdmPrefix, "/", NULL);
}

static int bdmGetImage(item_list_t *itemList, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm)
{
    char path[256];

    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    if (!pDeviceData || !pDeviceData->bdmConnected)
        return -1;

    if (isRelative)
        snprintf(path, sizeof(path), "%s%s/%s_%s", pDeviceData->bdmPrefix, folder, value, suffix);
    else
        snprintf(path, sizeof(path), "%s%s_%s", folder, value, suffix);
    return texDiscoverLoad(resultTex, path, -1);
}

static int bdmGetTextId(item_list_t *itemList)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    if (!pDeviceData)
        return _STR_BDM_GAMES;

    switch (pDeviceData->bdmDeviceType) {
        case BDM_TYPE_USB:
            return _STR_USB_GAMES;
        case BDM_TYPE_ILINK:
            return _STR_ILINK_GAMES;
        case BDM_TYPE_SDC:
            return _STR_MX4SIO_GAMES;
        case BDM_TYPE_ATA:
            return _STR_HDD_GAMES;
        case BDM_TYPE_UDPBD:
            return _STR_UDPBD_GAMES;
        default:
            return _STR_BDM_GAMES;
    }
}

static int bdmGetIconId(item_list_t *itemList)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    if (!pDeviceData)
        return BDM_ICON;

    switch (pDeviceData->bdmDeviceType) {
        case BDM_TYPE_USB:
            return USB_ICON;
        case BDM_TYPE_ILINK:
            return ILINK_ICON;
        case BDM_TYPE_SDC:
            return MX4SIO_ICON;
        case BDM_TYPE_ATA:
            return HDD_ICON;
        case BDM_TYPE_UDPBD:
            return UDPBD_ICON;
        default:
            return BDM_ICON;
    }
}

static void bdmCleanUp(item_list_t *itemList, int exception)
{
    if (itemList->enabled) {
        LOG("BDMSUPPORT CleanUp\n");

        bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
        if (pDeviceData != NULL) {
            free(pDeviceData->bdmGames);
            free(pDeviceData);
            itemList->priv = NULL;
        }
    }
}

static void bdmShutdown(item_list_t *itemList)
{
    char path[16];

    LOG("BDMSUPPORT Shutdown\n");

    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    if (pDeviceData != NULL && pDeviceData->bdmUnit >= 0) {
        snprintf(path, sizeof(path), "mass%d:", pDeviceData->bdmUnit);
        fileXioDevctl(path, USBMASS_DEVCTL_STOP_ALL, NULL, 0, NULL, 0);
    }

    if (itemList->enabled && pDeviceData != NULL) {
        free(pDeviceData->bdmGames);
        free(pDeviceData);
        itemList->priv = NULL;
    }
}

static int bdmCheckVMC(item_list_t *itemList, char *name, int createSize)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    if (!pDeviceData || !pDeviceData->bdmConnected)
        return -1;
    return sysCheckVMC(pDeviceData->bdmPrefix, "/", name, createSize, NULL);
}

static char *bdmGetPrefix(item_list_t *itemList)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    return pDeviceData ? pDeviceData->bdmPrefix : "";
}

static item_list_t bdmGameListTemplate = {
    BDM_MODE, 2, 0, 0, MENU_MIN_INACTIVE_FRAMES, BDM_MODE_UPDATE_DELAY, NULL, NULL, &bdmGetTextId, &bdmGetPrefix, &bdmInit, &bdmNeedsUpdate,
    &bdmUpdateGameList, &bdmGetGameCount, &bdmGetGame, &bdmGetGameName, &bdmGetGameNameLength, &bdmGetGameStartup, &bdmDeleteGame, &bdmRenameGame,
    &bdmLaunchGame, &bdmGetConfig, &bdmGetImage, &bdmCleanUp, &bdmShutdown, &bdmCheckVMC, &bdmGetIconId};

void bdmInitSemaphore()
{
    ee_sema_t semaphore;
    semaphore.init_count = 1;
    semaphore.max_count = 1;
    semaphore.option = 0;
    bdmLoadModuleLock = CreateSema(&semaphore);
}

static void bdmEnsureDriverSlotsInit(void)
{
    static const int slotTypes[BDM_SLOT_COUNT] = {BDM_TYPE_UDPBD, BDM_TYPE_USB, BDM_TYPE_SDC, BDM_TYPE_ATA};

    if (!bdmDriverListInitialized) {
        bdmDriverListInitialized = 1;
        for (int i = 0; i < BDM_SLOT_COUNT; i++) {
            memcpy(&bdmDriverList[i], &bdmGameListTemplate, sizeof(item_list_t));
            bdm_device_data_t *pData = (bdm_device_data_t *)calloc(1, sizeof(bdm_device_data_t));
            pData->bdmDeviceType = slotTypes[i];
            pData->bdmUnit = -1;
            pData->bdmULSizePrev = -2;
            bdmDriverList[i].priv = pData;
        }
    } else {
        for (int i = 0; i < BDM_SLOT_COUNT; i++) {
            if (bdmDriverList[i].priv == NULL) {
                bdm_device_data_t *pData = (bdm_device_data_t *)calloc(1, sizeof(bdm_device_data_t));
                pData->bdmDeviceType = slotTypes[i];
                pData->bdmUnit = -1;
                pData->bdmULSizePrev = -2;
                bdmDriverList[i].priv = pData;
                bdmDriverList[i].enabled = 0;
            }
        }
    }
}

item_list_t *bdmGetDriverObject(int targetType, int slotMode)
{
    bdmEnsureDriverSlotsInit();
    int slot = bdmDriverTypeToSlot(targetType);
    if (slot < 0)
        return NULL;
    bdmDriverList[slot].mode = slotMode;
    return &bdmDriverList[slot];
}

int bdmIsDriverConnected(int targetType)
{
    int slot = bdmDriverTypeToSlot(targetType);
    if (slot < 0 || bdmDriverList[slot].priv == NULL)
        return 0;
    return ((bdm_device_data_t *)bdmDriverList[slot].priv)->bdmConnected ? 1 : 0;
}

int bdmIsDriverProbing(int targetType)
{
    int slot = bdmDriverTypeToSlot(targetType);
    if (slot < 0 || bdmDriverList[slot].priv == NULL)
        return 0;
    return ((bdm_device_data_t *)bdmDriverList[slot].priv)->bdmProbing ? 1 : 0;
}

void bdmResetDriverProbe(int targetType)
{
    int slot = bdmDriverTypeToSlot(targetType);
    if (slot < 0 || bdmDriverList[slot].priv == NULL)
        return;
    bdm_device_data_t *pData = (bdm_device_data_t *)bdmDriverList[slot].priv;
    pData->bdmULSizePrev = -2;
    pData->bdmDeviceTick = -1;
    pData->bdmProbing = 1;
    pData->ForceRefresh = 1;
}

void bdmInitDevicesData(void)
{
    bdmEnsureDriverSlotsInit();
}

void bdmEnumerateDevices()
{
    bdmEnsureDriverSlotsInit();
}

void bdmResolveLBA_UDMA(bdm_device_data_t *pDeviceData)
{
    pDeviceData->bdmHddIsLBA48 = fileXioDevctl("xhdd0:", ATA_DEVCTL_IS_48BIT, NULL, 0, NULL, 0);
    if (pDeviceData->bdmHddIsLBA48 < 0)
        pDeviceData->bdmHddIsLBA48 = 0;

    pDeviceData->ataHighestUDMAMode = fileXioDevctl("xhdd0:", ATA_DEVCTL_GET_HIGHEST_UDMA_MODE, NULL, 0, NULL, 0);
    if (pDeviceData->ataHighestUDMAMode < 0)
        pDeviceData->ataHighestUDMAMode = 4;
    else if (pDeviceData->ataHighestUDMAMode > 4)
        pDeviceData->ataHighestUDMAMode = 4;

    hddSetTransferMode(0x40, pDeviceData->ataHighestUDMAMode);
}

static int bdmCheckWritable(int unit)
{
    static u32 data[128], check[128];
    char path[40];
    u32 seed = (u32)GetTimerSystemTime();
    int fd, i, result;

    for (i = 0; i < 128; i++)
        data[i] = seed + i * 0x9E3779B9;

    sprintf(path, "mass%d:/yaopl_write_test.tmp", unit);
    if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC)) < 0)
        return 0;
    result = (write(fd, data, sizeof(data)) == sizeof(data));
    close(fd);

    if (result && (fd = open(path, O_RDONLY)) >= 0) {
        result = (read(fd, check, sizeof(check)) == sizeof(check)) && (memcmp(data, check, sizeof(data)) == 0);
        close(fd);
    } else
        result = 0;

    remove(path);
    return result;
}

int bdmUpdateDeviceData(item_list_t *itemList)
{
    char path[16] = {0};

    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    if (pDeviceData == NULL)
        return 0;

    int targetType = pDeviceData->bdmDeviceType;
    int unit = bdmFindUnitByType(targetType);

    if (unit >= 0) {
        sprintf(path, "mass%d:/", unit);
        int dir = fileXioDopen(path);
        if (dir >= 0) {
            int wasConnected = pDeviceData->bdmConnected;
            int prevUnit = pDeviceData->bdmUnit;

            if (!wasConnected || prevUnit != unit || pDeviceData->bdmPrefix[0] == '\0') {
                char oldPrefix[sizeof(pDeviceData->bdmPrefix)];
                strcpy(oldPrefix, pDeviceData->bdmPrefix);

                if (gBDMPrefix[0] != '\0')
                    snprintf(pDeviceData->bdmPrefix, sizeof(pDeviceData->bdmPrefix), "mass%d:%s/", unit, gBDMPrefix);
                else
                    snprintf(pDeviceData->bdmPrefix, sizeof(pDeviceData->bdmPrefix), "mass%d:", unit);

                if (oldPrefix[0] != '\0' && strcmp(oldPrefix, pDeviceData->bdmPrefix))
                    sbDropThemesLanguages(oldPrefix, &pDeviceData->ThemesLoaded, &pDeviceData->LanguagesLoaded);

                fileXioIoctl2(dir, USBMASS_IOCTL_GET_DRIVERNAME, NULL, 0, &pDeviceData->bdmDriver, sizeof(pDeviceData->bdmDriver) - 1);
                fileXioIoctl2(dir, USBMASS_IOCTL_GET_DEVICE_NUMBER, NULL, 0, &pDeviceData->massDeviceIndex, sizeof(pDeviceData->massDeviceIndex));

                itemList->flags = (targetType == BDM_TYPE_ATA) ? MODE_FLAG_COMPAT_DMA : 0;

                if (targetType == BDM_TYPE_ATA)
                    bdmResolveLBA_UDMA(pDeviceData);

                if (targetType == BDM_TYPE_UDPBD && !bdmCheckWritable(unit))
                    itemList->flags |= MODE_FLAG_READ_ONLY;

                pDeviceData->bdmUnit = unit;
                pDeviceData->bdmConnected = 1;
                fileXioDclose(dir);
                return 1;
            }

            fileXioDclose(dir);
            return 0;
        }
    }

    if (pDeviceData->bdmConnected) {
        pDeviceData->bdmConnected = 0;
        pDeviceData->bdmUnit = -1;
        pDeviceData->bdmULSizePrev = -2;
        pDeviceData->bdmModifiedCDPrev = 0;
        pDeviceData->bdmModifiedDVDPrev = 0;
        return -1;
    }

    pDeviceData->bdmUnit = -1;
    return 0;
}

static int bdmWaitForDevice(int deviceId, u32 timeoutMs)
{
    const int RETRY_DELAY = 100;
    char path[16];

    u32 start = GetTimerSystemTime();
    sprintf(path, "mass%d:/", deviceId);

    while (1) {
        int dir = fileXioDopen(path);
        if (dir >= 0) {
            fileXioDclose(dir);
            return 1;
        }
        u32 now = GetTimerSystemTime();
        u32 elapsed_ms = (now - start) / (kBUSCLK / 1000);
        if (elapsed_ms > timeoutMs)
            return 0;
        DelayThread(RETRY_DELAY * 1000);
    }
}

static int bdmDeviceIsPresent(int deviceId)
{
    char path[16];
    sprintf(path, "mass%d:/", deviceId);
    int dir = fileXioDopen(path);
    if (dir >= 0) {
        fileXioDclose(dir);
        return 1;
    }
    return 0;
}

static int bdmDeviceIsATA(int deviceId)
{
    char driver[32];
    return bdmGetDriverName(deviceId, driver, sizeof(driver)) && !strcmp(driver, "ata");
}

static int bdmGetATADeviceId()
{
    for (int i = 0; i < MAX_BDM_DEVICES; i++) {
        if (bdmDeviceIsATA(i))
            return i;
    }
    return -1;
}

int bdmHDDIsPresent(u32 timeoutMs)
{
    int hdd_id = -1;
    int timedout = 0;

    if (!hddIsPresent())
        return 0;

    hdd_id = bdmGetATADeviceId();
    if (hdd_id >= 0)
        return 1;

    hdd_id = 0;
    for (int i = 0; i < MAX_BDM_DEVICES; i++) {
        if (!bdmDeviceIsPresent(i)) {
            hdd_id = i;
            break;
        }
    }

    if (bdmWaitForDevice(hdd_id, timeoutMs)) {
        if (bdmDeviceIsATA(hdd_id))
            return 1;
    } else
        timedout = 1;

    if (!timedout)
        DelayThread(timeoutMs * 1000);

    return bdmGetATADeviceId() >= 0;
}
