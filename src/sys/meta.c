/**
 * @file sys/meta.c
 *
 * @copyright 2015 Bill Zissimopoulos
 */

#include <sys/driver.h>

typedef struct _FSP_META_CACHE_ITEM
{
    LIST_ENTRY ListEntry;
    struct _FSP_META_CACHE_ITEM *DictNext;
    PVOID ItemBuffer;
    UINT64 ItemIndex;
    UINT64 ExpirationTime;
    LONG RefCount;
} FSP_META_CACHE_ITEM;

typedef struct
{
    PVOID Item;
    ULONG Size;
    __declspec(align(MEMORY_ALLOCATION_ALIGNMENT)) UINT8 Buffer[];
} FSP_META_CACHE_ITEM_BUFFER;

static inline
VOID MetaCacheDereferenceItem(FSP_META_CACHE_ITEM *Item)
{
    LONG RefCount = InterlockedDecrement(&Item->RefCount);
    if (0 == RefCount)
    {
        /* if we ever need to add a finalizer for meta items it should go here */
        FspFree(Item->ItemBuffer);
        FspFree(Item);
    }
}

NTSTATUS MetaCacheCreate(
    ULONG MetaCapacity, ULONG ItemSizeMax, PLARGE_INTEGER MetaTimeout,
    FSP_META_CACHE **PMetaCache)
{
    *PMetaCache = 0;

    FSP_META_CACHE *MetaCache;
    ULONG BucketCount = (PAGE_SIZE - sizeof *MetaCache) / sizeof MetaCache->ItemBuckets[0];
    MetaCache = FspAllocNonPaged(PAGE_SIZE);
    if (0 == MetaCache)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(MetaCache, PAGE_SIZE);

    KeInitializeSpinLock(&MetaCache->SpinLock);
    InitializeListHead(&MetaCache->ItemList);
    MetaCache->MetaCapacity = MetaCapacity;
    MetaCache->ItemSizeMax = ItemSizeMax;
    MetaCache->MetaTimeout = MetaTimeout->QuadPart;
    MetaCache->ItemBucketCount = BucketCount;

    *PMetaCache = MetaCache;

    return STATUS_SUCCESS;
}

VOID MetaCacheDelete(FSP_META_CACHE *MetaCache)
{
    MetaCacheInvalidateAll(MetaCache);
    FspFree(MetaCache);
}

static VOID MetaCacheInvalidateItems(FSP_META_CACHE *MetaCache, UINT64 ExpirationTime)
{
    FSP_META_CACHE_ITEM *Item;
    PLIST_ENTRY Head, Entry;
    ULONG HashIndex;
    KIRQL Irql;

    for (;;)
    {
        KeAcquireSpinLock(&MetaCache->SpinLock, &Irql);
        Item = 0;
        Head = &MetaCache->ItemList;
        Entry = Head->Flink;
        if (Head != Entry)
        {
            Item = CONTAINING_RECORD(Entry, FSP_META_CACHE_ITEM, ListEntry);
            if (Item->ExpirationTime <= ExpirationTime)
            {
                HashIndex = Item->ItemIndex % MetaCache->ItemBucketCount;
                for (FSP_META_CACHE_ITEM **P = (PVOID)&MetaCache->ItemBuckets[HashIndex];
                    *P; P = &(*P)->DictNext)
                    if (*P == Item)
                    {
                        *P = (*P)->DictNext;
                        break;
                    }
                RemoveEntryList(&Item->ListEntry);
                MetaCache->ItemCount--;
            }
            else
                Item = 0;
        }
        KeReleaseSpinLock(&MetaCache->SpinLock, Irql);

        if (0 == Item)
            break;

        MetaCacheDereferenceItem(Item);
    }
}

VOID MetaCacheInvalidateAll(FSP_META_CACHE *MetaCache)
{
    MetaCacheInvalidateItems(MetaCache, (UINT64)-1LL);
}

VOID MetaCacheInvalidateExpired(FSP_META_CACHE *MetaCache)
{
    MetaCacheInvalidateItems(MetaCache, KeQueryInterruptTime());
}

PVOID MetaCacheReferenceItemBuffer(FSP_META_CACHE *MetaCache, UINT64 ItemIndex, PULONG PSize)
{
    FSP_META_CACHE_ITEM *Item = 0;
    FSP_META_CACHE_ITEM_BUFFER *ItemBuffer;
    ULONG HashIndex;
    KIRQL Irql;

    KeAcquireSpinLock(&MetaCache->SpinLock, &Irql);
    HashIndex = ItemIndex % MetaCache->ItemBucketCount;
    for (FSP_META_CACHE_ITEM *ItemX = MetaCache->ItemBuckets[HashIndex]; ItemX; ItemX = ItemX->DictNext)
        if (ItemX->ItemIndex == ItemIndex)
        {
            Item = ItemX;
            InterlockedIncrement(&Item->RefCount);
            break;
        }
    KeReleaseSpinLock(&MetaCache->SpinLock, Irql);

    if (0 == Item)
    {
        if (0 != PSize)
            *PSize = 0;
        return 0;
    }

    ItemBuffer = Item->ItemBuffer;
    if (0 != PSize)
        *PSize = ItemBuffer->Size;
    return ItemBuffer->Buffer;
}

VOID MetaCacheDereferenceItemBuffer(PVOID Buffer)
{
    FSP_META_CACHE_ITEM_BUFFER *ItemBuffer = (PVOID)((PUINT8)Buffer - sizeof *ItemBuffer);
    MetaCacheDereferenceItem(ItemBuffer->Item);
}

UINT64 MetaCacheAddItem(FSP_META_CACHE *MetaCache, PVOID Buffer, ULONG Size)
{
    FSP_META_CACHE_ITEM *Item;
    FSP_META_CACHE_ITEM_BUFFER *ItemBuffer;
    UINT64 ItemIndex = 0;
    ULONG HashIndex;
    BOOLEAN HasCapacity;
    KIRQL Irql;

    if (sizeof *ItemBuffer + Size > MetaCache->ItemSizeMax)
        return 0;

    Item = FspAllocNonPaged(sizeof *Item);
    if (0 == Item)
        return 0;

    ItemBuffer = FspAlloc(sizeof *ItemBuffer + Size);
    if (0 == ItemBuffer)
    {
        FspFree(Item);
        return 0;
    }

    RtlZeroMemory(Item, sizeof *Item);
    Item->ItemBuffer = ItemBuffer;
    Item->ExpirationTime = KeQueryInterruptTime() + MetaCache->MetaTimeout;
    Item->RefCount = 1;

    RtlZeroMemory(ItemBuffer, sizeof *ItemBuffer);
    ItemBuffer->Item = Item;
    ItemBuffer->Size = Size;
    RtlCopyMemory(ItemBuffer->Buffer, Buffer, Size);

    KeAcquireSpinLock(&MetaCache->SpinLock, &Irql);
    HasCapacity = MetaCache->ItemCount < MetaCache->MetaCapacity;
    if (HasCapacity)
    {
        ItemIndex = MetaCache->ItemIndex;
        ItemIndex = (UINT64)-1LL == ItemIndex ? 1 : ItemIndex + 1;
        MetaCache->ItemIndex = ItemIndex;
        Item->ItemIndex = ItemIndex;

        MetaCache->ItemCount++;
        InsertTailList(&MetaCache->ItemList, &Item->ListEntry);
        HashIndex = ItemIndex % MetaCache->ItemBucketCount;
#if DBG
        for (FSP_META_CACHE_ITEM *ItemX = MetaCache->ItemBuckets[HashIndex]; ItemX; ItemX = ItemX->DictNext)
            ASSERT(ItemX->ItemIndex != ItemIndex);
#endif
        Item->DictNext = MetaCache->ItemBuckets[HashIndex];
        MetaCache->ItemBuckets[HashIndex] = Item;
    }
    KeReleaseSpinLock(&MetaCache->SpinLock, Irql);

    if (!HasCapacity)
    {
        FspFree(ItemBuffer);
        FspFree(Item);
    }

    return ItemIndex;
}

VOID MetaCacheInvalidateItem(FSP_META_CACHE *MetaCache, UINT64 ItemIndex)
{
    FSP_META_CACHE_ITEM *Item = 0;
    ULONG HashIndex;
    KIRQL Irql;

    KeAcquireSpinLock(&MetaCache->SpinLock, &Irql);
    HashIndex = ItemIndex % MetaCache->ItemBucketCount;
    for (FSP_META_CACHE_ITEM **P = (PVOID)&MetaCache->ItemBuckets[HashIndex]; *P; P = &(*P)->DictNext)
        if ((*P)->ItemIndex == ItemIndex)
        {
            Item = *P;
            *P = (*P)->DictNext;
            RemoveEntryList(&Item->ListEntry);
            MetaCache->ItemCount--;
            break;
        }
    KeReleaseSpinLock(&MetaCache->SpinLock, Irql);

    if (0 != Item)
        MetaCacheDereferenceItem(Item);
}