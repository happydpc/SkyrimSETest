#include "../../../tbb2018/concurrent_hash_map.h"
#include "../../common.h"
#include "BSTScatterTable.h"
#include "BSReadWriteLock.h"
#include "TESForm.h"
#include "BGSDistantTreeBlock.h"

AutoPtr(BSReadWriteLock, GlobalFormLock, 0x1EEA0D0);
AutoPtr(templated(BSTCRCScatterTable<uint32_t, TESForm *> *), GlobalFormList, 0x1EE9C38);

tbb::concurrent_hash_map<uint32_t, TESForm *> g_FormMap[TES_FORM_MASTER_COUNT];

namespace Bitmap
{
    const uint32_t EntrySize  = (TES_FORM_INDEX_COUNT / 32) * sizeof(LONG);
    const uint32_t EntryCount = TES_FORM_MASTER_COUNT;

	alignas(64) LONG *FormBitmap[EntryCount];

	uintptr_t MemorySize;
	uintptr_t MemoryBase;
	uintptr_t MemoryEnd;

	LONG CALLBACK PageFaultExceptionFilter(LPEXCEPTION_POINTERS Info)
	{
		// Check if the violation was inside our memory region
		uintptr_t code = Info->ExceptionRecord->ExceptionCode;
		uintptr_t addr = Info->ExceptionRecord->ExceptionInformation[1];

		if (code != EXCEPTION_ACCESS_VIOLATION)
			return EXCEPTION_CONTINUE_SEARCH;

		if (addr < MemoryBase || addr > MemoryEnd)
			return EXCEPTION_CONTINUE_SEARCH;

		// Lazy-allocate the bitmap when needed (128KB [aligned] chunks, prevent committing unused pages)
		uintptr_t sliceSize = 128 * 1024;
		uintptr_t sliceBase = (addr / sliceSize) * sliceSize;

		sliceBase = max(sliceBase, MemoryBase);
		sliceSize = min(sliceSize, MemoryEnd - sliceBase);

		ProfileCounterAdd("Cache Bytes Utilized", sliceSize);

		VirtualAlloc((LPVOID)sliceBase, sliceSize, MEM_COMMIT, PAGE_READWRITE);
		return EXCEPTION_CONTINUE_EXECUTION;
	}

    void TryInitRange()
    {
        MemorySize = EntryCount * EntrySize;
        MemoryBase = (uintptr_t)VirtualAlloc(nullptr, MemorySize, MEM_RESERVE, PAGE_NOACCESS);
		MemoryEnd  = MemoryBase + MemorySize - 1;

        for (int i = 0; i < EntryCount; i++)
            FormBitmap[i] = (LONG *)(MemoryBase + (i * EntrySize));

		AddVectoredExceptionHandler(0, PageFaultExceptionFilter);
    }

	void SetNull(uint32_t MasterId, uint32_t BaseId, bool Unset)
    {
        if (!ui::opt::EnableCacheBitmap || MasterId >= EntryCount)
            return;

		if (Unset)
			_interlockedbittestandreset(&FormBitmap[MasterId][BaseId / 32], BaseId % 32);
		else
			_interlockedbittestandset(&FormBitmap[MasterId][BaseId / 32], BaseId % 32);
    }

	bool IsNull(uint32_t MasterId, uint32_t BaseId)
    {
        if (!ui::opt::EnableCacheBitmap || MasterId >= EntryCount)
            return false;

        // If bit is set, return true
        return _bittest(&FormBitmap[MasterId][BaseId / 32], BaseId % 32) != 0;
    }
}

void UpdateFormCache(uint32_t FormId, TESForm *Value, bool Invalidate)
{
    ProfileTimer("Cache Update Time");

    const unsigned char masterId = (FormId & 0xFF000000) >> 24;
	const unsigned int baseId	 = (FormId & 0x00FFFFFF);

    // NOTE: If the pointer is 0 we can short-circuit and skip the entire
    // hash map. Any fetches will check the bitmap first and also skip the map.
    if (!Value && !Invalidate)
    {
        // Atomic write can be outside of the lock
        Bitmap::SetNull(masterId, baseId, false);
    }
	else
	{
		if (Invalidate)
			Bitmap::SetNull(masterId, baseId, true);

		if (Invalidate)
			g_FormMap[masterId].erase(baseId);
		else
			g_FormMap[masterId].insert(std::make_pair(baseId, Value));
	}

	BGSDistantTreeBlock::InvalidateCachedForm(FormId);
}

bool GetFormCache(uint32_t FormId, TESForm *&Form)
{
	if (!ui::opt::EnableCache)
		return false;

    ProfileCounterInc("Cache Lookups");
    ProfileTimer("Cache Fetch Time");

    const unsigned char masterId = (FormId & 0xFF000000) >> 24;
    const unsigned int baseId    = (FormId & 0x00FFFFFF);
    bool hit;

    // Check if the bitmap says this is a nullptr
    if (Bitmap::IsNull(masterId, baseId))
    {
        hit  = true;
        Form = nullptr;

        ProfileCounterInc("Null Fetches");
    }
	else
	{
		// Try a hash map lookup instead
		tbb::concurrent_hash_map<uint32_t, TESForm *>::accessor accessor;

		if (g_FormMap[masterId].find(accessor, baseId))
		{
			hit = true;
			Form = accessor->second;
		}
		else
		{
			// Total cache miss, worst case scenario
			hit = false;
			Form = nullptr;

			ProfileCounterInc("Cache Misses");
		}
	}

    return hit;
}

void CRC32_Lazy(int *out, int idIn)
{
    ((void (*)(int *, int))(g_ModuleBase + 0xC06030))(out, idIn);
}

TESForm *GetFormById(unsigned int FormId)
{
    // Hybrid bitmap/std::unordered_map cache
	TESForm *formPointer;

	if (GetFormCache(FormId, formPointer))
        return formPointer;

    // Try to use Bethesda's scatter table which is considerably slower
	GlobalFormLock.LockForRead();

	if (!GlobalFormList || !GlobalFormList->get(FormId, formPointer))
		formPointer = nullptr;

	GlobalFormLock.UnlockRead();

    UpdateFormCache(FormId, formPointer, false);
    return formPointer;
}

uint8_t *origFunc3 = nullptr;
__int64 UnknownFormFunction3(__int64 a1, __int64 a2, int a3, __int64 a4)
{
    UpdateFormCache(*(uint32_t *)a4, nullptr, true);

    return ((decltype(&UnknownFormFunction3))origFunc3)(a1, a2, a3, a4);
}

uint8_t *origFunc2 = nullptr;
__int64 UnknownFormFunction2(__int64 a1, __int64 a2, int a3, DWORD *formId, __int64 **a5)
{
    UpdateFormCache(*formId, nullptr, true);

    return ((decltype(&UnknownFormFunction2))origFunc2)(a1, a2, a3, formId, a5);
}

uint8_t *origFunc1 = nullptr;
__int64 UnknownFormFunction1(__int64 a1, __int64 a2, int a3, DWORD *formId, __int64 *a5)
{
    UpdateFormCache(*formId, nullptr, true);

    return ((decltype(&UnknownFormFunction1))origFunc1)(a1, a2, a3, formId, a5);
}

uint8_t *origFunc0 = nullptr;
void UnknownFormFunction0(__int64 form, bool a2)
{
    UpdateFormCache(*(uint32_t *)(form + 0x14), nullptr, true);

	((decltype(&UnknownFormFunction0))origFunc0)(form, a2);
}

TESForm *TESForm::LookupFormById(uint32_t FormId)
{
	return GetFormById(FormId);
}

void PatchTESForm()
{
    Bitmap::TryInitRange();

	origFunc0 = Detours::X64::DetourFunctionClass((PBYTE)(g_ModuleBase + 0x194970), &UnknownFormFunction0);
	origFunc1 = Detours::X64::DetourFunctionClass((PBYTE)(g_ModuleBase + 0x196070), &UnknownFormFunction1);
	origFunc2 = Detours::X64::DetourFunctionClass((PBYTE)(g_ModuleBase + 0x195DA0), &UnknownFormFunction2);
	origFunc3 = Detours::X64::DetourFunctionClass((PBYTE)(g_ModuleBase + 0x196960), &UnknownFormFunction3);
    Detours::X64::DetourFunctionClass((PBYTE)(g_ModuleBase + 0x1943B0), &GetFormById);
}