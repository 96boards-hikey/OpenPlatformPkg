/** @file

  Copyright (c) 2014, ARM Ltd. All rights reserved.<BR>
  Copyright (c) 2015, Linaro Ltd. All rights reserved.
  Copyright (c) 2015, Hisilicon Ltd. All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

/*
  Implementation of the Android Fastboot Platform protocol, to be used by the
  Fastboot UEFI application, for Hisilicon HiKey platform.
*/

#include <Protocol/AndroidFastbootPlatform.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DiskIo.h>
#include <Protocol/EraseBlock.h>
#include <Protocol/SimpleTextOut.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>

#include <Guid/HiKeyVariable.h>

#define FLASH_DEVICE_PATH_SIZE(DevPath) ( GetDevicePathSize (DevPath) - \
                                            sizeof (EFI_DEVICE_PATH_PROTOCOL))

#define PARTITION_NAME_MAX_LENGTH 72/2

#define IS_ALPHA(Char) (((Char) <= L'z' && (Char) >= L'a') || \
                        ((Char) <= L'Z' && (Char) >= L'Z'))
#define IS_HEXCHAR(Char) (((Char) <= L'9' && (Char) >= L'0') || \
                          IS_ALPHA(Char))

#define SERIAL_NUMBER_LENGTH      16
#define BOOT_DEVICE_LENGTH        16

#define HIKEY_ERASE_SIZE          (16 * 1024 * 1024)
#define HIKEY_ERASE_BLOCKS        (HIKEY_ERASE_SIZE / EFI_PAGE_SIZE)

typedef struct _FASTBOOT_PARTITION_LIST {
  LIST_ENTRY  Link;
  CHAR16      PartitionName[PARTITION_NAME_MAX_LENGTH];
  EFI_HANDLE  PartitionHandle;
  EFI_LBA     StartingLBA;
  EFI_LBA     EndingLBA;
} FASTBOOT_PARTITION_LIST;

STATIC LIST_ENTRY                       mPartitionListHead;
STATIC EFI_HANDLE                       mFlashHandle;
STATIC EFI_BLOCK_IO_PROTOCOL           *mFlashBlockIo;
STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *mTextOut;

/*
  Helper to free the partition list
*/
STATIC
VOID
FreePartitionList (
  VOID
  )
{
  FASTBOOT_PARTITION_LIST *Entry;
  FASTBOOT_PARTITION_LIST *NextEntry;

  Entry = (FASTBOOT_PARTITION_LIST *) GetFirstNode (&mPartitionListHead);
  while (!IsNull (&mPartitionListHead, &Entry->Link)) {
    NextEntry = (FASTBOOT_PARTITION_LIST *) GetNextNode (&mPartitionListHead, &Entry->Link);

    RemoveEntryList (&Entry->Link);
    FreePool (Entry);

    Entry = NextEntry;
  }
}
/*
  Read the PartitionName fields from the GPT partition entries, putting them
  into an allocated array that should later be freed.
*/
STATIC
EFI_STATUS
ReadPartitionEntries (
  IN  EFI_BLOCK_IO_PROTOCOL *BlockIo,
  OUT EFI_PARTITION_ENTRY  **PartitionEntries,
  OUT UINTN                 *PartitionNumbers
  )
{
  EFI_STATUS                  Status;
  UINT32                      MediaId;
  UINTN                       BlockSize;
  UINTN                       PageCount;
  UINTN                       Count, EndLBA;
  EFI_PARTITION_TABLE_HEADER *GptHeader;
  EFI_PARTITION_ENTRY        *Entry;
  VOID                       *Buffer;

  if ((PartitionEntries == NULL) || (PartitionNumbers == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  MediaId = BlockIo->Media->MediaId;
  BlockSize = BlockIo->Media->BlockSize;

  //
  // Read size of Partition entry and number of entries from GPT header
  //

  PageCount = EFI_SIZE_TO_PAGES (34 * BlockSize);
  Buffer = AllocatePages (PageCount);
  if (Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = BlockIo->ReadBlocks (BlockIo, MediaId, 0, PageCount * EFI_PAGE_SIZE, Buffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  GptHeader = (EFI_PARTITION_TABLE_HEADER *)(Buffer + BlockSize);

  // Check there is a GPT on the media
  if (GptHeader->Header.Signature != EFI_PTAB_HEADER_ID ||
      GptHeader->MyLBA != 1) {
    DEBUG ((EFI_D_ERROR,
      "Fastboot platform: No GPT on flash. "
      "Fastboot on Versatile Express does not support MBR.\n"
      ));
    return EFI_DEVICE_ERROR;
  }

  Entry = (EFI_PARTITION_ENTRY *)(Buffer + (2 * BlockSize));
  EndLBA = GptHeader->FirstUsableLBA - 1;
  Count = 0;
  while (1) {
    if ((Entry->StartingLBA > EndLBA) && (Entry->EndingLBA <= GptHeader->LastUsableLBA)) {
      Count++;
      EndLBA = Entry->EndingLBA;
      Entry++;
    } else {
      break;
    }
  }
  if (Count == 0) {
    return EFI_INVALID_PARAMETER;
  }
  if (Count > GptHeader->NumberOfPartitionEntries) {
    Count = GptHeader->NumberOfPartitionEntries;
  }

  *PartitionEntries = (EFI_PARTITION_ENTRY *)((UINTN)Buffer + (2 * BlockSize));
  *PartitionNumbers = Count;
  return EFI_SUCCESS;
}


EFI_STATUS
LoadPtable (
  VOID
  )
{
  EFI_STATUS                          Status;
  EFI_DEVICE_PATH_PROTOCOL           *FlashDevicePath;
  EFI_DEVICE_PATH_PROTOCOL           *FlashDevicePathDup;
  UINTN                               PartitionNumbers;
  UINTN                               LoopIndex;
  EFI_PARTITION_ENTRY                *PartitionEntries;
  FASTBOOT_PARTITION_LIST            *Entry;

  InitializeListHead (&mPartitionListHead);

  Status = gBS->LocateProtocol (&gEfiSimpleTextOutProtocolGuid, NULL, (VOID **) &mTextOut);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
      "Fastboot platform: Couldn't open Text Output Protocol: %r\n", Status
      ));
    return Status;
  }

  //
  // Get EFI_HANDLES for all the partitions on the block devices pointed to by
  // PcdFastbootFlashDevicePath, also saving their GPT partition labels.
  // There's no way to find all of a device's children, so we get every handle
  // in the system supporting EFI_BLOCK_IO_PROTOCOL and then filter out ones
  // that don't represent partitions on the flash device.
  //
  FlashDevicePath = ConvertTextToDevicePath ((CHAR16*)FixedPcdGetPtr (PcdAndroidFastbootNvmDevicePath));

  // Create another device path pointer because LocateDevicePath will modify it.
  FlashDevicePathDup = FlashDevicePath;
  Status = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &FlashDevicePathDup, &mFlashHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Warning: Couldn't locate Android NVM device (status: %r)\n", Status));
    // Failing to locate partitions should not prevent to do other Android FastBoot actions
    return EFI_SUCCESS;
  }


  Status = gBS->OpenProtocol (
                  mFlashHandle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **) &mFlashBlockIo,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Fastboot platform: Couldn't open Android NVM device (status: %r)\n", Status));
    return EFI_DEVICE_ERROR;
  }

  // Read the GPT partition entry array into memory so we can get the partition names
  Status = ReadPartitionEntries (mFlashBlockIo, &PartitionEntries, &PartitionNumbers);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Warning: Failed to read partitions from Android NVM device (status: %r)\n", Status));
    // Failing to locate partitions should not prevent to do other Android FastBoot actions
    return EFI_SUCCESS;
  }
  for (LoopIndex = 0; LoopIndex < PartitionNumbers; LoopIndex++) {
    // Create entry
    Entry = AllocatePool (sizeof (FASTBOOT_PARTITION_LIST));
    if (Entry == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      FreePartitionList ();
      goto Exit;
    }
    StrnCpy (
      Entry->PartitionName,
      PartitionEntries[LoopIndex].PartitionName,
      PARTITION_NAME_MAX_LENGTH
      );
    Entry->StartingLBA = PartitionEntries[LoopIndex].StartingLBA;
    Entry->EndingLBA = PartitionEntries[LoopIndex].EndingLBA;
    InsertTailList (&mPartitionListHead, &Entry->Link);
  }
Exit:
  FreePages (
    (VOID *)((UINTN)PartitionEntries - (2 * EFI_PAGE_SIZE)),
    EFI_SIZE_TO_PAGES (34 * mFlashBlockIo->Media->BlockSize)
    );
  return Status;
}


/*
  Initialise: Open the Android NVM device and find the partitions on it. Save them in
  a list along with the "PartitionName" fields for their GPT entries.
  We will use these partition names as the key in
  HiKeyFastbootPlatformFlashPartition.
*/
EFI_STATUS
HiKeyFastbootPlatformInit (
  VOID
  )
{
  EFI_STATUS               Status;

  Status = LoadPtable ();
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return Status;
}

VOID
HiKeyFastbootPlatformUnInit (
  VOID
  )
{
  FreePartitionList ();
}

EFI_STATUS
HiKeyFlashPtable (
  IN UINTN   Size,
  IN VOID   *Image
  )
{
  EFI_STATUS               Status;
  VOID                    *Buffer;
  UINTN                    BlockSize;
  EFI_DISK_IO_PROTOCOL    *DiskIo;
  UINT32                   EntrySize, EntryOffset;
  UINT32                   MediaId;

  MediaId = mFlashBlockIo->Media->MediaId;
  BlockSize = mFlashBlockIo->Media->BlockSize;
  Status = gBS->OpenProtocol (
                  mFlashHandle,
                  &gEfiDiskIoProtocolGuid,
                  (VOID **) &DiskIo,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Buffer = Image;
  if (AsciiStrnCmp (Buffer, "ENTRYHDR", 8) != 0) {
    DEBUG ((EFI_D_ERROR, "unknown ptable image\n"));
    return EFI_UNSUPPORTED;
  }
  Buffer += 8;
  if (AsciiStrnCmp (Buffer, "primary", 7) != 0) {
    DEBUG ((EFI_D_ERROR, "unknown ptable imag\n"));
    return EFI_UNSUPPORTED;
  }
  Buffer += 8;
  EntryOffset = *(UINT32 *)Buffer * BlockSize;
  Buffer += 4;
  EntrySize = *(UINT32 *)Buffer * BlockSize;
  if ((EntrySize + BlockSize) > Size) {
    DEBUG ((DEBUG_ERROR, "Entry size doesn't match\n"));
    return EFI_UNSUPPORTED;
  }
  Buffer = Image + BlockSize;
  Status = DiskIo->WriteDisk (DiskIo, MediaId, EntryOffset, EntrySize, Buffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  FreePartitionList ();
  Status = LoadPtable ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Buffer = Image + 16 + 12;
  if (AsciiStrnCmp (Buffer, "ENTRYHDR", 8) != 0) {
    return Status;
  }
  Buffer += 8;
  if (AsciiStrnCmp (Buffer, "second", 6) != 0) {
    return Status;
  }
  Buffer += 8;
  EntryOffset = *(UINT32 *)Buffer * BlockSize;
  Buffer += 4;
  EntrySize = *(UINT32 *)Buffer * BlockSize;
  if ((EntrySize + BlockSize) > Size) {
    DEBUG ((DEBUG_ERROR, "Entry size doens't match\n"));
    return EFI_UNSUPPORTED;
  }
  Buffer = Image + BlockSize;
  Status = DiskIo->WriteDisk (DiskIo, MediaId, EntryOffset, EntrySize, Buffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  mFlashBlockIo->FlushBlocks (mFlashBlockIo);
  MicroSecondDelay (50000);
  return Status;
}

EFI_STATUS
HiKeyFastbootPlatformFlashPartition (
  IN CHAR8  *PartitionName,
  IN UINTN   Size,
  IN VOID   *Image
  )
{
  EFI_STATUS               Status;
  UINTN                    PartitionSize;
  FASTBOOT_PARTITION_LIST *Entry;
  CHAR16                   PartitionNameUnicode[60];
  BOOLEAN                  PartitionFound;
  EFI_DISK_IO_PROTOCOL    *DiskIo;
  UINTN                    BlockSize;

  // Support the pseudo partition name, such as "ptable".
  if (AsciiStrCmp (PartitionName, "ptable") == 0) {
    return HiKeyFlashPtable (Size, Image);
  }

  AsciiStrToUnicodeStr (PartitionName, PartitionNameUnicode);
  PartitionFound = FALSE;
  Entry = (FASTBOOT_PARTITION_LIST *) GetFirstNode (&(mPartitionListHead));
  while (!IsNull (&mPartitionListHead, &Entry->Link)) {
    // Search the partition list for the partition named by PartitionName
    if (StrCmp (Entry->PartitionName, PartitionNameUnicode) == 0) {
      PartitionFound = TRUE;
      break;
    }

   Entry = (FASTBOOT_PARTITION_LIST *) GetNextNode (&mPartitionListHead, &(Entry)->Link);
  }
  if (!PartitionFound) {
    return EFI_NOT_FOUND;
  }

  // Check image will fit on device
  BlockSize = mFlashBlockIo->Media->BlockSize;
  PartitionSize = (Entry->EndingLBA - Entry->StartingLBA + 1) * BlockSize;
  if (PartitionSize < Size) {
    DEBUG ((DEBUG_ERROR, "Partition not big enough.\n"));
    DEBUG ((DEBUG_ERROR, "Partition Size:\t%ld\nImage Size:\t%ld\n", PartitionSize, Size));

    return EFI_VOLUME_FULL;
  }

  Status = gBS->OpenProtocol (
                  mFlashHandle,
                  &gEfiDiskIoProtocolGuid,
                  (VOID **) &DiskIo,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  ASSERT_EFI_ERROR (Status);

  Status = DiskIo->WriteDisk (
                     DiskIo,
                     mFlashBlockIo->Media->MediaId,
                     Entry->StartingLBA * BlockSize,
                     Size,
                     Image
                     );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to write %d bytes into 0x%x, Status:%r\n", Size, Entry->StartingLBA * BlockSize, Status));
    return Status;
  }

  mFlashBlockIo->FlushBlocks(mFlashBlockIo);
  MicroSecondDelay (50000);

  return Status;
}

EFI_STATUS
HiKeyErasePtable (
  VOID
  )
{
  EFI_STATUS                  Status;
  EFI_ERASE_BLOCK_PROTOCOL   *EraseBlockProtocol;

  Status = gBS->OpenProtocol (
                  mFlashHandle,
                  &gEfiEraseBlockProtocolGuid,
                  (VOID **) &EraseBlockProtocol,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Fastboot platform: could not open Erase Block IO: %r\n", Status));
    return EFI_DEVICE_ERROR;
  }
  Status = EraseBlockProtocol->EraseBlocks (
                                 mFlashBlockIo,
                                 mFlashBlockIo->Media->MediaId,
                                 0,
                                 NULL,
                                 34 * mFlashBlockIo->Media->BlockSize
                                 );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  FreePartitionList ();
  return Status;
}

EFI_STATUS
HiKeyFastbootPlatformErasePartition (
  IN CHAR8 *PartitionName
  )
{
  EFI_STATUS                  Status;
  EFI_ERASE_BLOCK_PROTOCOL   *EraseBlockProtocol;
  UINTN                       Size;
  BOOLEAN                     PartitionFound;
  CHAR16                      PartitionNameUnicode[60];
  FASTBOOT_PARTITION_LIST    *Entry;

  AsciiStrToUnicodeStr (PartitionName, PartitionNameUnicode);

  // Support the pseudo partition name, such as "ptable".
  if (AsciiStrCmp (PartitionName, "ptable") == 0) {
    return HiKeyErasePtable ();
  }

  PartitionFound = FALSE;
  Entry = (FASTBOOT_PARTITION_LIST *) GetFirstNode (&mPartitionListHead);
  while (!IsNull (&mPartitionListHead, &Entry->Link)) {
    // Search the partition list for the partition named by PartitionName
    if (StrCmp (Entry->PartitionName, PartitionNameUnicode) == 0) {
      PartitionFound = TRUE;
      break;
    }
    Entry = (FASTBOOT_PARTITION_LIST *) GetNextNode (&mPartitionListHead, &Entry->Link);
  }
  if (!PartitionFound) {
    return EFI_NOT_FOUND;
  }

  Status = gBS->OpenProtocol (
                  mFlashHandle,
                  &gEfiEraseBlockProtocolGuid,
                  (VOID **) &EraseBlockProtocol,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Size = (Entry->EndingLBA - Entry->StartingLBA + 1) * mFlashBlockIo->Media->BlockSize;
  Status = EraseBlockProtocol->EraseBlocks (
                                 mFlashBlockIo,
                                 mFlashBlockIo->Media->MediaId,
                                 Entry->StartingLBA,
                                 NULL,
                                 Size
                                 );
  return Status;
}

EFI_STATUS
HiKeyFastbootPlatformGetVar (
  IN  CHAR8   *Name,
  OUT CHAR8   *Value
  )
{
  EFI_STATUS               Status;
  EFI_BLOCK_IO_PROTOCOL   *BlockIo;
  UINT64                   PartitionSize;
  FASTBOOT_PARTITION_LIST *Entry;
  CHAR16                   PartitionNameUnicode[60];
  BOOLEAN                  PartitionFound;
  CHAR16                   DataUnicode[17];
  UINTN                    VariableSize;

  if (!AsciiStrCmp (Name, "max-download-size")) {
    AsciiStrCpy (Value, FixedPcdGetPtr (PcdArmFastbootFlashLimit));
  } else if (!AsciiStrCmp (Name, "product")) {
    AsciiStrCpy (Value, FixedPcdGetPtr (PcdFirmwareVendor));
  } else if (!AsciiStrCmp (Name, "serialno")) {
    VariableSize = 17 * sizeof (CHAR16);
    Status = gRT->GetVariable (
                    (CHAR16 *)L"SerialNo",
                    &gHiKeyVariableGuid,
                    NULL,
                    &VariableSize,
                    &DataUnicode
                    );
    if (EFI_ERROR (Status)) {
      *Value = '\0';
      return EFI_NOT_FOUND;
    }
    DataUnicode[(VariableSize / sizeof(CHAR16)) - 1] = '\0';
    UnicodeStrToAsciiStr (DataUnicode, Value);
  } else if ( !AsciiStrnCmp (Name, "partition-size", 14)) {
    AsciiStrToUnicodeStr ((Name + 15), PartitionNameUnicode);
    PartitionFound = FALSE;
    Entry = (FASTBOOT_PARTITION_LIST *) GetFirstNode (&(mPartitionListHead));
    while (!IsNull (&mPartitionListHead, &Entry->Link)) {
      // Search the partition list for the partition named by PartitionName
      if (StrCmp (Entry->PartitionName, PartitionNameUnicode) == 0) {
        PartitionFound = TRUE;
        break;
      }

     Entry = (FASTBOOT_PARTITION_LIST *) GetNextNode (&mPartitionListHead, &(Entry)->Link);
    }
    if (!PartitionFound) {
      *Value = '\0';
      return EFI_NOT_FOUND;
    }

    Status = gBS->OpenProtocol (
                    Entry->PartitionHandle,
                    &gEfiBlockIoProtocolGuid,
                    (VOID **) &BlockIo,
                    gImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "Fastboot platform: couldn't open Block IO for flash: %r\n", Status));
      *Value = '\0';
      return EFI_NOT_FOUND;
    }

    PartitionSize = (BlockIo->Media->LastBlock + 1) * BlockIo->Media->BlockSize;
    DEBUG ((EFI_D_ERROR, "Fastboot platform: check for partition-size:%a 0X%llx\n", Name, PartitionSize ));
    AsciiSPrint (Value, 12, "0x%llx", PartitionSize);
  } else if ( !AsciiStrnCmp (Name, "partition-type", 14)) {
      DEBUG ((EFI_D_ERROR, "Fastboot platform: check for partition-type:%a\n", (Name + 15) ));
    if ( !AsciiStrnCmp  ( (Name + 15) , "system", 6) || !AsciiStrnCmp  ( (Name + 15) , "userdata", 8)
            || !AsciiStrnCmp  ( (Name + 15) , "cache", 5)) {
      AsciiStrCpy (Value, "ext4");
    } else {
      AsciiStrCpy (Value, "raw");
    }
  } else {
    *Value = '\0';
  }
  return EFI_SUCCESS;
}

EFI_STATUS
HiKeyFastbootPlatformOemCommand (
  IN  CHAR8   *Command
  )
{
  CHAR16     CommandUnicode[65];
  UINTN      Index = 0, VariableSize;
  UINT16     AutoBoot, Data;
  EFI_STATUS Status;

  if (AsciiStrCmp (Command, "Demonstrate") == 0) {
    DEBUG ((EFI_D_ERROR, "ARM OEM Fastboot command 'Demonstrate' received.\n"));
    return EFI_SUCCESS;
  } else if (AsciiStrnCmp (Command, "autoboot", AsciiStrLen ("autoboot")) == 0) {
    Index += sizeof ("autoboot");
    while (TRUE) {
      if (Command[Index] == '\0')
        goto out;
      else if (Command[Index] == ' ')
        Index++;
      else
        break;
    }
    Data = AsciiStrDecimalToUintn (Command + Index);

    VariableSize = sizeof (UINT16);
    Status = gRT->GetVariable (
                    (CHAR16 *)L"HiKeyAutoBoot",
                    &gHiKeyVariableGuid,
                    NULL,
                    &VariableSize,
                    &AutoBoot
                    );
    if ((EFI_ERROR (Status) == 0) && (AutoBoot == Data)) {
      return EFI_SUCCESS;
    }
    AutoBoot = Data;
    Status = gRT->SetVariable (
                    (CHAR16*)L"HiKeyAutoBoot",
                    &gHiKeyVariableGuid,
                    EFI_VARIABLE_NON_VOLATILE       |
                    EFI_VARIABLE_BOOTSERVICE_ACCESS |
                    EFI_VARIABLE_RUNTIME_ACCESS,
                    sizeof (UINT16),
                    &AutoBoot
                    );
    return Status;
  } else {
    AsciiStrToUnicodeStr (Command + Index, CommandUnicode);
    DEBUG ((EFI_D_ERROR,
      "HiKey: Unrecognised Fastboot OEM command: %s\n",
      CommandUnicode
      ));
    return EFI_NOT_FOUND;
  }
out:
  return EFI_NOT_FOUND;
}

EFI_STATUS
HiKeyFastbootPlatformFlashPartitionEx (
  IN CHAR8  *PartitionName,
  IN UINTN   Offset,
  IN UINTN   Size,
  IN VOID   *Image
  )
{
  EFI_STATUS               Status;
  UINTN                    PartitionSize;
  FASTBOOT_PARTITION_LIST *Entry;
  CHAR16                   PartitionNameUnicode[60];
  BOOLEAN                  PartitionFound;
  UINTN                    BlockSize;
  EFI_DISK_IO_PROTOCOL    *DiskIo;

  AsciiStrToUnicodeStr (PartitionName, PartitionNameUnicode);
  PartitionFound = FALSE;
  Entry = (FASTBOOT_PARTITION_LIST *) GetFirstNode (&(mPartitionListHead));
  while (!IsNull (&mPartitionListHead, &Entry->Link)) {
    // Search the partition list for the partition named by PartitionName
    if (StrCmp (Entry->PartitionName, PartitionNameUnicode) == 0) {
      PartitionFound = TRUE;
      break;
    }

   Entry = (FASTBOOT_PARTITION_LIST *) GetNextNode (&mPartitionListHead, &(Entry)->Link);
  }
  if (!PartitionFound) {
    return EFI_NOT_FOUND;
  }

  // Check image will fit on device
  PartitionSize = (Entry->EndingLBA - Entry->StartingLBA + 1) * mFlashBlockIo->Media->BlockSize;
  if (PartitionSize < Size) {
    DEBUG ((DEBUG_ERROR, "Partition not big enough.\n"));
    DEBUG ((DEBUG_ERROR, "Partition Size:\t%ld\nImage Size:\t%ld\n", PartitionSize, Size));

    return EFI_VOLUME_FULL;
  }

  BlockSize = mFlashBlockIo->Media->BlockSize;

  Status = gBS->OpenProtocol (
                  mFlashHandle,
                  &gEfiDiskIoProtocolGuid,
                  (VOID **) &DiskIo,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = DiskIo->WriteDisk (
                     DiskIo,
                     mFlashBlockIo->Media->MediaId,
                     Entry->StartingLBA * BlockSize + Offset,
                     Size,
                     Image
                     );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to write %d bytes into 0x%x, Status:%r\n", Size, Entry->StartingLBA * BlockSize + Offset, Status));
    return Status;
  }
  return Status;
}

FASTBOOT_PLATFORM_PROTOCOL mPlatformProtocol = {
  HiKeyFastbootPlatformInit,
  HiKeyFastbootPlatformUnInit,
  HiKeyFastbootPlatformFlashPartition,
  HiKeyFastbootPlatformErasePartition,
  HiKeyFastbootPlatformGetVar,
  HiKeyFastbootPlatformOemCommand,
  HiKeyFastbootPlatformFlashPartitionEx
};

EFI_STATUS
EFIAPI
HiKeyFastbootPlatformEntryPoint (
  IN EFI_HANDLE                            ImageHandle,
  IN EFI_SYSTEM_TABLE                      *SystemTable
  )
{
  return gBS->InstallProtocolInterface (
                &ImageHandle,
                &gAndroidFastbootPlatformProtocolGuid,
                EFI_NATIVE_INTERFACE,
                &mPlatformProtocol
                );
}
