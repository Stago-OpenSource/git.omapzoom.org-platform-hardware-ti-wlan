/*
 * HwInit.c
 *
 * Copyright(c) 1998 - 2010 Texas Instruments. All rights reserved.      
 * All rights reserved.                                                  
 *                                                                       
 * Redistribution and use in source and binary forms, with or without    
 * modification, are permitted provided that the following conditions    
 * are met:                                                              
 *                                                                       
 *  * Redistributions of source code must retain the above copyright     
 *    notice, this list of conditions and the following disclaimer.      
 *  * Redistributions in binary form must reproduce the above copyright  
 *    notice, this list of conditions and the following disclaimer in    
 *    the documentation and/or other materials provided with the         
 *    distribution.                                                      
 *  * Neither the name Texas Instruments nor the names of its            
 *    contributors may be used to endorse or promote products derived    
 *    from this software without specific prior written permission.      
 *                                                                       
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS   
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT     
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR 
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT  
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT      
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT   
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */



/*******************************************************************************/
/*                                                                             */
/*  MODULE:  HwInit.c                                                          */
/*  PURPOSE: HwInit module manages the init process of the TNETW, included     */
/*           firmware download process. It shall perform Hard Reset the chip   */
/*           if possible (this will require a Reset line to be connected to    */
/*           the host); Start InterfaceCtrl; Download NVS and FW               */
/*                                                                             */
/*                                                                             */
/*******************************************************************************/

#define __FILE_ID__  FILE_ID_105
#include "tidef.h"
#include "osApi.h"
#include "report.h"
#include "timer.h"
#include "HwInit_api.h"
#include "FwEvent_api.h"
#include "TwIf.h"
#include "TWDriver.h"
#include "TWDriverInternal.h"
#include "eventMbox_api.h"
#include "CmdBld.h"
#include "CmdMBox_api.h"
#ifdef TI_RANDOM_DEFAULT_MAC
#include <linux/random.h>
#include <linux/jiffies.h>
#endif

#ifdef TNETW1283_FPGA
#define FPGA_SKIP_DRPW
#endif

#ifdef TNETW1273_FPGA
#define FPGA_SKIP_TOP_INIT
#define FPGA_SKIP_DRPW
#endif

extern void TWD_FinalizeOnFailure   (TI_HANDLE hTWD);
extern void cmdBld_FinalizeDownload (TI_HANDLE hCmdBld, TBootAttr *pBootAttr, FwStaticData_t *pFwInfo);


/************************************************************************
 * Defines
 ************************************************************************/

/* Download phase partition */
#define PARTITION_DOWN_MEM_ADDR       0                 
#define PARTITION_DOWN_MEM_SIZE       0x177C0
#define PARTITION_DOWN_REG_ADDR       REGISTERS_BASE	
#define PARTITION_DOWN_REG_SIZE       0x8800

/* Working phase partition */
#define PARTITION_WORK_MEM_ADDR1       0x40000
#define PARTITION_WORK_MEM_SIZE1       0x14FC0
#define PARTITION_WORK_MEM_ADDR2       REGISTERS_BASE    
#define PARTITION_WORK_MEM_SIZE2       0xA000   
#define PARTITION_WORK_MEM_ADDR3       0x3004F8     
#define PARTITION_WORK_MEM_SIZE3       0x4   
#define PARTITION_WORK_MEM_ADDR4       0x40404   

/* DRPW setting partition */
#define PARTITION_DRPW_MEM_ADDR       0x40000
#define PARTITION_DRPW_MEM_SIZE       0x14FC0
#define PARTITION_DRPW_REG_ADDR       DRPW_BASE         
#define PARTITION_DRPW_REG_SIZE       0x6000	        

/* Total range of bus addresses range */
#define PARTITION_TOTAL_ADDR_RANGE    0x1FFC0

/* Maximal block size in a single SDIO transfer --> Firmware image load chunk size */
#ifdef _VLCT_
#define MAX_SDIO_BLOCK					(4000)	
#else
#define MAX_SDIO_BLOCK					(0x1000)
#endif

#define ACX_EEPROMLESS_IND_REG        (SCR_PAD4)
#define USE_EEPROM                    (0)
#define SOFT_RESET_MAX_TIME           (1000000)
#define SOFT_RESET_STALL_TIME         (1000)
#define NVS_DATA_BUNDARY_ALIGNMENT    (4)

#define MAX_HW_INIT_CONSECUTIVE_TXN     15

#define WORD_SIZE                       4
#define WORD_ALIGNMENT_MASK             0x3
#define DEF_NVS_SIZE                    ((NVS_PRE_PARAMETERS_LENGTH) + (NVS_TX_TYPE_INDEX) + 4)

#define RADIO_SM_WAIT_LOOP  32

#define FREF_CLK_FREQ_MASK      0x7
#define FREF_CLK_TYPE_MASK      BIT_3
#define FREF_CLK_POLARITY_MASK  BIT_4

#define FREF_CLK_TYPE_BITS      0xfffffe7f
#define CLK_REQ_PRCM            0x100

#define FREF_CLK_POLARITY_BITS  0xfffff8ff
#define CLK_REQ_OUTN_SEL        0x700

#define DRPw_MASK_CHECK  0xc0
#define DRPw_MASK_SET    0x2000000

#define DRIVE_STRENGTH_MASK 6
/* time to wait till we check if fw is running */
#define STALL_TIMEOUT   7

#ifdef DOWNLOAD_TIMER_REQUIERD
#define FIN_LOOP 10
#endif


#ifdef _VLCT_
#define FIN_LOOP 10
#else
#define FIN_LOOP 20000
#endif



/************************************************************************
 * Macros
 ************************************************************************/

#define SET_DEF_NVS(aNVS)     aNVS[0]=0x01; aNVS[1]=0x6d; aNVS[2]=0x54; aNVS[3]=0x58; aNVS[4]=0x03; \
                              aNVS[5]=0x12; aNVS[6]=0x28; aNVS[7]=0x01; aNVS[8]=0x71; aNVS[9]=0x54; \
                              aNVS[10]=0x00; aNVS[11]=0x08; aNVS[12]=0x00; aNVS[13]=0x00; aNVS[14]=0x00; \
                              aNVS[15]=0x00; aNVS[16]=0x00; aNVS[17]=0x00; aNVS[18]=0x00; aNVS[19]=0x00; \
                              aNVS[20]=0x00; aNVS[21]=0x00; aNVS[22]=0x00; aNVS[23]=0x00; aNVS[24]=eNVS_NON_FILE;\
							  aNVS[25]=0x00; aNVS[26]=0x00; aNVS[27]=0x00;


#define SET_PARTITION(pPartition,uAddr1,uMemSize1,uAddr2,uMemSize2,uAddr3,uMemSize3,uAddr4) \
                    ((TPartition*)pPartition)[0].uMemAdrr = uAddr1; \
                    ((TPartition*)pPartition)[0].uMemSize = uMemSize1; \
                    ((TPartition*)pPartition)[1].uMemAdrr = uAddr2; \
                    ((TPartition*)pPartition)[1].uMemSize = uMemSize2; \
                    ((TPartition*)pPartition)[2].uMemAdrr = uAddr3; \
                    ((TPartition*)pPartition)[2].uMemSize = uMemSize3; \
                    ((TPartition*)pPartition)[3].uMemAdrr = uAddr4; 

#define HW_INIT_PTXN_SET(pHwInit, pTxn)  pTxn = (TTxnStruct*)&(pHwInit->aHwInitTxn[pHwInit->uTxnIndex].tTxnStruct);

#define BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, uAddr, uVal, uSize, direction, fCB, hCB)     \
                              HW_INIT_PTXN_SET(pHwInit, pTxn) \
                              TXN_PARAM_SET_DIRECTION(pTxn, direction); \
                              pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData = (TI_UINT32)uVal; \
                              BUILD_TTxnStruct(pTxn, uAddr, &(pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData), uSize, fCB, hCB)

#define BUILD_HW_INIT_FW_STATIC_TXN(pHwInit, pTxn, uAddr, fCB, hCB)     \
                              HW_INIT_PTXN_SET(pHwInit, pTxn) \
                              TXN_PARAM_SET_DIRECTION(pTxn, TXN_DIRECTION_READ); \
                              BUILD_TTxnStruct(pTxn, uAddr, &(pHwInit->tFwStaticTxn.tFwStaticInfo), sizeof(FwStaticData_t), fCB, hCB)

#define BUILD_HW_INIT_FW_DL_TXN(pHwInit, pTxn, uAddr, uVal, uSize, direction, fCB, hCB)     \
                              HW_INIT_PTXN_SET(pHwInit, pTxn) \
                              TXN_PARAM_SET_DIRECTION(pTxn, direction); \
                              BUILD_TTxnStruct(pTxn, uAddr, uVal, uSize, fCB, hCB)


#define SET_DRP_PARTITION(pPartition)\
                        SET_PARTITION(pPartition, PARTITION_DRPW_MEM_ADDR, PARTITION_DRPW_MEM_SIZE, PARTITION_DRPW_REG_ADDR, PARTITION_DRPW_REG_SIZE, 0, 0, 0)

#define SET_FW_LOAD_PARTITION(pPartition,uFwAddress)\
                            SET_PARTITION(pPartition,uFwAddress,PARTITION_DOWN_MEM_SIZE, PARTITION_DOWN_REG_ADDR, PARTITION_DOWN_REG_SIZE,0,0,0)

#define SET_WORK_PARTITION(pPartition)\
                        SET_PARTITION(pPartition,PARTITION_WORK_MEM_ADDR1, PARTITION_WORK_MEM_SIZE1, PARTITION_WORK_MEM_ADDR2, PARTITION_WORK_MEM_SIZE2, PARTITION_WORK_MEM_ADDR3, PARTITION_WORK_MEM_SIZE3, PARTITION_WORK_MEM_ADDR4)

/* Handle return status inside a state machine */
#define EXCEPT(phwinit,status)                                  \
    switch (status) {                                           \
        case TI_OK:                                             \
        case TXN_STATUS_OK:                                     \
        case TXN_STATUS_COMPLETE:                               \
             break;                                             \
        case TXN_STATUS_PENDING:                                \
             return TXN_STATUS_PENDING;                         \
        default:                                                \
            if(phwinit != NULL)                                 \
                TWD_FinalizeOnFailure (phwinit->hTWD);          \
             return TXN_STATUS_ERROR;                           \
    }


/* Handle return status inside an init sequence state machine  */
#define EXCEPT_I(phwinit,status)                                \
    switch (status) {                                           \
        case TI_OK:                                             \
        case TXN_STATUS_COMPLETE:                               \
             break;                                             \
        case TXN_STATUS_PENDING:                                \
             phwinit->uInitSeqStatus = status;                  \
             return TXN_STATUS_PENDING;                         \
        default:                                                \
             TWD_FinalizeOnFailure (phwinit->hTWD);             \
             return TXN_STATUS_ERROR;                           \
    }


/* Handle return status inside a load image state machine */
#define EXCEPT_L(phwinit,status)                                                           \
    switch (status) {                                                                      \
        case TXN_STATUS_OK:                                                                \
        case TXN_STATUS_COMPLETE:                                                          \
             break;                                                                        \
        case TXN_STATUS_PENDING:                                                           \
             phwinit->DownloadStatus = status;                                             \
             return TXN_STATUS_PENDING;                                                    \
        default:                                                                           \
             phwinit->DownloadStatus = status;                                             \
             if( NULL != phwinit->pFwTmpBuf )                                              \
             {                                                                             \
                 os_memoryFree (phwinit->hOs,                                              \
                                phwinit->pFwTmpBuf,                                        \
                                WSPI_PAD_LEN_WRITE + MAX_SDIO_BLOCK);                      \
                 phwinit->pFwTmpBuf = NULL;                                                \
             }                                                                             \
             TWD_FinalizeOnFailure (phwinit->hTWD);                                        \
             return TXN_STATUS_ERROR;                                                      \
    }


/************************************************************************
 * Types
 ************************************************************************/

enum
{
    REF_FREQ_19_2                   = 0,
    REF_FREQ_26_0                   = 1,
    REF_FREQ_38_4                   = 2,
    REF_FREQ_40_0                   = 3,
    REF_FREQ_33_6                   = 4,
    REF_FREQ_NUM                    = 5
};

enum
{
    LUT_PARAM_INTEGER_DIVIDER       = 0,
    LUT_PARAM_FRACTIONAL_DIVIDER    = 1,
    LUT_PARAM_ATTN_BB               = 2,
    LUT_PARAM_ALPHA_BB              = 3,
    LUT_PARAM_STOP_TIME_BB          = 4,
    LUT_PARAM_BB_PLL_LOOP_FILTER    = 5,
    LUT_PARAM_NUM                   = 6
};

typedef struct 
{
    TTxnStruct              tTxnStruct;
    TI_UINT32               uData; 

} THwInitTxn;

typedef struct 
{
    TTxnStruct              tTxnStruct;
    FwStaticData_t          tFwStaticInfo; 

} TFwStaticTxn;


/* The HW Init module object */
typedef struct 
{
    /* Handles */
    TI_HANDLE               hOs;
    TI_HANDLE               hReport;
    TI_HANDLE               hTWD;
    TI_HANDLE               hBusTxn;
    TI_HANDLE               hTwIf;

    TI_HANDLE 		    hFileInfo;	/* holds parameters of FW Image Portion - for DW Download */
    TEndOfHwInitCb          fInitHwCb;

    /* Firmware image ptr */
    TI_UINT8               *pFwBuf;       
    /* Firmware image length */
    TI_UINT32               uFwLength;
    TI_UINT32               uFwAddress;
    TI_UINT32               bFwBufLast;  
    TI_UINT32               uFwLastAddr;  
    /* EEPROM image ptr */
    TI_UINT8               *pEEPROMBuf;   
    /* EEPROM image length */
    TI_UINT32               uEEPROMLen;   

    TI_UINT8               *pEEPROMCurPtr;
    TI_UINT32               uEEPROMCurLen;
    TBootAttr               tBootAttr;
    TI_HANDLE               hHwCtrl;
    ETxnStatus              DownloadStatus;
    /* Upper module callback for the init stage */
    fnotify_t               fCb;          
    /* Upper module handle for the init stage */
    TI_HANDLE               hCb;          
    /* Init stage */
    TI_UINT32               uInitStage;   
    /* Reset statge */ 
    TI_UINT32               uResetStage;  
    /* EEPROM burst stage */
    TI_UINT32               uEEPROMStage; 
    /* Init state machine temporary data */
    TI_UINT32               uInitData;    
    /* ELP command image */
    TI_UINT32               uElpCmd;      
    /* Chip ID */
    TI_UINT32               uChipId;      
    /* Boot state machine temporary data */
    TI_UINT32               uBootData;    
    TI_UINT32               uSelfClearTime;
    TI_UINT8                uEEPROMBurstLen;
    TI_UINT8                uEEPROMBurstLoop;
    TI_UINT32               uEEPROMRegAddr;
    TI_STATUS               uEEPROMStatus;
    TI_UINT32               uNVSStartAddr;
    TI_UINT32               uNVSNumChar;
    TI_UINT32               uNVSNumByte;
    TI_STATUS               uNVSStatus;
    TI_UINT32               uScrPad6;
    TI_UINT32               uRefFreq; 
    TI_UINT32               uInitSeqStage;
    TI_STATUS               uInitSeqStatus;
    TI_UINT32               uLoadStage;
    TI_UINT32               uBlockReadNum;
    TI_UINT32               uBlockWriteNum;
    TI_UINT32               uPartitionLimit;
    TI_UINT32               uFinStage;
    TI_UINT32               uFinData;
    TI_UINT32               uFinLoop; 
     TI_UINT32               uRegStage;
    TI_UINT32               uRegLoop;
    TI_UINT32               uRegSeqStage;
    TI_UINT32               uRegData;
	TI_HANDLE               hStallTimer;

    /* Top register Read/Write SM temporary data*/
    TI_UINT32               uTopRegAddr;
    TI_UINT32               uTopRegValue;
    TI_UINT32               uTopRegMask;
    TI_UINT32               uTopRegUpdateValue;
    TI_UINT32               uTopStage;
    TI_STATUS               uTopStatus;


    TI_UINT8                *pFwTmpBuf;

    TFinalizeCb             fFinalizeDownload;
    TI_HANDLE               hFinalizeDownload;
    /* Size of the Fw image, retrieved from the image itself */         
    TI_UINT32               uFwDataLen; 
    TI_UINT8                aDefaultNVS[DEF_NVS_SIZE];
    TI_UINT8                uTxnIndex;
    THwInitTxn              aHwInitTxn[MAX_HW_INIT_CONSECUTIVE_TXN];
    TFwStaticTxn            tFwStaticTxn;

#ifdef TNETW1283
    TI_BOOL                 bIsFREFClock;  
    /* PLL config stage */
    TI_UINT32               uPllStage; 
    TI_UINT32               uPllPendingFlag; 
    TI_UINT32               uClockConfig; 
#endif

    TI_UINT32               uSavedDataForWspiHdr;  /* For saving the 4 bytes before the NVS data for WSPI case 
                                                        where they are overrun by the WSPI BusDrv */
    TPartition              aPartition[NUM_OF_PARTITION];
} THwInit;


/************************************************************************
 * Local Functions Prototypes
 ************************************************************************/
static void      hwInit_SetPartition                (THwInit   *pHwInit, 
                                                     TPartition *pPartition);
static TI_STATUS hwInit_BootSm                      (TI_HANDLE hHwInit);
static TI_STATUS hwInit_ResetSm                     (TI_HANDLE hHwInit);
static TI_STATUS hwInit_EepromlessStartBurstSm      (TI_HANDLE hHwInit);                                                   
static TI_STATUS hwInit_LoadFwImageSm               (TI_HANDLE hHwInit);
static TI_STATUS hwInit_FinalizeDownloadSm          (TI_HANDLE hHwInit);                                             
#ifndef FPGA_SKIP_TOP_INIT
static TI_STATUS hwInit_TopRegisterRead(TI_HANDLE hHwInit);
static TI_STATUS hwInit_InitTopRegisterRead(TI_HANDLE hHwInit, TI_UINT32 uAddress);
static TI_STATUS hwInit_TopRegisterWrite(TI_HANDLE hHwInit);
static TI_STATUS hwInit_InitTopRegisterWrite(TI_HANDLE hHwInit, TI_UINT32 uAddress, TI_UINT32 uValue);
#endif
#ifdef TNETW1283
static TI_STATUS hwInit_PllConfigSm (TI_HANDLE hHwInit);
#endif
#ifdef DOWNLOAD_TIMER_REQUIERD
static void      hwInit_StallTimerCb                (TI_HANDLE hHwInit, TI_BOOL bTwdInitOccured);
#endif

/*******************************************************************************
*                       PUBLIC  FUNCTIONS  IMPLEMENTATION                      *
********************************************************************************/


/*************************************************************************
*                        hwInit_Create                                   *
**************************************************************************
* DESCRIPTION:  This function initializes the HwInit module.
*
* INPUT:        hOs - handle to Os Abstraction Layer
*               
* RETURN:       Handle to the allocated HwInit module
*************************************************************************/
TI_HANDLE hwInit_Create (TI_HANDLE hOs)
{
    THwInit *pHwInit;
  
    /* Allocate HwInit module */
    pHwInit = os_memoryAlloc (hOs, sizeof(THwInit));

    if (pHwInit == NULL)
    {
        WLAN_OS_REPORT(("Error allocating the HwInit Module\n"));
        return NULL;
    }

    /* Reset HwInit module */
    os_memoryZero (hOs, pHwInit, sizeof(THwInit));

    pHwInit->hOs = hOs;

    return (TI_HANDLE)pHwInit;
}


/***************************************************************************
*                           hwInit_Destroy                                 *
****************************************************************************
* DESCRIPTION:  This function unload the HwInit module. 
*
* INPUTS:       hHwInit - the object
*
* OUTPUT:
*
* RETURNS:      TI_OK - Unload succesfull
*               TI_NOK - Unload unsuccesfull
***************************************************************************/
TI_STATUS hwInit_Destroy (TI_HANDLE hHwInit)
{
    THwInit *pHwInit = (THwInit *)hHwInit;

        if (pHwInit->hStallTimer)
        {
#ifdef DOWNLOAD_TIMER_REQUIERD
		tmr_DestroyTimer (pHwInit->hStallTimer);
#endif

        }        

    /* Free HwInit Module */
    os_memoryFree (pHwInit->hOs, pHwInit, sizeof(THwInit));

    return TI_OK;
}


/***************************************************************************
*                           hwInit_Init                                    *
****************************************************************************
* DESCRIPTION:  This function configures the hwInit module
*
* RETURNS:      TI_OK - Configuration successful
*               TI_NOK - Configuration unsuccessful
***************************************************************************/
TI_STATUS hwInit_Init (TI_HANDLE      hHwInit,
                         TI_HANDLE      hReport,
                       TI_HANDLE      hTimer,
                         TI_HANDLE      hTWD,
                         TI_HANDLE 	hFinalizeDownload, 
			 TFinalizeCb    fFinalizeDownload, 
                         TEndOfHwInitCb fInitHwCb)
{
    THwInit   *pHwInit = (THwInit *)hHwInit;
    TTxnStruct* pTxn;
#ifdef TI_RANDOM_DEFAULT_MAC
    u32 rand_mac;
#endif

    /* Configure modules handles */
    pHwInit->hReport    = hReport;
    pHwInit->hTWD       = hTWD;
    pHwInit->hTwIf      = ((TTwd *)hTWD)->hTwIf;
    pHwInit->hOs        = ((TTwd *)hTWD)->hOs;
    pHwInit->fInitHwCb  = fInitHwCb;
    pHwInit->pFwTmpBuf  = NULL;
    pHwInit->fFinalizeDownload 	= fFinalizeDownload;
    pHwInit->hFinalizeDownload 	= hFinalizeDownload;

    SET_DEF_NVS(pHwInit->aDefaultNVS)
#ifdef TI_RANDOM_DEFAULT_MAC
    /* Create random MAC address: offset 3, 4 and 5 */
    srandom32((u32)jiffies);
    rand_mac = random32();
    pHwInit->aDefaultNVS[3] = (u8)rand_mac;
    pHwInit->aDefaultNVS[4] = (u8)(rand_mac >> 8);
    pHwInit->aDefaultNVS[5] = (u8)(rand_mac >> 16);
#endif

    for (pHwInit->uTxnIndex=0;pHwInit->uTxnIndex<MAX_HW_INIT_CONSECUTIVE_TXN;pHwInit->uTxnIndex++)
    {
        HW_INIT_PTXN_SET(pHwInit, pTxn)
        /* Setting write as default transaction */
        TXN_PARAM_SET(pTxn, TXN_LOW_PRIORITY, TXN_FUNC_ID_WLAN, TXN_DIRECTION_WRITE, TXN_INC_ADDR)
    }

#ifdef DOWNLOAD_TIMER_REQUIERD
	pHwInit->hStallTimer = tmr_CreateTimer (hTimer);
	if (pHwInit->hStallTimer == NULL) 
	{
		return TI_NOK;
	}
#endif

    TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT, ".....HwInit configured successfully\n");
    
    return TI_OK;
}


TI_STATUS hwInit_SetNvsImage (TI_HANDLE hHwInit, TI_UINT8 *pbuf, TI_UINT32 length)
{
    THwInit   *pHwInit = (THwInit *)hHwInit;

    pHwInit->pEEPROMBuf = pbuf;
    pHwInit->uEEPROMLen = length; 

    return TI_OK;
}


TI_STATUS hwInit_SetFwImage (TI_HANDLE hHwInit, TFileInfo *pFileInfo)
{
    THwInit   *pHwInit = (THwInit *)hHwInit;

    if ((hHwInit == NULL) || (pFileInfo == NULL))
    {
	return TI_NOK;
    }

    pHwInit->pFwBuf 	= pFileInfo->pBuffer;
    pHwInit->uFwLength  = pFileInfo->uLength;
    pHwInit->uFwAddress = pFileInfo->uAddress;
    pHwInit->bFwBufLast = pFileInfo->bLast;

    return TI_OK;
}


/** 
 * \fn     hwInit_SetPartition
 * \brief  Set HW addresses partition
 * 
 * Set the HW address ranges for download or working memory and registers access.
 * Generate and configure the bus access address mapping table.
 * The partition is split between register (fixed partition of 24KB size, exists in all modes), 
 *     and memory (dynamically changed during init and gets constant value in run-time, 104KB size).
 * The TwIf configures the memory mapping table on the device by issuing write transaction to 
 *     table address (note that the TxnQ and bus driver see this as a regular transaction). 
 * 
 * \note In future versions, a specific bus may not support partitioning (as in wUART), 
 *       In this case the HwInit module shall not call this function (will learn the bus 
 *       configuration from the INI file).
 *
 * \param  pHwInit   - The module's object
 * \param  pPartition  - all partition base address
 * \return void
 * \sa     
 */ 
static void hwInit_SetPartition (THwInit   *pHwInit, 
                                 TPartition *pPartition)
{
   TRACE7(pHwInit->hReport, REPORT_SEVERITY_INFORMATION, "hwInit_SetPartition: uMemAddr1=0x%x, MemSize1=0x%x uMemAddr2=0x%x, MemSize2=0x%x, uMemAddr3=0x%x, MemSize3=0x%x, uMemAddr4=0x%x, MemSize4=0x%x\n",pPartition[0].uMemAdrr, pPartition[0].uMemSize,pPartition[1].uMemAdrr, pPartition[1].uMemSize,pPartition[2].uMemAdrr, pPartition[2].uMemSize,pPartition[3].uMemAdrr );

    /* Prepare partition Txn data and send to HW */
    twIf_SetPartition (pHwInit->hTwIf,pPartition);
}


/****************************************************************************
 *                      hwInit_Boot()
 ****************************************************************************
 * DESCRIPTION: Start HW init sequence which writes and reads some HW registers
 *                  that are needed prior to FW download.
 * 
 * INPUTS:  None    
 * 
 * OUTPUT:  None
 * 
 * RETURNS: TI_OK or TI_NOK
 ****************************************************************************/
TI_STATUS hwInit_Boot (TI_HANDLE hHwInit)
{ 
    THwInit      *pHwInit = (THwInit *)hHwInit;
    TTwd         *pTWD = (TTwd *)pHwInit->hTWD;
    TWlanParams  *pWlanParams = &DB_WLAN(pTWD->hCmdBld);
    TBootAttr     tBootAttr;

    tBootAttr.MacClock      = pWlanParams->MacClock;
    tBootAttr.ArmClock      = pWlanParams->ArmClock;
    tBootAttr.FirmwareDebug = TI_FALSE;

    /*
     * Initialize the status of download to  pending 
     * It will be set to TXN_STATUS_COMPLETE at the FinalizeDownload function 
     */
    pHwInit->DownloadStatus = TXN_STATUS_PENDING;

    /* Call the boot sequence state machine */
    pHwInit->uInitStage = 0;

    os_memoryCopy (pHwInit->hOs, &pHwInit->tBootAttr, &tBootAttr, sizeof(TBootAttr));

    hwInit_BootSm (hHwInit);

    /*
     * If it returns the status of the StartInstance only then we can here query for the download status 
     * and then return the status up to the TNETW_Driver.
     * This return value will go back up to the TNETW Driver layer so that the init from OS will know
     * if to wait for the InitComplte or not in case of TXN_STATUS_ERROR.
     * This value will always be pending since the SPI is ASYNC 
     * and in SDIOa timer is set so it will be ASync also in anyway.
     */
    return pHwInit->DownloadStatus;
}


 /****************************************************************************
 * DESCRIPTION: Firmware boot state machine
 * 
 * INPUTS:  
 * 
 * OUTPUT:  None
 * 
 * RETURNS: TI_OK 
 ****************************************************************************/
static TI_STATUS hwInit_BootSm (TI_HANDLE hHwInit)
{
    THwInit    *pHwInit = (THwInit *)hHwInit;
    TI_STATUS   status = 0;
    TTxnStruct  *pTxn;
    TTwd                    *pTWD        = (TTwd *) pHwInit->hTWD;
    IniFileGeneralParam     *pGenParams = &DB_GEN(pTWD->hCmdBld);
#ifdef TNETW1283
    TWlanParams             *pWlanParams = &DB_WLAN(pTWD->hCmdBld);
#endif
    TI_UINT32   clkVal = 0x3;
#ifndef FPGA_SKIP_TOP_INIT
    TI_UINT32               uData;
#endif

    switch (pHwInit->uInitStage)
    {
    case 0:
        pHwInit->uInitStage++;
        pHwInit->uTxnIndex = 0;

        /* Set the bus addresses partition to its "running" mode */
        SET_WORK_PARTITION(pHwInit->aPartition)
        hwInit_SetPartition (pHwInit,pHwInit->aPartition);

        /* Read Chip ID */
        BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn,  CHIP_ID, 0,
                               REGISTER_SIZE, TXN_DIRECTION_READ,(TTxnDoneCb)hwInit_BootSm, hHwInit)
        status = twIf_Transact(pHwInit->hTwIf, pTxn);

        EXCEPT (pHwInit, status)

    case 1:

        pHwInit->uInitStage ++;
        /* We don't zero pHwInit->uTxnIndex at the begining because we need it's value to the next transaction */
        pHwInit->uChipId = pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData;
        /* Now we can zero the index */
        pHwInit->uTxnIndex = 0;

		if (pHwInit->uChipId == CHIP_ID_1273_PG10)
        {
            WLAN_OS_REPORT(("Error!!  1273 PG 1.0 is not supported anymore!!.\n"));
        }
		else if (pHwInit->uChipId == CHIP_ID_1273_PG20)
        {
            WLAN_OS_REPORT(("Working on a 1273 PG 2.0 board.\n"));
        }
		else if (pHwInit->uChipId == CHIP_ID_1283_PG10)
        {
            WLAN_OS_REPORT(("Working on a 1283 PG 1.0 board.\n"));
        }
        else if (pHwInit->uChipId == CHIP_ID_1283_PG20)
        {
            WLAN_OS_REPORT(("Working on a 1283 PG 2.0 board.\n"));
        }
        else
        {
            WLAN_OS_REPORT (("Error!! Found unknown Chip Id = 0x%, HW Init Failed. \n", pHwInit->uChipId));
            status = TI_NOK;
            EXCEPT (pHwInit, status)
        }

#ifdef _VLCT_
         /* Set FW to test mode */    
         BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, SCR_PAD8, 0xBABABABE, 
                                REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
         twIf_Transact(pHwInit->hTwIf, pTxn);
         pHwInit->uTxnIndex++;
#endif

#ifdef TNETW1283
        TRACE1(pHwInit->hReport, REPORT_SEVERITY_INIT ,"hwInit_BootSm: NewPllAlgo = %d\n", pWlanParams->NewPllAlgo);
        if (pWlanParams->NewPllAlgo)
        {
             /*
              * New PLL configuration algorithm - see hwInit_PllConfigSm()
              * Skip TRIO setting of register PLL_PARAMETERS(6040) and WU_COUNTER_PAUSE(6008)
              * Continue from WELP_ARM_COMMAND(6100) setting - Continue the ELP wake up sequence
              */
            TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT ,"NEW PLL ALGO - hwInit_BootSm: Call hwInit_PllConfigSm\n");
             /* Call PLL configuration state machine */
            pHwInit->uPllStage = 0;
            pHwInit->uPllPendingFlag = 0;
            pHwInit->uClockConfig = 0;
            status = hwInit_PllConfigSm(pHwInit);
        
            TRACE1(pHwInit->hReport, REPORT_SEVERITY_INIT ,"hwInit_BootSm: hwInit_PllConfigSm return status = %d\n", status);

            EXCEPT (pHwInit, status)
        }
        else
#endif
        {
            if (( 0 == (pGenParams->RefClk & FREF_CLK_FREQ_MASK)) || (2 == (pGenParams->RefClk & FREF_CLK_FREQ_MASK))
                 || (4 == (pGenParams->RefClk & FREF_CLK_FREQ_MASK)))
            {/* ref clk: 19.2/38.4/38.4-XTAL */
                clkVal = 0x3;
            }
            if ((1 == (pGenParams->RefClk & FREF_CLK_FREQ_MASK)) || (3 == (pGenParams->RefClk & FREF_CLK_FREQ_MASK))
                 || (5 == (pGenParams->RefClk & FREF_CLK_FREQ_MASK)))
            {/* ref clk: 26/52/26-XTAL  */
                clkVal = 0x5;
            }

	WLAN_OS_REPORT(("CHIP VERSION... set 1273 chip top registers\n"));
#ifdef FPGA_SKIP_TOP_INIT
            WLAN_OS_REPORT(("hwInit_BootSm: SKIP TOP INIT\n"));
            /* 
             * Don't Access PLL registers 
             * Replace with SCR_PAD2 just not to break the boot states
             */
            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, SCR_PAD2, 0, 
                                   REGISTER_SIZE, TXN_DIRECTION_READ,(TTxnDoneCb)hwInit_BootSm, hHwInit)
            status = twIf_Transact(pHwInit->hTwIf, pTxn);
            pHwInit->uTxnIndex++;
#else
            /* set the reference clock freq' to be used (pll_selinpfref field) */
            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, PLL_PARAMETERS, clkVal,
                                   REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
            twIf_Transact(pHwInit->hTwIf, pTxn);

            pHwInit->uTxnIndex++;

            /* read the PAUSE value to highest threshold */
            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, PLL_PARAMETERS, 0,
                                   REGISTER_SIZE, TXN_DIRECTION_READ, (TTxnDoneCb)hwInit_BootSm, hHwInit)
            status = twIf_Transact(pHwInit->hTwIf, pTxn);
#endif
            EXCEPT (pHwInit, status)
        }

    case 2:
	pHwInit->uInitStage ++;

#ifdef FPGA_SKIP_TOP_INIT
        WLAN_OS_REPORT(("hwInit_BootSm 1: SKIP TOP INIT - don't set PLL registers\n"));
#else

#ifdef TNETW1283
        if (pWlanParams->NewPllAlgo)
        {
            TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT ,"NEW PLL ALGO - hwInit_BootSm: stage 1 entry\n");
        }
        else
#endif
        {
            /* We don't zero pHwInit->uTxnIndex at the begining because we need it's value to the next transaction */
            uData = pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData;
            uData &= ~(0x3ff);

            /* Now we can zero the index */
            pHwInit->uTxnIndex = 0;

            /* set the the PAUSE value to highest threshold */
            uData |= WU_COUNTER_PAUSE_VAL;
            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, WU_COUNTER_PAUSE, uData,
                                   REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
            twIf_Transact(pHwInit->hTwIf, pTxn);

            pHwInit->uTxnIndex++;
        }

        /* Continue the ELP wake up sequence */
        BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, WELP_ARM_COMMAND, WELP_ARM_COMMAND_VAL, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
        twIf_Transact(pHwInit->hTwIf, pTxn);

#endif /* else of FPGA_SKIP_TOP_INIT */

        /* Wait 500uS */
        os_StalluSec (pHwInit->hOs, 500);

#ifdef FPGA_SKIP_DRPW
        /* 
         * Don't Access DRPW registers 
         * Replace with SCR_PAD2 just not to break the boot states
         */
        WLAN_OS_REPORT(("hwInit_BootSm 1: skip set_partition and skip read DRPW reg 0x%x\n", DRPW_SCRATCH_START));
        BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, SCR_PAD2, 0, 
                               REGISTER_SIZE, TXN_DIRECTION_READ,(TTxnDoneCb)hwInit_BootSm, hHwInit)
        status = twIf_Transact(pHwInit->hTwIf, pTxn);
        pHwInit->uTxnIndex++;
        /* Skip to next phase */
#else
        /* Set the bus addresses partition to DRPw registers region */
        SET_DRP_PARTITION(pHwInit->aPartition)        
        hwInit_SetPartition (pHwInit,pHwInit->aPartition);

        pHwInit->uTxnIndex++;

        /* Read-modify-write DRPW_SCRATCH_START register (see next state) to be used by DRPw FW. 
           The RTRIM value will be added  by the FW before taking DRPw out of reset */
        BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, DRPW_SCRATCH_START, 0, 
                               REGISTER_SIZE, TXN_DIRECTION_READ,(TTxnDoneCb)hwInit_BootSm, hHwInit)
        status = twIf_Transact(pHwInit->hTwIf, pTxn);
#endif
        EXCEPT (pHwInit, status)

    case 3:
        pHwInit->uInitStage ++;

#ifdef FPGA_SKIP_DRPW
        WLAN_OS_REPORT(("hwInit_BootSm case 3: skip set_partition and skip write DRPW reg 0x%x\n", DRPW_SCRATCH_START));
#else


        /* multiply fref value by 2, so that {0,1,2,3} values will become {0,2,4,6} */
        /* Then, move it 4 places to the right, to alter Fref relevant bits in register 0x2c */
        clkVal = pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData;
        pHwInit->uTxnIndex = 0; /* Reset index only after getting the last read value! */
        
#ifdef TNETW1283 
        TRACE1(pHwInit->hReport, REPORT_SEVERITY_INIT ,"\n **** in BootSM, setting clkVal,  pHwInit->bIsFREFClock=%d  *****\n", pHwInit->bIsFREFClock);
        if (pHwInit->bIsFREFClock == TI_TRUE)
        {
            clkVal |= ((pGenParams->RefClk & 0x3) << 1) << 4;
        }
        else
        {
            clkVal |= ((pWlanParams->TcxoRefClk & 0x3) << 1) << 4;
        }
        
#else
        clkVal |= ((pGenParams->RefClk & 0x3) << 1) << 4;
#endif

        if ((pGenParams->GeneralSettings[0] & DRPw_MASK_CHECK) > 0)          
        {
            clkVal |= DRPw_MASK_SET;
        }
        BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, DRPW_SCRATCH_START, clkVal, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
        twIf_Transact(pHwInit->hTwIf, pTxn);
#endif
        pHwInit->uTxnIndex++;


        /* Set the bus addresses partition back to its "running" mode */
        SET_WORK_PARTITION(pHwInit->aPartition)
        hwInit_SetPartition (pHwInit,pHwInit->aPartition);

        /* 
         * end of CHIP init seq.
         */

        /* Disable interrupts */
        BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, ACX_REG_INTERRUPT_MASK, ACX_INTR_ALL, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
        twIf_Transact(pHwInit->hTwIf, pTxn);

        pHwInit->uTxnIndex++;
    
        /*
         * Soft reset 
         */
        pHwInit->uResetStage = 0;
        pHwInit->uSelfClearTime = 0;
        pHwInit->uBootData = 0;
        status = hwInit_ResetSm (pHwInit);    

        EXCEPT (pHwInit, status)

    case 4:
        pHwInit->uInitStage ++;

        TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT , "TNET SOFT-RESET\n");

        WLAN_OS_REPORT(("Starting to process NVS...\n"));

        /*
         * Start EEPROM/NVS burst
         */

        if (pHwInit->pEEPROMBuf) 
        {
            /* NVS file exists (EEPROM-less support) */
            pHwInit->uEEPROMCurLen = pHwInit->uEEPROMLen;

            TRACE2(pHwInit->hReport, REPORT_SEVERITY_INIT , "EEPROM Image addr=0x%x, EEPROM Len=0x0x%x\n", pHwInit->pEEPROMBuf, pHwInit->uEEPROMLen);
            WLAN_OS_REPORT (("NVS found, EEPROM Image addr=0x%x, EEPROM Len=0x0x%x\n", 
            pHwInit->pEEPROMBuf, pHwInit->uEEPROMLen));
        }
        else
        {
            WLAN_OS_REPORT (("No Nvs, Setting default MAC address\n"));
            pHwInit->uEEPROMCurLen = DEF_NVS_SIZE;
            pHwInit->pEEPROMBuf = (TI_UINT8*)(&pHwInit->aDefaultNVS[0]);
            WLAN_OS_REPORT (("pHwInit->uEEPROMCurLen: %x\n", pHwInit->uEEPROMCurLen));
            WLAN_OS_REPORT (("ERROR: If you are not calibating the device, you will soon get errors !!!\n"));

        }

        pHwInit->pEEPROMCurPtr = pHwInit->pEEPROMBuf;
        pHwInit->uEEPROMStage = 0;
        status = hwInit_EepromlessStartBurstSm (hHwInit);

        EXCEPT (pHwInit, status)
        
    case 5: 
        pHwInit->uInitStage ++;
        pHwInit->uTxnIndex = 0;

        if (pHwInit->pEEPROMBuf) 
        {
            /* Signal FW that we are eeprom less */
            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, ACX_EEPROMLESS_IND_REG, ACX_EEPROMLESS_IND_REG, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
            twIf_Transact(pHwInit->hTwIf, pTxn);

            TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT , "DRIVER NVS BURST-READ\n");
        }
        else
        {
	    /* 1273 - EEPROM is not support by FPGA yet */ 
            /*
             * Start ACX EEPROM
             */     
            /*pHwInit->uRegister = START_EEPROM_MGR;
            TXN_PARAM_SET(pTxn, TXN_LOW_PRIORITY, TXN_FUNC_ID_WLAN, TXN_DIRECTION_WRITE, TXN_INC_ADDR)
            BUILD_TTxnStruct(pTxn, ACX_REG_EE_START, &pHwInit->uRegister, REGISTER_SIZE, 0, NULL, NULL)
            twIf_Transact(pHwInit->hTwIf, pTxn);*/

            /*
             * The stall is needed so the EEPROM NVS burst read will complete
             */     
            os_StalluSec (pHwInit->hOs, 40000);

            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, ACX_EEPROMLESS_IND_REG, USE_EEPROM, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
            twIf_Transact(pHwInit->hTwIf, pTxn);

            TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT , "STARTING EEPROM NVS BURST-READ\n");
        }

        pHwInit->uTxnIndex++;

        /* Read Scr2 to verify that the HW is ready */
        BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, SCR_PAD2, 0, 
                               REGISTER_SIZE, TXN_DIRECTION_READ,(TTxnDoneCb)hwInit_BootSm, hHwInit)
        status = twIf_Transact(pHwInit->hTwIf, pTxn);
        EXCEPT (pHwInit, status)

    case 6:
        pHwInit->uInitStage ++;
        /* We don't zero pHwInit->uTxnIndex at the begining because we need it's value to the next transaction */
        pHwInit->uBootData = pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData;

        if (pHwInit->uBootData == 0xffffffff)
        {
            TRACE0(pHwInit->hReport, REPORT_SEVERITY_FATAL_ERROR , "Error in SCR_PAD2 register\n");
            EXCEPT (pHwInit, TXN_STATUS_ERROR)
        }

        /* Call the restart sequence */
        pHwInit->uInitSeqStage = 0;
        pHwInit->uInitSeqStatus = TXN_STATUS_COMPLETE;

#ifdef FPGA_SKIP_TOP_INIT
        WLAN_OS_REPORT(("hwInit_BootSm: SKIP TOP INIT, skip to last stage 13\n"));
        pHwInit->uInitStage = 13;
#else
        EXCEPT (pHwInit, status)

    case 7:
#ifdef FPGA_SKIP_TOP_INIT
        WLAN_OS_REPORT(("hwInit_BootSm: SKIP TOP INIT, ERROR in stage 8\n"));
#endif
        pHwInit->uInitStage++;
        if ((pGenParams->RefClk & FREF_CLK_TYPE_MASK) != 0x0)
        {            
            status = hwInit_InitTopRegisterRead(hHwInit, CLK_REQ);
            EXCEPT (pHwInit, status)
        }

    case 8:
        pHwInit->uInitStage++;

        if ((pGenParams->RefClk & FREF_CLK_TYPE_MASK) != 0x0)
        { 
            pHwInit->uTopRegValue &= FREF_CLK_TYPE_BITS;
            pHwInit->uTopRegValue |= CLK_REQ_PRCM;
			status =  hwInit_InitTopRegisterWrite( hHwInit, CLK_REQ, pHwInit->uTopRegValue);
            EXCEPT (pHwInit, status)
        }

    case 9:
        pHwInit->uInitStage++;

#ifdef TNETW1283

        /*
        Configure SDIO/wSPI DS according to the following table:
        00   8mA.
        01   4mA (default).
        10   6mA.
        11   2mA.
        Write bits [1:0] of Register 0xd14
        data is in pWlanParams->PlatformConfiguration bits [2:1]
        */
        status = hwInit_InitTopRegisterWrite(hHwInit,SDIO_IO_DS,(pWlanParams->PlatformConfiguration & DRIVE_STRENGTH_MASK)>>1);

        EXCEPT (pHwInit, status)
#else
        if ((pGenParams->RefClk & FREF_CLK_POLARITY_MASK) == 0x0)
        {            
            status = hwInit_InitTopRegisterRead(hHwInit, TESTMODE_CLK_REQ_OUTN_SEL);
            EXCEPT (pHwInit, status)
        }
#endif

    case 10:
        pHwInit->uInitStage++;
#ifndef TNETW1283     
        if ((pGenParams->RefClk & FREF_CLK_POLARITY_MASK) == 0x0)
        {    
            pHwInit->uTopRegValue &= FREF_CLK_POLARITY_BITS;
            pHwInit->uTopRegValue |= CLK_REQ_OUTN_SEL;            
            status =  hwInit_InitTopRegisterWrite( hHwInit, TESTMODE_CLK_REQ_OUTN_SEL, pHwInit->uTopRegValue);
            EXCEPT (pHwInit, status)
        }
#endif

    case 11:
	pHwInit->uInitStage++;
#ifndef TNETW1283
	status = hwInit_InitTopRegisterRead(hHwInit, FUSE_DATA_2_1);
	EXCEPT (pHwInit, status)
#endif

    case 12: /* Store the PG Version (from FUSE_DATA_2_1) */
	pHwInit->uInitStage++;
#ifndef TNETW1283
	status = cmdBld_SetPGVersion( pTWD->hCmdBld, (pHwInit->uTopRegValue & PG_VERSION_MASK) >> PG_VERSION_OFFSET );
	EXCEPT (pHwInit, status)
#endif

    case 13:
#endif /*FPGA_SKIP_TOP_INIT*/
#ifdef FPGA_SKIP_TOP_INIT
        WLAN_OS_REPORT(("hwInit_BootSm: SKIP TOP INIT, now in stage 13\n"));
#endif
        pHwInit->uInitStage = 0;
        
        /* Set the Download Status to COMPLETE */
        pHwInit->DownloadStatus = TXN_STATUS_COMPLETE;
        /* Call upper layer callback */
        if (pHwInit->fInitHwCb)
        {
            (*pHwInit->fInitHwCb) (pHwInit->hTWD);
        }

        return TI_OK;
    }

    return TI_OK;
}

#ifdef TNETW1283
/* 
 * New PLL Configuration Algorithm
 * -------------------------------
 */
#define CLOCK_CONFIG_19_2_M      0
#define CLOCK_CONFIG_26_M        1
#define CLOCK_CONFIG_38_4_M      2
#define CLOCK_CONFIG_52_M        3
#define CLOCK_CONFIG_38_4_M_XTAL 4
#define CLOCK_CONFIG_16_368_M    4
#define CLOCK_CONFIG_26_M_XTAL   5
#define CLOCK_CONFIG_32_736_M    5
#define CLOCK_CONFIG_16_8_M      6
#define CLOCK_CONFIG_33_6_M      7



 /****************************************************************************
 * DESCRIPTION: PLL configuration state machine
 * 
 * INPUTS:  
 * 
 * OUTPUT:  None
 * 
 * RETURNS: TI_OK 
 ****************************************************************************/
static TI_STATUS hwInit_PllConfigSm (TI_HANDLE hHwInit)
{
    THwInit                 *pHwInit = (THwInit *)hHwInit;
    TI_STATUS               status = TI_OK;
    TI_UINT32               uData;
    TI_UINT32               uMcsPllConfig;
    TTwd                    *pTWD        = (TTwd *) pHwInit->hTWD;
    IniFileGeneralParam     *pGenParams = &DB_GEN(pTWD->hCmdBld);
    TWlanParams             *pWlanParams = &DB_WLAN(pTWD->hCmdBld);
   pHwInit->uTxnIndex =    0;
    

    pHwInit->bIsFREFClock = TI_FALSE;
    
    TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"NEW PLL ALGO - hwInit_PllConfigSm: entry, uPllStage=%d\n", pHwInit->uPllStage);
    /*
     * Select FREF or TCXO clock 
     */
    while (TI_TRUE)
    {
        TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"NEW PLL ALGO - hwInit_PllConfigSm( uPllStage = %d ): while\n", pHwInit->uPllStage);
        switch (pHwInit->uPllStage)
        {

        /*
         * 1. TCXO Frequency Detection
         * ---------------------------
         */
        case 0:
            pHwInit->uPllStage++;
            pHwInit->uTxnIndex = 0;

            os_StalluSec (pHwInit->hOs, 60000);

            
            
            TRACE2(pHwInit->hReport,REPORT_SEVERITY_INIT,"\n TCXO CLOCK=%d, FREF CLOCK=%d !!!!!! \n", pWlanParams->TcxoRefClk, pGenParams->RefClk);
            TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"\n CHIP ID Found --->>>  0x%x !!!!!! \n", pHwInit->uChipId);
            TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"pWlanParams->PlatformConfiguration = 0x%x  <------\n",pWlanParams->PlatformConfiguration);


			/*
			 *	if working on XTAL-only mode go directly to TCXO TO FREF SWITCH
			 */
            if(pGenParams->RefClk == CLOCK_CONFIG_38_4_M_XTAL || pGenParams->RefClk == CLOCK_CONFIG_26_M_XTAL)
			{
				pHwInit->uPllStage = 2; /* TCXO TO FREF SWITCH */
				continue;
			}
            
            
            
            TRACE0(pHwInit->hReport,REPORT_SEVERITY_INIT,"NEW PLL ALGO - hwInit_PllConfigSm(1): read SYS_CLK_CFG_REG\n");

            /* 
             * Read clock source FREF or TCXO 
             */
            status = hwInit_InitTopRegisterRead(hHwInit, SYS_CLK_CFG_REG);            
            if (status == TXN_STATUS_PENDING) pHwInit->uPllPendingFlag = 1;

            EXCEPT (pHwInit, status)

        case 1:
            pHwInit->uPllStage++;
            pHwInit->uClockConfig = pHwInit->uTopRegValue;
            pHwInit->uTxnIndex = 0;
            
            /* 
             * Check the clock source in bit 4 from SYS_CLK_CFG_REG 
             */
            if (pHwInit->uClockConfig & PRCM_CM_EN_MUX_WLAN_FREF)
            {
                pHwInit->bIsFREFClock = TI_TRUE;
                /* 
                 * if bit 4 is set - working with FREF clock, skip to FREF wait 15 msec stage
                 */
                TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"NEW PLL ALGO - hwInit_PllConfigSm(1): bit 4 is set(SYS_CLK_CFG_REG=0x%x), working with FREF clock!!!\n", pHwInit->uClockConfig);
                if (CHIP_ID_1283_PG10 == pHwInit->uChipId) 
                {
                    TRACE0(pHwInit->hReport,REPORT_SEVERITY_INIT,"\n CHIP PG1.0 Detected, Moving to stage 6 (FREF Detection)!!! \n");
                    pHwInit->uPllStage = 5;
                }
                else if (CHIP_ID_1283_PG20 == pHwInit->uChipId) 
                {
                    TRACE0(pHwInit->hReport,REPORT_SEVERITY_INIT,"\n CHIP PG2.0 Detected!!! \n");
                }
                else
                {
                    TRACE0(pHwInit->hReport,REPORT_SEVERITY_ERROR,"\n ERROR!!!!!!!!: unrecognized chip ID found!!! \n");
                    continue;
                }   
                
                pHwInit->uPllStage = 3;
                continue;
            }

            /* 
             * if bit 3 is clear - working with TCXO clock
             */
            TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"NEW PLL ALGO - hwInit_PllConfigSm(1): bit 3 is clear(SYS_CLK_CFG_REG=0x%x), working with TCXO clock, read TCXO_CLK_DETECT_REG\n", pHwInit->uClockConfig);
            continue;

#ifndef TNETW1283
            /* 
             * Read the status of the TXCO detection mechanism 
             */
            status = hwInit_InitTopRegisterRead(hHwInit, TCXO_CLK_DETECT_REG);            
            if (status == TXN_STATUS_PENDING) pHwInit->uPllPendingFlag = 1;

            EXCEPT (pHwInit, status)
#endif
            
        case 2:
            pHwInit->uPllStage++;
            /* register val is in high word */
            uData = pHwInit->uTopRegValue;
            pHwInit->uTxnIndex = 0;


            TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"\n In stage 3, pWlanParams->TcxoRefClk=%d \n", pWlanParams->TcxoRefClk);
            
#ifndef TNETW1283            
            /* 
             * check bit 4 from TCXO_CLK_DETECT_REG
             */
            if (uData & TCXO_DET_FAILED)
            {
                /* 
                 * if bit 4 is set - TCXO detect failure
                 */
                TRACE0(pHwInit->hReport,REPORT_SEVERITY_ERROR,"NEW PLL ALGO - hwInit_PllConfigSm(2): ERROR !!!!!!!! bit 4 is set, TCXO Detect failed\n");
            }
#endif

            /* 
             * check TCXO clock config from INI file
             */
            if ((pWlanParams->TcxoRefClk != CLOCK_CONFIG_16_368_M) && (pWlanParams->TcxoRefClk != CLOCK_CONFIG_32_736_M))
            {
                /* 
                 * not 16.368Mhz and not 32.736Mhz - skip to configure ELP stage
                 */
                TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"NEW PLL ALGO - hwInit_PllConfigSm(2): TcxoRefClk=%d - not 16.368Mhz and not 32.736Mhz - skip to configure ELP stage\n", pWlanParams->TcxoRefClk);
                pHwInit->uPllStage = 5;
                continue;
            }
        
            /*
             * 2. TCXO to FREF switch
             * ----------------------
             */
            TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"NEW PLL ALGO - hwInit_PllConfigSm(2): TcxoRefClk=%d - 16.368Mhz or 32.736Mhz - TCXO to FREF switch started: \n", pWlanParams->TcxoRefClk);

            /* 
             * Write enable FREF_CLK_REQ 
             */
            if (CHIP_ID_1283_PG10 == pHwInit->uChipId)
            {     
                pHwInit->uClockConfig |= WL_CLK_REQ_TYPE_FREF;
                status = hwInit_InitTopRegisterWrite(hHwInit, SYS_CLK_CFG_REG, pHwInit->uClockConfig);            
                pHwInit->uTxnIndex++; 
            }
            else if (CHIP_ID_1283_PG20 == pHwInit->uChipId) 
            {
                uData = 0x68; /* setting bits 3,5 & 6 */
                status = hwInit_InitTopRegisterWrite(hHwInit, WL_SPARE_REG, uData);

                status = hwInit_InitTopRegisterWrite(hHwInit, SYS_CLK_CFG_REG, 0x0D);            
                pHwInit->uTxnIndex++;
            }

            pHwInit->bIsFREFClock = TI_TRUE;
            continue;

        case 3:
            /*
             * 2.1 Wait settling time
             * ----------------------
             */
            pHwInit->uPllStage++;

            TRACE0(pHwInit->hReport,REPORT_SEVERITY_INIT,"NEW PLL ALGO - hwInit_PllConfigSm(3): Wait settling time, Read FREF_CLK_DETECT_REG\n");
#ifdef TNETW1283
            os_StalluSec (pHwInit->hOs, 15000); /* Wait for 15 msec */
            continue;
#else
            os_StalluSec (pHwInit->hOs, pGenParams->SettlingTime*1000);
#endif

#ifndef TNETW1283 
            /* 
             * Read the status of the FREF detection mechanism 
             */
            status = hwInit_InitTopRegisterRead(hHwInit, FREF_CLK_DETECT_REG);            
            if (status == TXN_STATUS_PENDING) pHwInit->uPllPendingFlag = 1;
            
            EXCEPT (pHwInit, status)
#endif
            
        case 4:
            pHwInit->uPllStage++;
            /* register val is in high word */
            uData = pHwInit->uTopRegValue;
            pHwInit->uTxnIndex = 0;

#ifndef TNETW1283
            /* 
             * check bit 4 from FREF_CLK_DETECT_REG
             */
            if (uData & FREF_CLK_DETECT_FAIL)
            {
                /* 
                 * if bit 4 is set - FREF detect failure
                 */
                TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"NEW PLL ALGO - hwInit_PllConfigSm(4): FREF_CLK_DETECT_REG=0x%x, ERROR !!!!!!!! bit 4 is set, FREF Detect failed\n", uData);
            }
#endif

            /* 
             * Configure the MCS PLL's input to be FREF -  MCS_PLL_CLK_SEL_FREF
             * Configure WLAN's clock supply and DRPw's to be FREF -  PRCM_CM_EN_MUX_WLAN_FREF
             */
            if (CHIP_ID_1283_PG10 == pHwInit->uChipId)
            {
                pHwInit->uClockConfig |= (MCS_PLL_CLK_SEL_FREF | PRCM_CM_EN_MUX_WLAN_FREF);
                status = hwInit_InitTopRegisterWrite(hHwInit, SYS_CLK_CFG_REG, pHwInit->uClockConfig); 
                TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"\n NEW PLL ALGO - hwInit_PllConfigSm(4): Configure MCS and WLAN to FREF. uClockConfig=0x%x \n", pHwInit->uClockConfig);           
                
            }
            continue;
            
        case 5:
            /* 
             * 3. Configure MCS PLL settings to TCXO/FREF Freq 
             * -----------------------------------------------
             */
            pHwInit->uPllStage++;
            pHwInit->uTxnIndex = 0;

            
            /* 
             * Set the values that determine the time elapse since the PLL's get their enable signal until the lock indication is set
             */
            if (CHIP_ID_1283_PG10 == pHwInit->uChipId) 
            {
                TRACE0(pHwInit->hReport,REPORT_SEVERITY_INIT,"NEW PLL ALGO - hwInit_PllConfigSm(5): Configure PLL_LOCK_COUNTERS_REG for PG1.0 only \n");
                uData = PLL_LOCK_COUNTERS_COEX | PLL_LOCK_COUNTERS_MCS;
                status = hwInit_InitTopRegisterWrite(hHwInit, PLL_LOCK_COUNTERS_REG, uData);            
                pHwInit->uTxnIndex++;  
            }

            /* 
             * Read MCS PLL in order to set only bits[6:4]
             */
            
            TRACE0(pHwInit->hReport,REPORT_SEVERITY_INIT,"\n NEW PLL ALGO - hwInit_PllConfigSm(5): Read MCS PLL in order to set only bits[6:4]\n");
            status = hwInit_InitTopRegisterRead(hHwInit, MCS_PLL_CONFIG_REG);            
            if (status == TXN_STATUS_PENDING) pHwInit->uPllPendingFlag = 1;
    
            EXCEPT (pHwInit, status)            
    
            

        case 6:
            pHwInit->uPllStage++;
            /* the data ins in the high word */
            uData = pHwInit->uTopRegValue; 
            pHwInit->uTxnIndex = 0;

            TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"\n [stage 7]: MCS_PLL_CONFIG_REG=0x%x ****** \n", pHwInit->uTopRegValue);
            


            if (CHIP_ID_1283_PG20 == pHwInit->uChipId) 
            {
                TRACE0(pHwInit->hReport,REPORT_SEVERITY_INIT,"\n Setting bit 2 in spare register to avoid illegal access \n");
                uData = WL_SPARE_VAL; 
                status = hwInit_InitTopRegisterWrite(hHwInit, WL_SPARE_REG, uData);  

                if (TI_FALSE == pHwInit->bIsFREFClock) 
                {
                    if ((CLOCK_CONFIG_16_8_M == pGenParams->TcxoRefClk) || (CLOCK_CONFIG_33_6_M == pGenParams->TcxoRefClk)) 
                    {
                        TRACE0(pHwInit->hReport,REPORT_SEVERITY_INIT,"\n 16_8_M or 33_6_M TCXO detected so configure the MCS PLL settings manually!!!! \n"); 
                        status = hwInit_InitTopRegisterWrite(hHwInit, MCS_PLL_M_REG, MCS_PLL_M_REG_VAL);
                        status = hwInit_InitTopRegisterWrite(hHwInit, MCS_PLL_N_REG, MCS_PLL_N_REG_VAL);
                        status = hwInit_InitTopRegisterWrite(hHwInit, MCS_PLL_CONFIG_REG, MCS_PLL_CONFIG_REG_VAL);
                        continue;
                    }
                }
            }

            if (pHwInit->bIsFREFClock == TI_TRUE)
            {
            /* 
             * Set the MCS PLL input frequency value according to the FREF/TCXO value detected/read
             */
                TRACE0(pHwInit->hReport,REPORT_SEVERITY_INIT,"!!!!!!!!    FREF CLOCK     !!!!!!!!!!\n");
                uMcsPllConfig = HW_CONFIG_19_2_M; /* default */
                if (pGenParams->RefClk == CLOCK_CONFIG_19_2_M)
                {               
                    uMcsPllConfig = HW_CONFIG_19_2_M;
                    TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"uMcsPllConfig = %d (HW_CONFIG_19_2_M)\n",uMcsPllConfig);
                }
                if ((pGenParams->RefClk == CLOCK_CONFIG_26_M) || (pGenParams->RefClk == CLOCK_CONFIG_26_M_XTAL))
                {                
                   uMcsPllConfig = HW_CONFIG_26_M; 
                   TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"uMcsPllConfig = %d (HW_CONFIG_26_M)\n",uMcsPllConfig);
                }
                if ((pGenParams->RefClk == CLOCK_CONFIG_38_4_M) || (pGenParams->RefClk == CLOCK_CONFIG_38_4_M_XTAL))
                {                
                    uMcsPllConfig = HW_CONFIG_38_4_M;
                    TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"uMcsPllConfig = %d (HW_CONFIG_38_4_M)\n",uMcsPllConfig);
                }
                if (pGenParams->RefClk == CLOCK_CONFIG_52_M)
                {                   
                    uMcsPllConfig = HW_CONFIG_52_M;
                    TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"uMcsPllConfig = %d (HW_CONFIG_52_M)\n",uMcsPllConfig);
                }
#ifdef TNETW1283
                if (pGenParams->RefClk == CLOCK_CONFIG_38_4_M)
                {                
                    uMcsPllConfig = HW_CONFIG_19_2_M;
                    TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"uMcsPllConfig = %d (HW_CONFIG_19_2_M)\n",uMcsPllConfig);
                }
                if (pGenParams->RefClk == CLOCK_CONFIG_52_M)  
                {
                    uMcsPllConfig = HW_CONFIG_26_M;
                    TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"uMcsPllConfig = %d (HW_CONFIG_26_M)\n",uMcsPllConfig);
                }
#endif
           }
           else
           {

                TRACE0(pHwInit->hReport,REPORT_SEVERITY_INIT,"!!!!!!!!    TCXO CLOCK     !!!!!!!!!!\n");
                uMcsPllConfig = HW_CONFIG_19_2_M;
                if (pGenParams->TcxoRefClk == CLOCK_CONFIG_19_2_M)
                {  
                    uMcsPllConfig = HW_CONFIG_19_2_M;
                    TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"uMcsPllConfig = %d (HW_CONFIG_19_2_M)\n",uMcsPllConfig);
                }
                if (pGenParams->TcxoRefClk == CLOCK_CONFIG_26_M)  
                {  
                    uMcsPllConfig = HW_CONFIG_26_M;
                    TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"uMcsPllConfig = %d (HW_CONFIG_26_M)\n",uMcsPllConfig);
                }
                if (pGenParams->TcxoRefClk == CLOCK_CONFIG_38_4_M)
                {  
                    uMcsPllConfig = HW_CONFIG_38_4_M;
                    TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"uMcsPllConfig = %d (HW_CONFIG_38_4_M)\n",uMcsPllConfig);
                }
                if (pGenParams->TcxoRefClk == CLOCK_CONFIG_52_M)   
                { 
                    uMcsPllConfig = HW_CONFIG_52_M;
                    TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"uMcsPllConfig = %d (HW_CONFIG_52_M)\n",uMcsPllConfig);
                }
#ifdef TNETW1283
                if (pGenParams->TcxoRefClk == CLOCK_CONFIG_38_4_M) 
                { 
                    uMcsPllConfig = HW_CONFIG_19_2_M;
                    TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"uMcsPllConfig = %d (HW_CONFIG_19_2_M)\n",uMcsPllConfig);
                }
                if (pGenParams->TcxoRefClk == CLOCK_CONFIG_52_M)    
                {
                    uMcsPllConfig = HW_CONFIG_26_M;
                    TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"uMcsPllConfig = %d (HW_CONFIG_26_M)\n",uMcsPllConfig);
                }
#endif          
            }
           

            uData &= ~MCS_SEL_IN_FREQ_MASK; /* Zero any read bits */
            uData |= (uMcsPllConfig << (MCS_SEL_IN_FREQ_SHIFT)) & (MCS_SEL_IN_FREQ_MASK); /* Bits[6:4]  */
            if (CHIP_ID_1283_PG10 == pHwInit->uChipId) 
            {
                TRACE0(pHwInit->hReport,REPORT_SEVERITY_INIT,"\n Configuring MCS_PLL for CHIP_ID_1283_PG10!!!!! \n");
                uData |= 0x02;
            }
            else
            {
                TRACE0(pHwInit->hReport,REPORT_SEVERITY_INIT,"\n Configuring MCS_PLL for CHIP_ID_1283_PG20!!!!! \n");
                uData |= 0x03;
            }
           
            
            TRACE2(pHwInit->hReport,REPORT_SEVERITY_INIT,"\n NEW PLL ALGO - hwInit_PllConfigSm(6): Write to MCS_PLL_CONFIG_REG value of (Data=0x%x), McsPllConfig=%d\n", uData, uMcsPllConfig);
            status = hwInit_InitTopRegisterWrite(hHwInit, MCS_PLL_CONFIG_REG, uData);            
            continue;

        case 7:
            pHwInit->uPllStage = 0;

            
            if (pHwInit->uPllPendingFlag == 1)
            {
                TRACE0(pHwInit->hReport,REPORT_SEVERITY_INIT,"NEW PLL ALGO - hwInit_PllConfigSm(7): back to bootSm\n");
                hwInit_BootSm (hHwInit);
            }

            TRACE0(pHwInit->hReport,REPORT_SEVERITY_INIT,"NEW PLL ALGO - hwInit_PllConfigSm(7): return TXN_STATUS_COMPLETE\n");
            return TXN_STATUS_COMPLETE;
        } /* switch */
    } /* while */


    TRACE1(pHwInit->hReport,REPORT_SEVERITY_INIT,"NEW PLL ALGO - hwInit_PllConfigSm(%d): Exit\n", pHwInit->uPllStage);
    return TI_OK;
}

#endif

TI_STATUS hwInit_LoadFw (TI_HANDLE hHwInit)
{
    THwInit   *pHwInit = (THwInit *)hHwInit;
    TI_STATUS  status;

    /* check parameters */
    if (hHwInit == NULL)
    {
        EXCEPT (pHwInit, TXN_STATUS_ERROR)
    }

    if (pHwInit->pFwBuf)
    {
        TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT , "CPU halt -> download code\n");

        /* Load firmware image */ 
        pHwInit->uLoadStage = 0;
        status = hwInit_LoadFwImageSm (pHwInit);

        switch (status)
        {
        case TI_OK:
        case TXN_STATUS_OK:
        case TXN_STATUS_COMPLETE:
            WLAN_OS_REPORT (("Firmware successfully downloaded.\n"));
            break;
        case TXN_STATUS_PENDING:
            WLAN_OS_REPORT (("Starting to download firmware...\n"));
            break;
        default:
            TRACE0(pHwInit->hReport, REPORT_SEVERITY_ERROR , "Firmware download failed!\n");
            break;
        }

        EXCEPT (pHwInit, status);
    }   
    else
    {
        TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT , "Firmware not downloaded...\n");

        EXCEPT (pHwInit, TXN_STATUS_ERROR)
    }
            
    WLAN_OS_REPORT (("FW download OK...\n"));
    return TI_OK;
}                                                  
    

/****************************************************************************
 *                      hwInit_FinalizeDownloadSm()
 ****************************************************************************
 * DESCRIPTION: Run the Hardware firmware
 *              Wait for Init Complete
 *              Configure the Bus Access with Addresses available on the scratch pad register 
 *              Change the SDIO/SPI partitions to be able to see all the memory addresses
 * 
 * INPUTS:  None    
 * 
 * OUTPUT:  None
 * 
 * RETURNS: None
 ****************************************************************************/
static TI_STATUS hwInit_FinalizeDownloadSm (TI_HANDLE hHwInit)
{
    THwInit  *pHwInit = (THwInit *)hHwInit;
    TTwd     *pTWD = (TTwd *)pHwInit->hTWD;
    TI_STATUS status = TI_OK;
    TTxnStruct* pTxn;


    while (TI_TRUE)
    {
        switch (pHwInit->uFinStage)
        {
        case 0:
            pHwInit->uFinStage = 1;
            pHwInit->uTxnIndex = 0;
            /*
             * Run the firmware (I) - Read current value from ECPU Control Reg.
             */
            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, ACX_REG_ECPU_CONTROL, 0, 
                               REGISTER_SIZE, TXN_DIRECTION_READ, (TTxnDoneCb)hwInit_FinalizeDownloadSm, hHwInit)
            status = twIf_Transact(pHwInit->hTwIf, pTxn);

            EXCEPT (pHwInit, status)

        case 1:
            pHwInit->uFinStage ++;
            /* We don't zero pHwInit->uTxnIndex at the begining because we need it's value to the next transaction */
            pHwInit->uFinData = pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData;
            /* Now we can zero the index */
            pHwInit->uTxnIndex = 0;

            /*
             * Run the firmware (II) - Take HW out of reset (write ECPU_CONTROL_HALT to ECPU Control Reg.)
             */
            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, ACX_REG_ECPU_CONTROL, (pHwInit->uFinData | ECPU_CONTROL_HALT), 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
            twIf_Transact(pHwInit->hTwIf, pTxn);

            WLAN_OS_REPORT (("Firmware running.\n"));

            pHwInit->uFinLoop = 0;

            TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT , "Wait init complete\n");

        case 2:
            pHwInit->uTxnIndex = 0;

            /* 
             * Wait for init complete 
             */
            if (pHwInit->uFinLoop < FIN_LOOP)
            {           
                pHwInit->uFinStage = 3;

                
#ifndef DOWNLOAD_TIMER_REQUIERD
				os_StalluSec (pHwInit->hOs, 50);
#endif

                /* Read interrupt status register */
                BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, ACX_REG_INTERRUPT_NO_CLEAR, 0, 
                               REGISTER_SIZE, TXN_DIRECTION_READ, (TTxnDoneCb)hwInit_FinalizeDownloadSm, hHwInit)
                status = twIf_Transact(pHwInit->hTwIf, pTxn);

                EXCEPT (pHwInit, status)
            }
            else
			{
				pHwInit->uFinStage = 4;
			}                
            continue;


        case 3:
            /* We don't zero pHwInit->uTxnIndex at the begining because we need it's value to the next transaction */
            pHwInit->uFinData = pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData;
            /* Now we can zero the index */
            pHwInit->uTxnIndex = 0;
            
            if (pHwInit->uFinData == 0xffffffff) /* error */
            {
                TRACE0(pHwInit->hReport, REPORT_SEVERITY_ERROR , "Error reading hardware complete init indication\n");

                pHwInit->DownloadStatus = TXN_STATUS_ERROR;
                EXCEPT (pHwInit, TXN_STATUS_ERROR)
            }
            
            if (IS_MASK_ON (pHwInit->uFinData, ACX_INTR_INIT_COMPLETE))
            {
                pHwInit->uFinStage = 4;

                /* Interrupt ACK */
                BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, ACX_REG_INTERRUPT_ACK, ACX_INTR_INIT_COMPLETE, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
                twIf_Transact(pHwInit->hTwIf, pTxn);

                break;
            }
            else
            {
                pHwInit->uFinStage = 2;
                pHwInit->uFinLoop ++;

#ifdef DOWNLOAD_TIMER_REQUIERD
                tmr_StartTimer (pHwInit->hStallTimer, hwInit_StallTimerCb, hHwInit, STALL_TIMEOUT, TI_FALSE);
                return TXN_STATUS_PENDING;
				#endif
            }
#ifndef DOWNLOAD_TIMER_REQUIERD
                continue;
            #endif
				
        case 4:
            pHwInit->uFinStage++;

            if (pHwInit->uFinLoop >= FIN_LOOP)
            {
                TRACE0(pHwInit->hReport, REPORT_SEVERITY_ERROR , "Timeout waiting for the hardware to complete initialization\n");

                pHwInit->DownloadStatus = TXN_STATUS_ERROR;
                EXCEPT (pHwInit, TXN_STATUS_ERROR);
            }
        
            TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT , "Firmware init complete...\n");
            
            /* 
             * There are valid addresses of the command and event mailbox 
             * on the scratch pad registers 
             */
            /* Hardware config command mail box */
            status = cmdMbox_ConfigHw (pTWD->hCmdMbox,
                                       (fnotify_t)hwInit_FinalizeDownloadSm, 
                                       hHwInit);
            EXCEPT (pHwInit, status)
            
        case 5:
            pHwInit->uFinStage++;

            /* Hardware config event mail box */
            status = eventMbox_InitMboxAddr (pTWD->hEventMbox,
                                         (fnotify_t)hwInit_FinalizeDownloadSm, 
                                         hHwInit);
            EXCEPT (pHwInit, status);
            
        case 6:
            pHwInit->uFinStage++;
            pHwInit->uTxnIndex = 0;

            SET_WORK_PARTITION(pHwInit->aPartition)
            /* Set the bus addresses partition to its "running" mode */
            SET_WORK_PARTITION(pHwInit->aPartition)
            hwInit_SetPartition (pHwInit,pHwInit->aPartition);

            /* Unmask interrupts needed in the FW configuration phase */
            fwEvent_SetInitMask (pTWD->hFwEvent);

            /* Get FW static information from mailbox area */
            BUILD_HW_INIT_FW_STATIC_TXN(pHwInit, pTxn, cmdMbox_GetMboxAddress (pTWD->hCmdMbox),
                                        (TTxnDoneCb)hwInit_FinalizeDownloadSm, hHwInit)
            status = twIf_Transact(pHwInit->hTwIf, pTxn);

            EXCEPT (pHwInit, status);
            continue;

        case 7:
            
            pHwInit->uFinStage = 0;

            cmdBld_FinalizeDownload (pTWD->hCmdBld, &pHwInit->tBootAttr, &(pHwInit->tFwStaticTxn.tFwStaticInfo));

            /* Set the Download Status to COMPLETE */
            pHwInit->DownloadStatus = TXN_STATUS_COMPLETE;

            return TXN_STATUS_COMPLETE;

        } /* End switch */

    } /* End while */

}


/****************************************************************************
 *                      hwInit_ResetSm()
 ****************************************************************************
 * DESCRIPTION: Reset hardware state machine
 * 
 * INPUTS:  None    
 * 
 * OUTPUT:  None
 * 
 * RETURNS: TI_OK or TI_NOK
 ****************************************************************************/
static TI_STATUS hwInit_ResetSm (TI_HANDLE hHwInit)
{
    THwInit *pHwInit = (THwInit *)hHwInit;
    TI_STATUS status = TI_OK;
    TTxnStruct* pTxn;

    pHwInit->uTxnIndex = 0;

        /* Disable Rx/Tx */
    BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, REG_ENABLE_TX_RX, 0x0, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
    status = twIf_Transact(pHwInit->hTwIf, pTxn);
	pHwInit->uTxnIndex++;
	return status;
}


/****************************************************************************
 *                      hwInit_EepromlessStartBurstSm()
 ****************************************************************************
 * DESCRIPTION: prepare eepromless configuration before boot
 * 
 * INPUTS:  
 * 
 * OUTPUT:  
 * 
 * RETURNS: 
 ****************************************************************************/
static TI_STATUS hwInit_EepromlessStartBurstSm (TI_HANDLE hHwInit)
{
    THwInit   *pHwInit = (THwInit *)hHwInit;
    TI_STATUS  status = TI_OK;
    TI_UINT8   *uAddr;
    TI_UINT32  uDeltaLength;
    TTxnStruct* pTxn;

    pHwInit->uTxnIndex = 0;

    while (TI_TRUE)
    {
        switch (pHwInit->uEEPROMStage)
        {
        /* 
         * Stages 0, 1 handles the eeprom format parameters: 
         * ------------------------------------------------
         * Length  - 8bit       --> The length is counted in 32bit words
         * Address - 16bit
         * Data    - (Length * 4) bytes
         * 
         * Note: The nvs is in big endian format and we need to change it to little endian
         */
        case 0: 
            /* Check if address LSB = 1 --> Register address */
            if ((pHwInit->uEEPROMRegAddr = pHwInit->pEEPROMCurPtr[1]) & 1)
            {
                /* Mask the register's address LSB before writing to it */
                pHwInit->uEEPROMRegAddr &= 0xfe;
                /* Change the address's endian */
                pHwInit->uEEPROMRegAddr |= (TI_UINT32)pHwInit->pEEPROMCurPtr[2] << 8;
                /* Length of burst data */
                pHwInit->uEEPROMBurstLen = pHwInit->pEEPROMCurPtr[0];
                pHwInit->pEEPROMCurPtr += 3;
                pHwInit->uEEPROMBurstLoop = 0; 
                /* 
                 * We've finished reading the burst information.
                 * Go to stage 1 in order to write it 
                 */
                pHwInit->uEEPROMStage = 1;
            }
            /* If address LSB = 0 --> We're not in the burst section */
            else
            {
                /* End of Burst transaction: we should see 7 zeroed bytes */
                if (pHwInit->pEEPROMCurPtr[0] == 0)
                {
                    pHwInit->pEEPROMCurPtr += 7;
                }
                pHwInit->uEEPROMCurLen -= (pHwInit->pEEPROMCurPtr - pHwInit->pEEPROMBuf + 1);
                pHwInit->uEEPROMCurLen = (pHwInit->uEEPROMCurLen + NVS_DATA_BUNDARY_ALIGNMENT - 1) & 0xfffffffc;
                /* End of Burst transaction, go to TLV section */
                pHwInit->uEEPROMStage = 2;
            }
            continue;            

        case 1: 
            if (pHwInit->uEEPROMBurstLoop < pHwInit->uEEPROMBurstLen)
            {
                /* Change the data's endian */
                TI_UINT32 val = (pHwInit->pEEPROMCurPtr[0] | 
                                (pHwInit->pEEPROMCurPtr[1] << 8) | 
                                (pHwInit->pEEPROMCurPtr[2] << 16) | 
                                (pHwInit->pEEPROMCurPtr[3] << 24));

                TRACE2(pHwInit->hReport, REPORT_SEVERITY_INIT , "NVS::BurstRead: *(%08x) = %x\n", pHwInit->uEEPROMRegAddr, val);

                BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, (REGISTERS_BASE+pHwInit->uEEPROMRegAddr), val, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, (TTxnDoneCb)hwInit_EepromlessStartBurstSm, hHwInit)
                status = twIf_Transact(pHwInit->hTwIf, pTxn);
 
                pHwInit->uEEPROMStatus = status;
                pHwInit->uEEPROMRegAddr += WORD_SIZE;
                pHwInit->pEEPROMCurPtr +=  WORD_SIZE;
                /* While not end of burst, we stay in stage 1 */
                pHwInit->uEEPROMStage = 1;
                pHwInit->uEEPROMBurstLoop ++;

                EXCEPT (pHwInit, status);
            }
            else
            {
                /* If end of burst return to stage 0 to read the next one */
                pHwInit->uEEPROMStage = 0;
            }
             
            continue;

        case 2:


            pHwInit->uEEPROMStage = 3;
    

            /* Set the bus addresses partition to its "running" mode */
            SET_WORK_PARTITION(pHwInit->aPartition)
            hwInit_SetPartition (pHwInit,pHwInit->aPartition);
            continue;
 
        case 3:
            TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT , "Reached TLV section\n");

            /* Align the host address */
            if (((TI_UINT32)pHwInit->pEEPROMCurPtr & WORD_ALIGNMENT_MASK) && (pHwInit->uEEPROMCurLen > 0) )
            {
                uAddr = (TI_UINT8*)(((TI_UINT32)pHwInit->pEEPROMCurPtr & 0xFFFFFFFC)+WORD_SIZE);
                uDeltaLength = uAddr - pHwInit->pEEPROMCurPtr + 1;

                pHwInit->pEEPROMCurPtr = uAddr;
                pHwInit->uEEPROMCurLen-= uDeltaLength;
            }

            TRACE2(pHwInit->hReport, REPORT_SEVERITY_INIT , "NVS::WriteTLV: pEEPROMCurPtr= %x, Length=%d\n", pHwInit->pEEPROMCurPtr, pHwInit->uEEPROMCurLen);

            if (pHwInit->uEEPROMCurLen)
            {
                /* Save the 4 bytes before the NVS data for WSPI case where they are overrun by the WSPI BusDrv */
                pHwInit->uSavedDataForWspiHdr = *(TI_UINT32 *)(pHwInit->pEEPROMCurPtr - WSPI_PAD_LEN_WRITE);

                /* Prepare the Txn structure for the NVS transaction to the CMD_MBOX */
                HW_INIT_PTXN_SET(pHwInit, pTxn)
                TXN_PARAM_SET_DIRECTION(pTxn, TXN_DIRECTION_WRITE);
                BUILD_TTxnStruct(pTxn, CMD_MBOX_ADDRESS, pHwInit->pEEPROMCurPtr, pHwInit->uEEPROMCurLen, 
                                 (TTxnDoneCb)hwInit_EepromlessStartBurstSm, hHwInit)

                /* Transact the NVS data to the CMD_MBOX */
                status = twIf_Transact(pHwInit->hTwIf, pTxn);
                
                pHwInit->uEEPROMCurLen = 0;
                pHwInit->uNVSStatus = status;

                EXCEPT (pHwInit, status); 
            }
            else
            {
                /* Restore the 4 bytes before the NVS data for WSPI case were they are overrun by the WSPI BusDrv */
                *(TI_UINT32 *)(pHwInit->pEEPROMCurPtr - WSPI_PAD_LEN_WRITE) = pHwInit->uSavedDataForWspiHdr;

                /* Call the upper level state machine */
                if (pHwInit->uEEPROMStatus == TXN_STATUS_PENDING || 
                    pHwInit->uNVSStatus == TXN_STATUS_PENDING)
                {
                    hwInit_BootSm (hHwInit);
                }

                return TXN_STATUS_COMPLETE;
            }
        } /* End switch */
 
    } /* End while */
}

/****************************************************************************
 *                      hwInit_LoadFwImageSm()
 ****************************************************************************
 * DESCRIPTION: Load image from the host and download into the hardware 
 * 
 * INPUTS:  None    
 * 
 * OUTPUT:  None
 * 
 * RETURNS: TI_OK or TI_NOK
 ****************************************************************************/


#define ADDRESS_SIZE		(sizeof(TI_INT32))

static TI_STATUS hwInit_LoadFwImageSm (TI_HANDLE hHwInit)
{
    THwInit *pHwInit 			= (THwInit *)hHwInit;
    TI_STATUS status 			= TI_OK;
	ETxnStatus	TxnStatus;
	TI_UINT32 uMaxPartitionSize	= PARTITION_DOWN_MEM_SIZE;
    TTxnStruct* pTxn;

    pHwInit->uTxnIndex = 0;

    while (TI_TRUE)
    {
        switch (pHwInit->uLoadStage)
        {
		case 0:
            pHwInit->uLoadStage = 1; 

			/* Check the Downloaded FW alignment */
			if ((pHwInit->uFwLength % ADDRESS_SIZE) != 0)
			{
				TRACE1(pHwInit->hReport, REPORT_SEVERITY_ERROR , "Length of downloaded Portion (%d) is not aligned\n",pHwInit->uFwLength);
				EXCEPT_L (pHwInit, TXN_STATUS_ERROR);
			}

			TRACE2(pHwInit->hReport, REPORT_SEVERITY_INIT , "Image addr=0x%x, Len=0x%x\n", pHwInit->pFwBuf, pHwInit->uFwLength);

	/* Set bus memory partition to current download area */
           SET_FW_LOAD_PARTITION(pHwInit->aPartition,pHwInit->uFwAddress)
           hwInit_SetPartition (pHwInit,pHwInit->aPartition);

           pHwInit->pFwTmpBuf = os_memoryAlloc (pHwInit->hOs, WSPI_PAD_LEN_WRITE + MAX_SDIO_BLOCK);

           if( NULL == pHwInit->pFwTmpBuf )
           {
                TRACE0(pHwInit->hReport, REPORT_SEVERITY_ERROR , "hwInit_LoadFwImageSm(): Can not allocate buffer for pHwInit->pFwTmpBuf\n" );
                EXCEPT_L (pHwInit, TXN_STATUS_ERROR);
           }

           status = TI_OK;
			break;

        case 1:

			pHwInit->uLoadStage = 2;
			/* if initial size is smaller than MAX_SDIO_BLOCK - go strait to stage 4 to write partial block */
			if (pHwInit->uFwLength < MAX_SDIO_BLOCK)
			{
				pHwInit->uLoadStage = 4; 
			}

			pHwInit->uBlockReadNum 		= 0;
			pHwInit->uBlockWriteNum 	= 0;
			pHwInit->uPartitionLimit 	= pHwInit->uFwAddress + uMaxPartitionSize;

            continue;
                    
        case 2:

            /* Load firmware by blocks */
 			if (pHwInit->uBlockReadNum < (pHwInit->uFwLength / MAX_SDIO_BLOCK))
            {            
                pHwInit->uLoadStage = 3;

                /* Change partition */
				/* The +2 is for the last block and the block remainder */  
				if ( ((pHwInit->uBlockWriteNum + 2) * MAX_SDIO_BLOCK + pHwInit->uFwAddress) > pHwInit->uPartitionLimit)
                {                					
					pHwInit->uFwAddress += pHwInit->uBlockWriteNum * MAX_SDIO_BLOCK;
					/* update uPartitionLimit */
					pHwInit->uPartitionLimit = pHwInit->uFwAddress + uMaxPartitionSize;
                    /* Set bus memory partition to current download area */
                    SET_FW_LOAD_PARTITION(pHwInit->aPartition,pHwInit->uFwAddress)
                    hwInit_SetPartition (pHwInit,pHwInit->aPartition);
                    TxnStatus = TXN_STATUS_OK;
					pHwInit->uBlockWriteNum = 0;
                    TRACE1(pHwInit->hReport, REPORT_SEVERITY_INIT , "Change partition to address offset = 0x%x\n", 									   pHwInit->uFwAddress + pHwInit->uBlockWriteNum * MAX_SDIO_BLOCK);
                    EXCEPT_L (pHwInit, TxnStatus);                                                     
                }
            }
            else
            {
                pHwInit->uLoadStage = 4;
                TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT , "Load firmware with Portions\n");
            }
            continue;

        case 3:        
            pHwInit->uLoadStage = 2;

            pHwInit->uTxnIndex = 0;

            /* Copy image block to temporary buffer */
            os_memoryCopy (pHwInit->hOs,
                           (void *)&pHwInit->pFwTmpBuf[WSPI_PAD_LEN_WRITE],
						   (void *)(pHwInit->pFwBuf + pHwInit->uBlockReadNum * MAX_SDIO_BLOCK),
						   MAX_SDIO_BLOCK);

            /* Load the block. Save WSPI_PAD_LEN_WRITE space for WSPI bus command */
             BUILD_HW_INIT_FW_DL_TXN(pHwInit, pTxn, (pHwInit->uFwAddress + pHwInit->uBlockWriteNum * MAX_SDIO_BLOCK),
                                     (pHwInit->pFwTmpBuf + WSPI_PAD_LEN_WRITE), MAX_SDIO_BLOCK, TXN_DIRECTION_WRITE,
                                     (TTxnDoneCb)hwInit_LoadFwImageSm, hHwInit)
            TxnStatus = twIf_Transact(pHwInit->hTwIf, pTxn);

            /* Log ERROR if the transaction returned ERROR */
            if (TxnStatus == TXN_STATUS_ERROR)
            {
                TRACE1(pHwInit->hReport, REPORT_SEVERITY_ERROR , "hwInit_LoadFwImageSm: twIf_Transact retruned status=0x%x\n", TxnStatus);
            } 

			pHwInit->uBlockWriteNum ++;
			pHwInit->uBlockReadNum ++;
            EXCEPT_L (pHwInit, TxnStatus);
            continue;

        case 4:    
			pHwInit->uLoadStage 	= 5;

            pHwInit->uTxnIndex = 0;

			/* If No Last block to write */
			if ( pHwInit->uFwLength % MAX_SDIO_BLOCK == 0 )
			{
				continue;
			}


            /* Copy the last image block */
             os_memoryCopy (pHwInit->hOs,
                           (void *)&pHwInit->pFwTmpBuf[WSPI_PAD_LEN_WRITE],
						   (void *)(pHwInit->pFwBuf + pHwInit->uBlockReadNum * MAX_SDIO_BLOCK),
						   pHwInit->uFwLength % MAX_SDIO_BLOCK);

            /* Load the last block */
             BUILD_HW_INIT_FW_DL_TXN(pHwInit, pTxn, (pHwInit->uFwAddress + pHwInit->uBlockWriteNum * MAX_SDIO_BLOCK),
                                     (pHwInit->pFwTmpBuf + WSPI_PAD_LEN_WRITE), (pHwInit->uFwLength % MAX_SDIO_BLOCK), TXN_DIRECTION_WRITE,
                                     (TTxnDoneCb)hwInit_LoadFwImageSm, hHwInit)
            TxnStatus = twIf_Transact(pHwInit->hTwIf, pTxn);

            if (TxnStatus == TXN_STATUS_ERROR)
			{
                TRACE1(pHwInit->hReport, REPORT_SEVERITY_ERROR , "hwInit_LoadFwImageSm: last block retruned status=0x%x\n", TxnStatus);
			}

            EXCEPT_L (pHwInit, TxnStatus);
            continue;

        case 5:
            pHwInit->uLoadStage = 0;

            if( NULL != pHwInit->pFwTmpBuf )
            {
                os_memoryFree (pHwInit->hOs, pHwInit->pFwTmpBuf, WSPI_PAD_LEN_WRITE + MAX_SDIO_BLOCK);
                pHwInit->pFwTmpBuf = NULL;
            }

			/*If end of overall FW Download Process: Finalize download (run firmware)*/
			if ( pHwInit->bFwBufLast == TI_TRUE )
			{			
				/* The download has completed */ 
				WLAN_OS_REPORT (("Finished downloading firmware.\n"));
				status = hwInit_FinalizeDownloadSm (hHwInit);
			}
			/* Have to wait to more FW Portions */
			else
			{
				/* Call the upper layer callback */
				if ( pHwInit->fFinalizeDownload != NULL )
				{
					(pHwInit->fFinalizeDownload) (pHwInit->hFinalizeDownload);
				}

				status = TI_OK;
			}
            return status;

        } /* End switch */

    } /* End while */

} /* hwInit_LoadFwImageSm() */

#define READ_TOP_REG_LOOP  32
#ifdef TNETW1283
#define TOP_REG_ADDR_MASK    0x1FFF
#else
#define TOP_REG_ADDR_MASK    0x7FF
#endif



/****************************************************************************
 *                      hwInit_InitPoalrity()
 ****************************************************************************
 * DESCRIPTION: hwInit_ReadRadioParamsSm 
 * initalizie hwInit_ReadRadioParamsSm parmaeters
  ****************************************************************************/
   
TI_STATUS hwInit_InitPolarity(TI_HANDLE hHwInit)
{
  THwInit      *pHwInit = (THwInit *)hHwInit;

  pHwInit->uRegStage = 0;
  pHwInit->uRegSeqStage = 0;
 
  return hwInit_WriteIRQPolarity (hHwInit);
}



/****************************************************************************
 *                      hwInit_WriteIRQPolarity ()
 ****************************************************************************
 * DESCRIPTION: hwInit_WriteIRQPolarity
  * INPUTS:  None    
 * 
 * OUTPUT:  None
 * 
 * RETURNS: TI_OK or TI_NOK
 ****************************************************************************/
 TI_STATUS hwInit_WriteIRQPolarity(TI_HANDLE hHwInit)
 {
     THwInit     *pHwInit = (THwInit *)hHwInit;
     TI_UINT32   Address,value;
     TI_UINT32   val=0;
     TTxnStruct  *pTxn;
     TI_STATUS   status = 0;

   /*  To write to a top level address from the WLAN IP:
       Write the top level address to the OCP_POR_CTR register. 
       Divide the top address by 2, and add 0x30000 to the result � for example for top address 0xC00, write to the OCP_POR_CTR 0x30600
       Write the data to the OCP_POR_WDATA register
       Write 0x1 to the OCP_CMD register. 

      To read from a top level address:
      Write the top level address to the OCP_POR_CTR register.
      Divide the top address by 2, and add 0x30000 to the result � for example for top address 0xC00, write to the OCP_POR_CTR 0x30600 
      Write 0x2 to the OCP_CMD register. 
      Poll bit [18] of OCP_DATA_RD for data valid indication
      Check bits 17:16 of OCP_DATA_RD:
      00 � no response
      01 � data valid / accept
      10 � request failed
      11 � response error
      Read the data from the OCP_DATA_RD register
   */
      
     while (TI_TRUE)
     {
         switch (pHwInit->uRegStage)
         {
         case 0:

             pHwInit->uRegStage = 1;
             pHwInit->uTxnIndex++;
             pHwInit->uRegLoop = 0;

             /* first read the IRQ Polarity register*/
             Address = (TI_UINT32)(FN0_CCCR_REG_32 / 2);
             val = (Address & TOP_REG_ADDR_MASK);
             val |= BIT_16 | BIT_17;

             /* Write IRQ Polarity address register to OCP_POR_CTR*/
             BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_POR_CTR, val, 
                                REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)

             twIf_Transact(pHwInit->hTwIf, pTxn);

             pHwInit->uTxnIndex++;  

             /* Write read (2)command to the OCP_CMD register. */

             BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_CMD, 0x2, 
                                REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
             twIf_Transact(pHwInit->hTwIf, pTxn);

             continue;

         case 1:
             
             pHwInit->uRegStage ++;
             pHwInit->uTxnIndex++; 

             BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_DATA_RD, 0, 
                                REGISTER_SIZE, TXN_DIRECTION_READ, (TTxnDoneCb)hwInit_WriteIRQPolarity, hHwInit)
             status = twIf_Transact(pHwInit->hTwIf, pTxn);

             EXCEPT (pHwInit, status)


         case 2:
             /* get the value from  IRQ Polarity register*/
             val = pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData;

             pHwInit->uTxnIndex = 0;

             /*Poll bit 18 of OCP_DATA_RD for data valid indication*/
             if (val & BIT_18)
             {
               if ((val & BIT_16) && (!(val & BIT_17)))
               {
                   pHwInit->uRegStage ++;
                   pHwInit->uRegLoop = 0;

               }
               else 
               {
                 TRACE0(pHwInit->hReport, REPORT_SEVERITY_ERROR , "can't writing bt_func7_sel\n");
                 TWD_FinalizePolarityRead(pHwInit->hTWD);

                return TI_NOK;
               }
             }
             else
             {
               if (pHwInit->uRegLoop < READ_TOP_REG_LOOP)
               {
                  pHwInit->uRegStage = 1;
                  pHwInit->uRegLoop++;
               }
               else 
               {

                 TRACE0(pHwInit->hReport, REPORT_SEVERITY_ERROR , "Timeout waiting for writing bt_func7_sel\n");
                 TWD_FinalizePolarityRead(pHwInit->hTWD);

                return TI_NOK;

               }
             }

             continue;


         case 3:
               /* second, write new value of IRQ polarity due to complation flag 1 - active low, 0 - active high*/
                pHwInit->uRegStage ++;
                Address = (TI_UINT32)(FN0_CCCR_REG_32 / 2);
                value = (Address & TOP_REG_ADDR_MASK);
                value |= BIT_16 | BIT_17;

                /* Write IRQ Polarity address register to OCP_POR_CTR*/
               
                BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_POR_CTR, value, 
                                   REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)

                twIf_Transact(pHwInit->hTwIf, pTxn);

                pHwInit->uTxnIndex++;  

#ifdef USE_IRQ_ACTIVE_HIGH
                TRACE0(pHwInit->hReport, REPORT_SEVERITY_INFORMATION , "Hwinit IRQ polarity active high\n");
                val |= 0x0<<1;
                    
#else
                TRACE0(pHwInit->hReport, REPORT_SEVERITY_INFORMATION , "Hwinit IRQ polarity active low\n");
                val |= 0x01<<1;
#endif

              /* Write the new IRQ polarity value to the OCP_POR_WDATA register */
                BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_POR_WDATA, val, 
                                   REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
                twIf_Transact(pHwInit->hTwIf, pTxn);

                pHwInit->uTxnIndex++; 

               /* Write write (1)command to the OCP_CMD register. */
                BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_CMD, 0x1, 
                                   REGISTER_SIZE, TXN_DIRECTION_WRITE, (TTxnDoneCb)hwInit_WriteIRQPolarity, hHwInit)
                status = twIf_Transact(pHwInit->hTwIf, pTxn);

                pHwInit->uTxnIndex++; 

                EXCEPT (pHwInit, status)              
                continue;

         case 4:

               TWD_FinalizePolarityRead(pHwInit->hTWD);

              return TI_OK;

          
         } /* End switch */

     } /* End while */

 }

#ifndef FPGA_SKIP_TOP_INIT

/****************************************************************************
 *                      hwInit_InitTopRegisterWrite()
 ****************************************************************************
 * DESCRIPTION: hwInit_InitTopRegisterWrite 
 * initalizie hwInit_TopRegisterWrite SM parmaeters
  ****************************************************************************/
   
TI_STATUS hwInit_InitTopRegisterWrite(TI_HANDLE hHwInit, TI_UINT32 uAddress, TI_UINT32 uValue)
{
  THwInit      *pHwInit = (THwInit *)hHwInit;

  TRACE2(pHwInit->hReport,REPORT_SEVERITY_INIT,"hwInit_InitTopRegisterWrite: address = 0x%x, value = 0x%x\n", uAddress, uValue);
  pHwInit->uTopStage = 0;
  uAddress = (TI_UINT32)(uAddress / 2);
  uAddress = (uAddress & TOP_REG_ADDR_MASK);  
  uAddress|= BIT_16 | BIT_17;
  pHwInit->uTopRegAddr = uAddress;
  pHwInit->uTopRegValue = uValue & 0xffff;  
  return hwInit_TopRegisterWrite (hHwInit);
}


/****************************************************************************
 *                      hwInit_TopRegisterWrite ()
 ****************************************************************************
 * DESCRIPTION: Generic function that writes to the top registers area
  * INPUTS:  None    
 * 
 * OUTPUT:  None
 * 
 * RETURNS: TI_OK or TI_NOK
 ****************************************************************************/
 TI_STATUS hwInit_TopRegisterWrite(TI_HANDLE hHwInit)
 {
     /*  To write to a top level address from the WLAN IP:
         Write the top level address to the OCP_POR_CTR register. 
         Divide the top address by 2, and add 0x30000 to the result � for example for top address 0xC00, write to the OCP_POR_CTR 0x30600
         Write the data to the OCP_POR_WDATA register
         Write 0x1 to the OCP_CMD register. 
     */ 
     THwInit *pHwInit = (THwInit *)hHwInit;
     TTxnStruct *pTxn;
        
     while (TI_TRUE)
     {
         switch (pHwInit->uTopStage)
         {
         case 0:
             pHwInit->uTopStage = 1;

             pHwInit->uTxnIndex++;
             /* Write the address to OCP_POR_CTR*/
             BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_POR_CTR, pHwInit->uTopRegAddr,
                                    REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
             twIf_Transact(pHwInit->hTwIf, pTxn);

             pHwInit->uTxnIndex++;
             /* Write the new value to the OCP_POR_WDATA register */
             BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_POR_WDATA, pHwInit->uTopRegValue, 
                                    REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
             twIf_Transact(pHwInit->hTwIf, pTxn);

             pHwInit->uTxnIndex++; 
             /* Write write (1)command to the OCP_CMD register. */
             BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_CMD, 0x1, 
                                    REGISTER_SIZE, TXN_DIRECTION_WRITE, (TTxnDoneCb)hwInit_TopRegisterWrite, hHwInit)
             pHwInit->uTopStatus = twIf_Transact(pHwInit->hTwIf, pTxn);

             pHwInit->uTxnIndex++;

             EXCEPT (pHwInit, pHwInit->uTopStatus)              
             continue;

         case 1:

             pHwInit->uTxnIndex = 0;
             
             if (pHwInit->uTopStatus == TXN_STATUS_PENDING) 
             {
                 hwInit_BootSm (hHwInit);
             }
             
             return TI_OK;
         
         } /* End switch */

     } /* End while */

 }


 /****************************************************************************
 *                      hwInit_InitTopRegisterRead()
 ****************************************************************************
 * DESCRIPTION: hwInit_InitTopRegisterRead 
 * initalizie hwInit_InitTopRegisterRead SM parmaeters
  ****************************************************************************/
   
TI_STATUS hwInit_InitTopRegisterRead(TI_HANDLE hHwInit, TI_UINT32 uAddress)
{
  THwInit      *pHwInit = (THwInit *)hHwInit;

  TRACE1(pHwInit->hReport, REPORT_SEVERITY_INIT ,"hwInit_InitTopRegisterRead: address = 0x%x\n", uAddress);
  pHwInit->uTopStage = 0;
  uAddress = (TI_UINT32)(uAddress / 2);
  uAddress = (uAddress & TOP_REG_ADDR_MASK);  
  uAddress|= BIT_16 | BIT_17;
  pHwInit->uTopRegAddr = uAddress;

  return hwInit_TopRegisterRead (hHwInit);
}


/****************************************************************************
 *                      hwInit_TopRegisterRead ()
 ****************************************************************************
 * DESCRIPTION: Generic function that reads the top registers area
  * INPUTS:  None    
 * 
 * OUTPUT:  None
 * 
 * RETURNS: TI_OK or TI_NOK
 ****************************************************************************/
 TI_STATUS hwInit_TopRegisterRead(TI_HANDLE hHwInit)
 {
     /*  
        To read from a top level address:
        Write the top level address to the OCP_POR_CTR register.
        Divide the top address by 2, and add 0x30000 to the result � for example for top address 0xC00, write to the OCP_POR_CTR 0x30600 
        Write 0x2 to the OCP_CMD register. 
        Poll bit [18] of OCP_DATA_RD for data valid indication
        Check bits 17:16 of OCP_DATA_RD:
        00 - no response
        01 - data valid / accept
        10 - request failed
        11 - response error
        Read the data from the OCP_DATA_RD register
     */

     THwInit *pHwInit = (THwInit *)hHwInit;
     TTxnStruct *pTxn;
        
     while (TI_TRUE)
     {
         switch (pHwInit->uTopStage)
         {
         case 0:
             pHwInit->uTopStage = 1;
             pHwInit->uTxnIndex++;
             pHwInit->uRegLoop = 0;

             /* Write the address to OCP_POR_CTR*/
             BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_POR_CTR, pHwInit->uTopRegAddr,
                                    REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
             twIf_Transact(pHwInit->hTwIf, pTxn);

             pHwInit->uTxnIndex++;  
             BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_CMD, 0x2, 
                                    REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
             twIf_Transact(pHwInit->hTwIf, pTxn);

             continue;

         case 1:
             pHwInit->uTopStage ++;
             pHwInit->uTxnIndex++; 

             BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_DATA_RD, 0, 
                                    REGISTER_SIZE, TXN_DIRECTION_READ, (TTxnDoneCb)hwInit_TopRegisterRead, hHwInit)
             pHwInit->uTopStatus = twIf_Transact(pHwInit->hTwIf, pTxn);
             
             EXCEPT (pHwInit, pHwInit->uTopStatus)

         case 2:
             /* get the value from  IRQ Polarity register*/
             pHwInit->uTopRegValue = pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData;
            
             pHwInit->uTxnIndex = 0;

             /*Poll bit 18 of OCP_DATA_RD for data valid indication*/
             if (pHwInit->uTopRegValue & BIT_18)
             {
               if ((pHwInit->uTopRegValue & BIT_16) && (!(pHwInit->uTopRegValue & BIT_17)))
               {                   
                   pHwInit->uTopRegValue &= 0xffff;
                   
                   pHwInit->uTxnIndex = 0;
                   pHwInit->uRegLoop = 0;
                   if (pHwInit->uTopStatus == TXN_STATUS_PENDING) 
                   {
                       hwInit_BootSm (hHwInit);
                   }
                   return TI_OK;
               }
               else 
               {
                 TRACE0(pHwInit->hReport, REPORT_SEVERITY_ERROR , "can't write bt_func7_sel\n");                 
                 if (pHwInit->uTopStatus == TXN_STATUS_PENDING) 
                 {
                       hwInit_BootSm (hHwInit);
                 }
                 return TI_NOK;
               }
             }
             else
             {
               if (pHwInit->uRegLoop < READ_TOP_REG_LOOP)
               {
                  pHwInit->uTopStage = 1;
                  pHwInit->uRegLoop++;
               }
               else 
               {
                 TRACE0(pHwInit->hReport, REPORT_SEVERITY_ERROR , "Timeout waiting for writing bt_func7_sel\n");                 
                 if (pHwInit->uTopStatus == TXN_STATUS_PENDING) 
                 {
                       hwInit_BootSm (hHwInit);
                 }
                 return TI_NOK;
               }
              }

             continue;
         
         } /* End switch */

     } /* End while */

 }
#endif

/****************************************************************************
*                      hwInit_StallTimerCb ()
****************************************************************************
* DESCRIPTION: CB timer function in fTimerFunction format that calls hwInit_StallTimerCb
* INPUTS:  TI_HANDLE hHwInit    
* 
* OUTPUT:  None
* 
* RETURNS: None
****************************************************************************/
#ifdef DOWNLOAD_TIMER_REQUIERD
 static void hwInit_StallTimerCb (TI_HANDLE hHwInit, TI_BOOL bTwdInitOccured)
{
	hwInit_FinalizeDownloadSm(hHwInit);
}

#endif


