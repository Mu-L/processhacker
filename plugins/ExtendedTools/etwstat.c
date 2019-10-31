/*
 * Process Hacker Extended Tools -
 *   ETW statistics collection
 *
 * Copyright (C) 2010-2011 wj32
 * Copyright (C) 2019 dmex
 *
 * This file is part of Process Hacker.
 *
 * Process Hacker is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Process Hacker is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Process Hacker.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "exttools.h"
#include "etwmon.h"

VOID NTAPI EtEtwProcessesUpdatedCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    );

VOID NTAPI EtEtwNetworkItemsUpdatedCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    );

BOOLEAN EtDiskExtEnabled = FALSE;
static PH_CALLBACK_REGISTRATION EtpProcessesUpdatedCallbackRegistration;
static PH_CALLBACK_REGISTRATION EtpNetworkItemsUpdatedCallbackRegistration;

ULONG EtpDiskReadRaw;
ULONG EtpDiskWriteRaw;
ULONG EtpNetworkReceiveRaw;
ULONG EtpNetworkSendRaw;

ULONG EtDiskReadCount;
ULONG EtDiskWriteCount;
ULONG EtNetworkReceiveCount;
ULONG EtNetworkSendCount;

PH_UINT32_DELTA EtDiskReadDelta;
PH_UINT32_DELTA EtDiskWriteDelta;
PH_UINT32_DELTA EtNetworkReceiveDelta;
PH_UINT32_DELTA EtNetworkSendDelta;

PH_UINT32_DELTA EtDiskReadCountDelta;
PH_UINT32_DELTA EtDiskWriteCountDelta;
PH_UINT32_DELTA EtNetworkReceiveCountDelta;
PH_UINT32_DELTA EtNetworkSendCountDelta;

PH_CIRCULAR_BUFFER_ULONG EtDiskReadHistory;
PH_CIRCULAR_BUFFER_ULONG EtDiskWriteHistory;
PH_CIRCULAR_BUFFER_ULONG EtNetworkReceiveHistory;
PH_CIRCULAR_BUFFER_ULONG EtNetworkSendHistory;
PH_CIRCULAR_BUFFER_ULONG EtMaxDiskHistory; // ID of max. disk usage process
PH_CIRCULAR_BUFFER_ULONG EtMaxNetworkHistory; // ID of max. network usage process

PVOID EtpProcessInformation;
PH_QUEUED_LOCK EtpProcessInformationLock = PH_QUEUED_LOCK_INIT;

VOID EtEtwStatisticsInitialization(
    VOID
    )
{
    ULONG sampleCount;

    EtDiskExtEnabled = !!PhGetIntegerSetting(SETTING_NAME_ENABLE_DISKEXT);
    sampleCount = PhGetIntegerSetting(L"SampleCount");
    PhInitializeCircularBuffer_ULONG(&EtDiskReadHistory, sampleCount);
    PhInitializeCircularBuffer_ULONG(&EtDiskWriteHistory, sampleCount);
    PhInitializeCircularBuffer_ULONG(&EtNetworkReceiveHistory, sampleCount);
    PhInitializeCircularBuffer_ULONG(&EtNetworkSendHistory, sampleCount);
    PhInitializeCircularBuffer_ULONG(&EtMaxDiskHistory, sampleCount);
    PhInitializeCircularBuffer_ULONG(&EtMaxNetworkHistory, sampleCount);

    EtEtwMonitorInitialization();

    PhRegisterCallback(
        PhGetGeneralCallback(GeneralCallbackProcessProviderUpdatedEvent),
        EtEtwProcessesUpdatedCallback,
        NULL,
        &EtpProcessesUpdatedCallbackRegistration
        );

    if (EtEtwEnabled)
    {
        PhRegisterCallback(
            PhGetGeneralCallback(GeneralCallbackNetworkProviderUpdatedEvent),
            EtEtwNetworkItemsUpdatedCallback,
            NULL,
            &EtpNetworkItemsUpdatedCallbackRegistration
            );
    }
}

VOID EtEtwStatisticsUninitialization(
    VOID
    )
{
    EtEtwMonitorUninitialization();
}

VOID EtProcessDiskEvent(
    _In_ PET_ETW_DISK_EVENT Event
    )
{
    PPH_PROCESS_ITEM processItem;
    PET_PROCESS_BLOCK block;

    if (Event->Type == EtEtwDiskReadType)
    {
        EtpDiskReadRaw += Event->TransferSize;
        EtDiskReadCount++;
    }
    else
    {
        EtpDiskWriteRaw += Event->TransferSize;
        EtDiskWriteCount++;
    }

    if (processItem = PhReferenceProcessItem(Event->ClientId.UniqueProcess))
    {
        block = EtGetProcessBlock(processItem);

        if (Event->Type == EtEtwDiskReadType)
        {
            block->DiskReadRaw += Event->TransferSize;
            block->DiskReadCount++;
        }
        else
        {
            block->DiskWriteRaw += Event->TransferSize;
            block->DiskWriteCount++;
        }

        PhDereferenceObject(processItem);
    }
}

VOID EtProcessNetworkEvent(
    _In_ PET_ETW_NETWORK_EVENT Event
    )
{
    PPH_PROCESS_ITEM processItem;
    PET_PROCESS_BLOCK block;
    PPH_NETWORK_ITEM networkItem;
    PET_NETWORK_BLOCK networkBlock;

    if (Event->Type == EtEtwNetworkReceiveType)
    {
        EtpNetworkReceiveRaw += Event->TransferSize;
        EtNetworkReceiveCount++;
    }
    else
    {
        EtpNetworkSendRaw += Event->TransferSize;
        EtNetworkSendCount++;
    }

    // Note: there is always the possibility of us receiving the event too early,
    // before the process item or network item is created. So events may be lost.

    if (processItem = PhReferenceProcessItem(Event->ClientId.UniqueProcess))
    {
        block = EtGetProcessBlock(processItem);

        if (Event->Type == EtEtwNetworkReceiveType)
        {
            block->NetworkReceiveRaw += Event->TransferSize;
            block->NetworkReceiveCount++;
        }
        else
        {
            block->NetworkSendRaw += Event->TransferSize;
            block->NetworkSendCount++;
        }

        PhDereferenceObject(processItem);
    }

    networkItem = PhReferenceNetworkItem(
        Event->ProtocolType,
        &Event->LocalEndpoint,
        &Event->RemoteEndpoint,
        Event->ClientId.UniqueProcess
        );

    if (!networkItem && Event->ProtocolType & PH_UDP_PROTOCOL_TYPE)
    {
        // Note: ETW generates UDP events with the LocalEndpoint set to the LAN endpoint address 
        // of the local adapter the packet was sent or recieved but GetExtendedUdpTable 
        // returns some UDP connections with endpoints set to in4addr_any/in6addr_any (zero). (dmex)

        if (Event->ProtocolType & PH_IPV4_NETWORK_TYPE)
            memset(&Event->LocalEndpoint.Address.InAddr, 0, sizeof(IN_ADDR)); // same as in4addr_any
        else
            memset(&Event->LocalEndpoint.Address.In6Addr, 0, sizeof(IN6_ADDR)); // same as in6addr_any

        networkItem = PhReferenceNetworkItem(
            Event->ProtocolType,
            &Event->LocalEndpoint,
            &Event->RemoteEndpoint,
            Event->ClientId.UniqueProcess
            );
    }

    if (networkItem)
    {
        networkBlock = EtGetNetworkBlock(networkItem);

        if (Event->Type == EtEtwNetworkReceiveType)
        {
            networkBlock->ReceiveRaw += Event->TransferSize;
            networkBlock->ReceiveCount++;
        }
        else
        {
            networkBlock->SendRaw += Event->TransferSize;
            networkBlock->SendCount++;
        }

        PhDereferenceObject(networkItem);
    }
}

VOID NTAPI EtEtwProcessesUpdatedCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    static ULONG runCount = 0; // MUST keep in sync with runCount in process provider

    if (
        EtDiskExtEnabled &&
        WindowsVersion >= WINDOWS_10_RS3 &&
        !PhIsExecutingInWow64() &&
        !PhGetOwnTokenAttributes().Elevated
        )
    {
        PSYSTEM_PROCESS_INFORMATION process;

        if (!(process = PhGetProcessInformationCache()))
            return;

        do
        {
            PPH_PROCESS_ITEM processItem;

            processItem = PhReferenceProcessItem(process->UniqueProcessId);

            if (!processItem)
                processItem = PhReferenceProcessItem(SYSTEM_PROCESS_ID);

            if (processItem)
            {
                PSYSTEM_PROCESS_INFORMATION_EXTENSION processExtension;
                PET_PROCESS_BLOCK block;

                if (!(processExtension = PH_PROCESS_EXTENSION(process)))
                    break;
                if (!(block = EtGetProcessBlock(processItem)))
                    break;

                block->DiskReadRaw = processExtension->DiskCounters.BytesRead;
                block->DiskWriteRaw = processExtension->DiskCounters.BytesWritten;

                PhUpdateDelta(&block->DiskReadRawDelta, block->DiskReadRaw);
                PhUpdateDelta(&block->DiskWriteRawDelta, block->DiskWriteRaw);

                if (!block->HaveFirstSample)
                {
                    block->DiskReadRawDelta.Delta = 0;
                    block->DiskWriteRawDelta.Delta = 0;
                    block->HaveFirstSample = TRUE;
                }

                if (runCount != 0)
                {
                    block->CurrentDiskRead = block->DiskReadRawDelta.Delta;
                    block->CurrentDiskWrite = block->DiskWriteRawDelta.Delta;

                    PhAddItemCircularBuffer_ULONG64(&block->DiskReadHistory, block->CurrentDiskRead);
                    PhAddItemCircularBuffer_ULONG64(&block->DiskWriteHistory, block->CurrentDiskWrite);
                }

                PhDereferenceObject(processItem);
            }
 
        } while (process = PH_NEXT_PROCESS(process));
    }
    else
    {       
        ULONG64 maxDiskValue = 0;
        PET_PROCESS_BLOCK maxDiskBlock = NULL;
        ULONG64 maxNetworkValue = 0;
        PET_PROCESS_BLOCK maxNetworkBlock = NULL;
        PLIST_ENTRY listEntry;

        // Since Windows 8, we no longer get the correct process/thread IDs in the
        // event headers for disk events. We need to update our process information since
        // etwmon uses our EtThreadIdToProcessId function.
        if (WindowsVersion >= WINDOWS_8)
            EtUpdateProcessInformation();

        // ETW is extremely lazy when it comes to flushing buffers, so we must do it
        // manually.
        EtFlushEtwSession();

        // Update global statistics.

        PhUpdateDelta(&EtDiskReadDelta, EtpDiskReadRaw);
        PhUpdateDelta(&EtDiskWriteDelta, EtpDiskWriteRaw);
        PhUpdateDelta(&EtNetworkReceiveDelta, EtpNetworkReceiveRaw);
        PhUpdateDelta(&EtNetworkSendDelta, EtpNetworkSendRaw);

        PhUpdateDelta(&EtDiskReadCountDelta, EtDiskReadCount);
        PhUpdateDelta(&EtDiskWriteCountDelta, EtDiskWriteCount);
        PhUpdateDelta(&EtNetworkReceiveCountDelta, EtNetworkReceiveCount);
        PhUpdateDelta(&EtNetworkSendCountDelta, EtNetworkSendCount);

        // Update per-process statistics.
        // Note: no lock is needed because we only ever modify the list on this same thread.

        listEntry = EtProcessBlockListHead.Flink;

        while (listEntry != &EtProcessBlockListHead)
        {
            PET_PROCESS_BLOCK block;

            block = CONTAINING_RECORD(listEntry, ET_PROCESS_BLOCK, ListEntry);

            PhUpdateDelta(&block->DiskReadDelta, block->DiskReadCount);
            PhUpdateDelta(&block->DiskReadRawDelta, block->DiskReadRaw);
            PhUpdateDelta(&block->DiskWriteDelta, block->DiskWriteCount);
            PhUpdateDelta(&block->DiskWriteRawDelta, block->DiskWriteRaw);
            PhUpdateDelta(&block->NetworkReceiveDelta, block->NetworkReceiveCount);
            PhUpdateDelta(&block->NetworkReceiveRawDelta, block->NetworkReceiveRaw);
            PhUpdateDelta(&block->NetworkSendDelta, block->NetworkSendCount);
            PhUpdateDelta(&block->NetworkSendRawDelta, block->NetworkSendRaw);

            if (maxDiskValue < block->DiskReadRawDelta.Delta + block->DiskWriteRawDelta.Delta)
            {
                maxDiskValue = block->DiskReadRawDelta.Delta + block->DiskWriteRawDelta.Delta;
                maxDiskBlock = block;
            }

            if (maxNetworkValue < block->NetworkReceiveRawDelta.Delta + block->NetworkSendRawDelta.Delta)
            {
                maxNetworkValue = block->NetworkReceiveRawDelta.Delta + block->NetworkSendRawDelta.Delta;
                maxNetworkBlock = block;
            }

            if (runCount != 0)
            {
                block->CurrentDiskRead = block->DiskReadRawDelta.Delta;
                block->CurrentDiskWrite = block->DiskWriteRawDelta.Delta;
                block->CurrentNetworkSend = block->NetworkSendRawDelta.Delta;
                block->CurrentNetworkReceive = block->NetworkReceiveRawDelta.Delta;

                PhAddItemCircularBuffer_ULONG64(&block->DiskReadHistory, block->CurrentDiskRead);
                PhAddItemCircularBuffer_ULONG64(&block->DiskWriteHistory, block->CurrentDiskWrite);
                PhAddItemCircularBuffer_ULONG64(&block->NetworkSendHistory, block->CurrentNetworkSend);
                PhAddItemCircularBuffer_ULONG64(&block->NetworkReceiveHistory, block->CurrentNetworkReceive);
            }

            listEntry = listEntry->Flink;
        }

        // Update history buffers.

        if (runCount != 0)
        {
            PhAddItemCircularBuffer_ULONG(&EtDiskReadHistory, EtDiskReadDelta.Delta);
            PhAddItemCircularBuffer_ULONG(&EtDiskWriteHistory, EtDiskWriteDelta.Delta);
            PhAddItemCircularBuffer_ULONG(&EtNetworkReceiveHistory, EtNetworkReceiveDelta.Delta);
            PhAddItemCircularBuffer_ULONG(&EtNetworkSendHistory, EtNetworkSendDelta.Delta);

            if (maxDiskBlock)
            {
                PhAddItemCircularBuffer_ULONG(&EtMaxDiskHistory, HandleToUlong(maxDiskBlock->ProcessItem->ProcessId));
                PhReferenceProcessRecordForStatistics(maxDiskBlock->ProcessItem->Record);
            }
            else
            {
                PhAddItemCircularBuffer_ULONG(&EtMaxDiskHistory, 0);
            }

            if (maxNetworkBlock)
            {
                PhAddItemCircularBuffer_ULONG(&EtMaxNetworkHistory, HandleToUlong(maxNetworkBlock->ProcessItem->ProcessId));
                PhReferenceProcessRecordForStatistics(maxNetworkBlock->ProcessItem->Record);
            }
            else
            {
                PhAddItemCircularBuffer_ULONG(&EtMaxNetworkHistory, 0);
            }
        }
    }

    runCount++;
}

VOID NTAPI EtEtwNetworkItemsUpdatedCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PLIST_ENTRY listEntry;

    // ETW is flushed in the processes-updated callback above. This may cause us the network
    // blocks to all fall one update interval behind, however.

    // Update per-connection statistics.
    // Note: no lock is needed because we only ever modify the list on this same thread.

    listEntry = EtNetworkBlockListHead.Flink;

    while (listEntry != &EtNetworkBlockListHead)
    {
        PET_NETWORK_BLOCK block;
        PH_UINT64_DELTA oldDeltas[4];

        block = CONTAINING_RECORD(listEntry, ET_NETWORK_BLOCK, ListEntry);

        memcpy(oldDeltas, block->Deltas, sizeof(block->Deltas));

        PhUpdateDelta(&block->ReceiveDelta, block->ReceiveCount);
        PhUpdateDelta(&block->ReceiveRawDelta, block->ReceiveRaw);
        PhUpdateDelta(&block->SendDelta, block->SendCount);
        PhUpdateDelta(&block->SendRawDelta, block->SendRaw);

        if (memcmp(oldDeltas, block->Deltas, sizeof(block->Deltas)))
        {
            // Values have changed. Invalidate the network node.
            PhAcquireQueuedLockExclusive(&block->TextCacheLock);
            memset(block->TextCacheValid, 0, sizeof(block->TextCacheValid));
            PhReleaseQueuedLockExclusive(&block->TextCacheLock);
        }

        listEntry = listEntry->Flink;
    }
}

VOID EtUpdateProcessInformation(
    VOID
    )
{
    PhAcquireQueuedLockExclusive(&EtpProcessInformationLock);

    if (EtpProcessInformation)
    {
        PhFree(EtpProcessInformation);
        EtpProcessInformation = NULL;
    }

    PhEnumProcesses(&EtpProcessInformation);

    PhReleaseQueuedLockExclusive(&EtpProcessInformationLock);
}

HANDLE EtThreadIdToProcessId(
    _In_ HANDLE ThreadId
    )
{
    PSYSTEM_PROCESS_INFORMATION process;
    ULONG i;
    HANDLE processId;

    PhAcquireQueuedLockShared(&EtpProcessInformationLock);

    if (!EtpProcessInformation)
    {
        PhReleaseQueuedLockShared(&EtpProcessInformationLock);
        return SYSTEM_PROCESS_ID;
    }

    process = PH_FIRST_PROCESS(EtpProcessInformation);

    do
    {
        for (i = 0; i < process->NumberOfThreads; i++)
        {
            if (process->Threads[i].ClientId.UniqueThread == ThreadId)
            {
                processId = process->UniqueProcessId;
                PhReleaseQueuedLockShared(&EtpProcessInformationLock);

                return processId;
            }
        }
    } while (process = PH_NEXT_PROCESS(process));

    PhReleaseQueuedLockShared(&EtpProcessInformationLock);

    return SYSTEM_PROCESS_ID;
}
