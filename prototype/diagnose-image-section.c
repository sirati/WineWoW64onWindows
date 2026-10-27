/* Test helper: measure SEC_IMAGE relocation behavior without a guest loader. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
typedef LONG (WINAPI *create_t)(HANDLE *,DWORD,void *,LARGE_INTEGER *,ULONG,ULONG,HANDLE);
typedef LONG (WINAPI *map_t)(HANDLE,HANDLE,void **,ULONG_PTR,SIZE_T,LARGE_INTEGER *,SIZE_T *,int,ULONG,ULONG);
typedef LONG (WINAPI *unmap_t)(HANDLE,void *);
static BYTE *raw;
static IMAGE_NT_HEADERS32 *nt;
static void *file_rva(DWORD rva)
{
    IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++)
        if(rva>=sections[i].VirtualAddress&&rva-sections[i].VirtualAddress<sections[i].SizeOfRawData)
            return raw+sections[i].PointerToRawData+rva-sections[i].VirtualAddress;
    return NULL;
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    FILE *input=fopen(argv[1],"rb");if(!input)return 3;
    fseek(input,0,SEEK_END);long length=ftell(input);rewind(input);
    raw=malloc(length);if(!raw||fread(raw,1,length,input)!=(SIZE_T)length)return 4;fclose(input);
    nt=(void *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC)return 5;
    IMAGE_DATA_DIRECTORY directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    BYTE *at=file_rva(directory.VirtualAddress),*end=at+directory.Size;DWORD target=0;
    while(at&&at<end&&!target)
    {
        IMAGE_BASE_RELOCATION *block=(void *)at;
        if(block->SizeOfBlock<8||at+block->SizeOfBlock>end)return 6;
        WORD *entries=(void *)(block+1);
        for(unsigned i=0;i<(block->SizeOfBlock-8)/2;i++)
            if(entries[i]>>12==IMAGE_REL_BASED_HIGHLOW){target=block->VirtualAddress+(entries[i]&4095);break;}
        at+=block->SizeOfBlock;
    }
    if(!target||!file_rva(target))return 7;
    DWORD original=*(DWORD *)file_rva(target);
    HMODULE native=GetModuleHandleA("ntdll.dll");
    create_t create=(create_t)GetProcAddress(native,"NtCreateSection");
    map_t map=(map_t)GetProcAddress(native,"NtMapViewOfSection");
    unmap_t unmap=(unmap_t)GetProcAddress(native,"NtUnmapViewOfSection");
    HANDLE file=CreateFileA(argv[1],GENERIC_READ|GENERIC_EXECUTE,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL),section=NULL;
    if(file==INVALID_HANDLE_VALUE)return 8;
    LONG status=create(&section,SECTION_MAP_READ|SECTION_MAP_EXECUTE,NULL,NULL,PAGE_EXECUTE_READ,SEC_IMAGE,file);
    CloseHandle(file);if(status<0)return 9;
    unsigned failures=0;
    const ULONG_PTR requests[]={0,0,nt->OptionalHeader.ImageBase,0x21000000};
    for(unsigned i=0;i<sizeof(requests)/sizeof(requests[0]);i++)
    {
        void *base=(void *)requests[i];SIZE_T size=0;ULONG_PTR zero_bits=i==1?32:0;
        status=map(section,GetCurrentProcess(),&base,zero_bits,0,NULL,&size,2,0,PAGE_READONLY);
        if(status<0)
        {
            printf("IMAGE_MAP requested=%p zero_bits=%llu status=%08lx\n",(void *)requests[i],(unsigned long long)zero_bits,status);
            /* ZeroBits=32 is an optional probe, rejected by native Windows. */
            if(!zero_bits||(ULONG)status!=0xc000000d)failures++;
            continue;
        }
        IMAGE_NT_HEADERS32 *mapped_nt=(void *)((BYTE *)base+((IMAGE_DOS_HEADER *)base)->e_lfanew);
        DWORD mapped_preferred=mapped_nt->OptionalHeader.ImageBase;
        DWORD mapped=*(DWORD *)((BYTE *)base+target);
        DWORD relocated=original+(DWORD)(ULONG_PTR)base-nt->OptionalHeader.ImageBase;
        DWORD canonical=original+mapped_preferred-nt->OptionalHeader.ImageBase;
        MEMORY_BASIC_INFORMATION info={0};VirtualQuery((BYTE *)base+target,&info,sizeof(info));
        printf("IMAGE_MAP requested=%p zero_bits=%llu base=%p status=%08lx raw_header=%08lx mapped_header=%08lx rva=%08lx raw=%08lx mapped=%08lx relocated=%08lx canonical=%08lx type=%08lx protection=%08lx kind=%s\n",
               (void *)requests[i],(unsigned long long)zero_bits,base,status,nt->OptionalHeader.ImageBase,mapped_preferred,target,original,mapped,relocated,canonical,info.Type,info.Protect,
               mapped==relocated?"relocated":mapped==canonical?"canonical":mapped==original?"raw":"unexpected");
        if(mapped!=original&&mapped!=relocated&&mapped!=canonical)failures++;
        unmap(GetCurrentProcess(),base);
    }
    CloseHandle(section);free(raw);return failures?11:0;
}
