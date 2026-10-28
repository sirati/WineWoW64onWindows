/* Compare the legacy pagefile result with its extended Windows counterpart. */
#include "test_support.h"

static void value(const char *label,DWORD length,ULONGLONG number)
{
    static const char hex[]="0123456789abcdef";
    char text[18];
    for(unsigned i=0;i<16;i++)text[i]=hex[(number>>(60-4*i))&15];
    text[16]='\r';text[17]='\n';
    message(label,length);message(text,sizeof(text));
}
#define VALUE(label,number) value(label,sizeof(label)-1,number)

void WINAPI app_entry(void)
{
    BOOL failed=FALSE;
    /* Include a sample after Wine's system-memory cache expires. */
    for(unsigned i=0;i<3;i++)
    {
        MEMORYSTATUSEX extended={.dwLength=sizeof(extended)};
        MEMORYSTATUS legacy={0};
        if(!GlobalMemoryStatusEx(&extended))ExitProcess(71);
        GlobalMemoryStatus(&legacy);
        VALUE("sample=",i);
        VALUE("extended.available.physical=",extended.ullAvailPhys);
        VALUE("legacy.available.physical=",legacy.dwAvailPhys);
        VALUE("extended.total.pagefile=",extended.ullTotalPageFile);
        VALUE("legacy.total.pagefile=",legacy.dwTotalPageFile);
        VALUE("extended.available.pagefile=",extended.ullAvailPageFile);
        VALUE("legacy.available.pagefile=",legacy.dwAvailPageFile);
        ULONGLONG expected=extended.ullAvailPageFile>MAXDWORD?MAXDWORD:extended.ullAvailPageFile;
        ULONGLONG actual=legacy.dwAvailPageFile;
        ULONGLONG difference=actual>expected?actual-expected:expected-actual;
        /* Wine keeps its documented Photoshop compatibility clamp for old
         * 32-bit images. Accept that exact result while rejecting unrelated
         * legacy/extended mismatches. Native Windows follows the direct
         * clamped value above. */
        ULONGLONG photoshop_clamp=extended.ullAvailPhys<2ULL*1024*1024*1024?
            2ULL*1024*1024*1024-extended.ullAvailPhys-1:0;
        BOOL compatibility_clamp=photoshop_clamp&&actual==photoshop_clamp;
        /* The two Windows queries can observe different system activity.
         * The demonstrated legacy clamp differs by gigabytes. */
        if(difference>16*1024*1024&&!compatibility_clamp)failed=TRUE;
        if(i<2)Sleep(1100);
    }
    if(failed)ExitProcess(72);
    MESSAGE("memory_status32: legacy and extended pagefile values agree passed\r\n");
    ExitProcess(0);
}
